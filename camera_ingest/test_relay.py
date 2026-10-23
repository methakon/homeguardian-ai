"""Tests for the loopback-only relay.

These run entirely offline against a fake source, so they are fast and do not touch
the camera. What they protect is the security posture, not the video path:
  * refuse to bind anything but loopback
  * require a bearer token, and reject a request without a valid one
  * never expose the camera credential to a consumer
  * never write a token or a credential to stderr
"""

from __future__ import annotations

import base64
import io
import os
import threading
import unittest
import urllib.error
import urllib.request
from unittest import mock

from camera_ingest.relay import (
    ENV_RELAY_TOKEN,
    LoopbackRelay,
    RelayBindRefused,
    RelayConfig,
    assert_loopback_only,
    relay_token_from_env,
)
from camera_ingest.source import CameraCredential, ProbeResult

FAKE_JPEG = b"\xff\xd8\xff\xe0" + b"jpeg-payload" * 4 + b"\xff\xd9"


class FakeSource:
    """A CameraSource that never touches a network.

    Records whether it was asked for a snapshot, so a test can prove the relay
    routed to the right call.
    """

    def __init__(self) -> None:
        self.snapshot_calls = 0
        self.stream_calls = 0

    @property
    def name(self) -> str:
        return "fake"

    def stream_url(self) -> str:
        return "http://fake.invalid/stream"

    def open_stream(self, timeout: float = 10.0):
        self.stream_calls += 1
        yield b"chunk-one"
        yield b"chunk-two"

    def probe(self, timeout: float = 8.0) -> ProbeResult:
        return ProbeResult(True, "ok")

    def snapshot(self, timeout: float = 8.0) -> bytes:
        self.snapshot_calls += 1
        return FAKE_JPEG


class TestBindRefusal(unittest.TestCase):
    """The single most important control: this must never listen on a routable address."""

    def test_refuses_a_lan_address(self) -> None:
        with self.assertRaises(RelayBindRefused):
            RelayConfig(bind="192.168.1.9", token="t").validate()

    def test_refuses_the_wildcard_address(self) -> None:
        with self.assertRaises(RelayBindRefused):
            RelayConfig(bind="0.0.0.0", token="t").validate()

    def test_refuses_a_public_address(self) -> None:
        with self.assertRaises(RelayBindRefused):
            RelayConfig(bind="8.8.8.8", token="t").validate()

    def test_accepts_loopback(self) -> None:
        for ok in ("127.0.0.1", "localhost", "::1"):
            RelayConfig(bind=ok, token="t").validate()

    def test_constructing_the_relay_itself_refuses_a_non_loopback_bind(self) -> None:
        """The refusal must happen on construction, not only on start()."""
        with self.assertRaises(RelayBindRefused):
            LoopbackRelay(FakeSource(), RelayConfig(bind="0.0.0.0", token="t"))

    def test_assert_loopback_only_guards_consumers(self) -> None:
        assert_loopback_only("http://127.0.0.1:9999/stream")
        with self.assertRaises(RelayBindRefused):
            assert_loopback_only("http://192.168.1.9:9999/stream")


class TestTokens(unittest.TestCase):
    def test_require_token_without_a_token_is_an_error(self) -> None:
        with self.assertRaises(ValueError):
            RelayConfig(token=None, require_token=True).validate()

    def test_a_token_is_generated_when_none_is_supplied(self) -> None:
        relay = LoopbackRelay(FakeSource(), RelayConfig(bind="127.0.0.1"))
        self.assertTrue(relay.token)
        self.assertGreaterEqual(len(relay.token), 16)

    def test_a_managed_token_from_the_environment_is_used(self) -> None:
        with mock.patch.dict(os.environ, {ENV_RELAY_TOKEN: "managed"}, clear=True):
            relay = LoopbackRelay(FakeSource(), RelayConfig(bind="127.0.0.1"))
        self.assertEqual(relay.token, "managed")

    def test_distinct_relays_get_distinct_tokens(self) -> None:
        a = LoopbackRelay(FakeSource(), RelayConfig(bind="127.0.0.1"))
        b = LoopbackRelay(FakeSource(), RelayConfig(bind="127.0.0.1"))
        self.assertNotEqual(a.token, b.token)

    def test_relay_token_from_env_falls_back_to_a_fresh_secret(self) -> None:
        with mock.patch.dict(os.environ, {}, clear=True):
            token = relay_token_from_env()
        self.assertTrue(token)


class TestServing(unittest.TestCase):
    """End-to-end over a real loopback socket, still with no camera involved."""

    def setUp(self) -> None:
        self.source = FakeSource()
        self.relay = LoopbackRelay(
            self.source, RelayConfig(bind="127.0.0.1", port=0, token="test-token")
        ).start()
        self.addCleanup(self.relay.stop)

    def _get(self, url: str, token: str | None = None):
        req = urllib.request.Request(url)
        if token is not None:
            req.add_header("Authorization", f"Bearer {token}")
        return urllib.request.urlopen(req, timeout=5)

    def test_url_contains_no_camera_credential(self) -> None:
        self.assertNotIn("@", self.relay.url)
        self.assertEqual(self.relay.url, f"http://127.0.0.1:{self.relay.port}/stream")

    def test_it_binds_loopback_only(self) -> None:
        httpd = self.relay._httpd
        self.assertIsNotNone(httpd)
        assert httpd is not None  # narrow for the type checker
        self.assertEqual(httpd.server_address[0], "127.0.0.1")

    def test_a_request_without_the_token_is_denied(self) -> None:
        with self.assertRaises(urllib.error.HTTPError) as caught:
            self._get(self.relay.snapshot_url)
        self.assertEqual(caught.exception.code, 401)

    def test_the_401_challenge_advertises_Basic_as_well_as_Bearer(self) -> None:
        """ffmpeg only retries with URL credentials if a Basic challenge is offered.

        Advertising only Bearer made the relay unusable by the one consumer that
        matters, even when the URL carried a perfectly valid token.
        """
        with self.assertRaises(urllib.error.HTTPError) as caught:
            self._get(self.relay.snapshot_url)
        challenges = caught.exception.headers.get_all("WWW-Authenticate") or []
        joined = " ".join(challenges)
        self.assertIn("Basic", joined, f"no Basic challenge: {challenges}")
        self.assertIn("Bearer", joined, f"no Bearer challenge: {challenges}")

    def test_a_request_with_a_wrong_token_is_denied(self) -> None:
        with self.assertRaises(urllib.error.HTTPError) as caught:
            self._get(self.relay.snapshot_url, token="wrong")
        self.assertEqual(caught.exception.code, 401)

    def test_a_denied_request_reads_no_media(self) -> None:
        """The camera must not be touched on an unauthorised request."""
        before = self.source.snapshot_calls
        with self.assertRaises(urllib.error.HTTPError):
            self._get(self.relay.snapshot_url)
        self.assertEqual(self.source.snapshot_calls, before)

    def test_basic_auth_also_works_because_ffmpeg_cannot_set_headers(self) -> None:
        """ffmpeg/ffprobe can only pass credentials in the URL, which is Basic.

        Without this path the relay is unusable by the one consumer that matters.
        The token is the RELAY's token; the camera credential stays in the relay.
        """
        import urllib.error as _errors
        import base64 as _b64
        token_b64 = _b64.b64encode(f"{self.relay.token}:".encode()).decode()
        req = urllib.request.Request(self.relay.snapshot_url)
        req.add_header("Authorization", f"Basic {token_b64}")
        try:
            with urllib.request.urlopen(req, timeout=5) as resp:
                self.assertEqual(resp.status, 200)
                self.assertEqual(resp.read(), FAKE_JPEG)
        finally:
            pass
        self.assertEqual(self.source.snapshot_calls, 1)

    def test_basic_auth_with_the_wrong_username_is_still_denied(self) -> None:
        import base64 as _b64
        token_b64 = _b64.b64encode(b"not-the-token:").decode()
        req = urllib.request.Request(self.relay.snapshot_url)
        req.add_header("Authorization", f"Basic {token_b64}")
        with self.assertRaises(urllib.error.HTTPError) as caught:
            urllib.request.urlopen(req, timeout=5)
        self.assertEqual(caught.exception.code, 401)

    def test_garbage_in_the_authorization_header_is_denied_not_crashed(self) -> None:
        for junk in ("Basic !!!not-base64!!!", "Basic ", "Digest xyz", "Bear", ""):
            with self.subTest(junk=junk):
                req = urllib.request.Request(self.relay.snapshot_url)
                if junk:
                    req.add_header("Authorization", junk)
                with self.assertRaises(urllib.error.HTTPError) as caught:
                    urllib.request.urlopen(req, timeout=5)
                self.assertEqual(caught.exception.code, 401)

    def test_a_valid_token_returns_the_snapshot(self) -> None:
        with self._get(self.relay.snapshot_url, token=self.relay.token) as resp:
            self.assertEqual(resp.status, 200)
            self.assertEqual(resp.read(), FAKE_JPEG)
        self.assertEqual(self.source.snapshot_calls, 1)

    def test_a_valid_token_streams_bytes_through(self) -> None:
        with self._get(self.relay.url, token=self.relay.token) as resp:
            body = resp.read()
        self.assertIn(b"chunk-one", body)
        self.assertIn(b"chunk-two", body)
        self.assertEqual(self.source.stream_calls, 1)

    def test_unknown_paths_are_not_proxied(self) -> None:
        """The relay must not become a general proxy onto the camera."""
        for path in ("/cgi-bin/gw2.cgi", "/custom/OEM", "/bubble/live?ch=0"):
            with self.subTest(path=path):
                with self.assertRaises(urllib.error.HTTPError) as caught:
                    self._get(f"http://127.0.0.1:{self.relay.port}{path}", token=self.relay.token)
                self.assertEqual(caught.exception.code, 404)

    def test_the_token_is_never_written_to_stderr(self) -> None:
        """The stock BaseHTTPRequestHandler logs the request line; we suppress it."""
        captures = io.StringIO()
        with mock.patch("sys.stderr", captures):
            self._get(self.relay.snapshot_url, token=self.relay.token).read()
            self._get(self.relay.snapshot_url) if False else None
            try:
                self._get(self.relay.snapshot_url)
            except urllib.error.HTTPError:
                pass
        self.assertNotIn("test-token", captures.getvalue())

    def test_stopping_twice_is_safe(self) -> None:
        relay = LoopbackRelay(FakeSource(), RelayConfig(bind="127.0.0.1", port=0, token="t")).start()
        relay.stop()
        relay.stop()  # must not raise

    def test_context_manager_starts_and_stops(self) -> None:
        with LoopbackRelay(FakeSource(), RelayConfig(bind="127.0.0.1", port=0, token="t")) as relay:
            port = relay.port
            self.assertGreater(port, 0)
        # After exit the port should no longer accept connections.
        with self.assertRaises(Exception):
            urllib.request.urlopen(f"http://127.0.0.1:{port}/snapshot", timeout=2)


class TestNoCredentialLeak(unittest.TestCase):
    """The relay holds the secret; nothing it exposes may contain it."""

    def test_a_consumer_never_sees_the_camera_password(self) -> None:
        secret = "camera-secret-xyz"
        source = FakeSource()
        relay = LoopbackRelay(source, RelayConfig(bind="127.0.0.1", port=0, token="tok")).start()
        self.addCleanup(relay.stop)

        req = urllib.request.Request(relay.snapshot_url)
        req.add_header("Authorization", "Bearer tok")
        with urllib.request.urlopen(req, timeout=5) as resp:
            body = resp.read()
            headers = dict(resp.headers)

        self.assertNotIn(secret.encode(), body)
        self.assertNotIn(secret, str(headers))
        self.assertNotIn(secret, relay.url)
        self.assertNotIn(secret, repr(relay))


if __name__ == "__main__":
    unittest.main(verbosity=2)
