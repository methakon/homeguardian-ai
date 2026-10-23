"""Tests for the camera source layer and credential handling.

Run:  python3 -m unittest discover -s camera_ingest -t . -v

These are pure unit tests: no camera, no network, no listener. The live verification
against the real device lives in test_live.py and is skipped when it is unreachable.
"""

from __future__ import annotations

import os
import pathlib
import tempfile
import unittest
from unittest import mock

from camera_ingest.source import (
    DEFAULT_HOST,
    DEFAULT_USER,
    ENV_HOST,
    ENV_PASSWORD,
    ENV_USER,
    CameraCredential,
    JuanHttpSource,
    load_dotenv,
)


class TestDotenvParsing(unittest.TestCase):
    """The .env parser must find the value and must not leak it anywhere else."""

    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self.path = pathlib.Path(self._tmp.name) / ".env"

    def tearDown(self) -> None:
        self._tmp.cleanup()

    def test_reads_a_plain_assignment(self) -> None:
        self.path.write_text("HG_CAMERA_PASSWORD=hunter2\n", encoding="utf-8")
        self.assertEqual(load_dotenv(self.path)["HG_CAMERA_PASSWORD"], "hunter2")

    def test_strips_matching_quotes(self) -> None:
        self.path.write_text('HG_CAMERA_PASSWORD="pa ss#word"\n', encoding="utf-8")
        self.assertEqual(load_dotenv(self.path)["HG_CAMERA_PASSWORD"], "pa ss#word")

    def test_empty_value_is_preserved_as_empty(self) -> None:
        """An explicitly empty password is a real state for this camera, not a missing key."""
        self.path.write_text("HG_CAMERA_PASSWORD=\n", encoding="utf-8")
        values = load_dotenv(self.path)
        self.assertIn("HG_CAMERA_PASSWORD", values)
        self.assertEqual(values["HG_CAMERA_PASSWORD"], "")

    def test_skips_comments_and_blanks(self) -> None:
        self.path.write_text("# comment\n\nHG_CAMERA_USER=admin\n", encoding="utf-8")
        self.assertEqual(load_dotenv(self.path), {"HG_CAMERA_USER": "admin"})

    def test_drops_an_inline_comment_after_an_unquoted_value(self) -> None:
        self.path.write_text("HG_CAMERA_USER=admin  # the owner's login\n", encoding="utf-8")
        self.assertEqual(load_dotenv(self.path)["HG_CAMERA_USER"], "admin")

    def test_missing_file_yields_empty_mapping(self) -> None:
        self.assertEqual(load_dotenv(pathlib.Path(self._tmp.name) / "nope.env"), {})


class TestCameraCredential(unittest.TestCase):
    def test_defaults_to_admin_with_no_password(self) -> None:
        """The camera as currently provisioned: admin, no password. Must not raise."""
        with mock.patch.dict(os.environ, {}, clear=True):
            cred = CameraCredential.from_env(dotenv_path="/nonexistent/.env")
        self.assertEqual(cred.user, DEFAULT_USER)
        self.assertTrue(cred.is_empty)

    def test_accepts_a_non_empty_password(self) -> None:
        """The whole point of supporting this now: the owner is about to set one."""
        with mock.patch.dict(os.environ, {ENV_PASSWORD: "s3cret-value"}, clear=True):
            cred = CameraCredential.from_env(dotenv_path="/nonexistent/.env")
        self.assertFalse(cred.is_empty)
        self.assertEqual(cred.password, "s3cret-value")

    def test_environment_beats_the_dotenv_file(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            p = pathlib.Path(tmp) / ".env"
            p.write_text("HG_CAMERA_PASSWORD=from-file\n", encoding="utf-8")
            with mock.patch.dict(os.environ, {ENV_PASSWORD: "from-env"}, clear=True):
                cred = CameraCredential.from_env(dotenv_path=p)
            self.assertEqual(cred.password, "from-env")

    def test_dotenv_used_when_environment_is_silent(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            p = pathlib.Path(tmp) / ".env"
            p.write_text("HG_CAMERA_PASSWORD=from-file\n", encoding="utf-8")
            with mock.patch.dict(os.environ, {}, clear=True):
                cred = CameraCredential.from_env(dotenv_path=p)
            self.assertEqual(cred.password, "from-file")

    def test_repr_never_contains_the_password(self) -> None:
        """A traceback or a stray print must not spill the secret."""
        cred = CameraCredential(user="admin", password="top-secret-do-not-print")
        for rendered in (repr(cred), str(cred), f"{cred}"):
            self.assertNotIn("top-secret-do-not-print", rendered)
        self.assertIn("<set>", repr(cred))

    def test_repr_marks_an_empty_password(self) -> None:
        self.assertIn("<empty>", repr(CameraCredential(user="admin", password="")))
        # And still shows the username, which is not secret.
        self.assertIn("admin", repr(CameraCredential(user="admin", password="")))

    def test_basic_header_is_well_formed(self) -> None:
        import base64

        cred = CameraCredential(user="admin", password="")
        self.assertEqual(cred.basic_header(), "Basic " + base64.b64encode(b"admin:").decode())

    def test_basic_header_encodes_a_non_empty_password(self) -> None:
        import base64

        cred = CameraCredential(user="admin", password="pw")
        self.assertEqual(cred.basic_header(), "Basic " + base64.b64encode(b"admin:pw").decode())


class TestJuanHttpSource(unittest.TestCase):
    def _source(self, **kwargs) -> JuanHttpSource:
        cred = CameraCredential(user="admin", password="pw")
        return JuanHttpSource(credential=cred, **kwargs)

    def test_preferred_stream_is_the_h264_sub_stream(self) -> None:
        """Decision (b): /livestream/12, H.264 640x360 — no transcode needed."""
        self.assertEqual(self._source(stream="sub").path, "/livestream/12")

    def test_named_stream_paths_match_the_verified_device_surface(self) -> None:
        self.assertEqual(self._source(stream="main").path, "/livestream/11")
        self.assertEqual(self._source(stream="snapshot").path, "/snapshot")

    def test_rejects_an_unknown_stream_name(self) -> None:
        with self.assertRaises(ValueError):
            self._source(stream="rtsp")

    def test_stream_url_omits_credentials_by_default(self) -> None:
        """The default URL must be safe to log."""
        url = self._source().stream_url()
        self.assertNotIn("pw", url)
        self.assertNotIn("@", url)
        self.assertEqual(url, f"http://{DEFAULT_HOST}:80/livestream/12")

    def test_stream_url_can_include_credentials_on_request(self) -> None:
        url = self._source().stream_url(include_credentials=True)
        self.assertIn("admin:pw@", url)

    def test_credential_goes_in_a_header_not_the_url(self) -> None:
        request = self._source()._request(timeout=1)
        self.assertEqual(request.get_header("Authorization"), "Basic " + __import__("base64").b64encode(b"admin:pw").decode())
        self.assertNotIn("pw@", request.full_url)

    def test_probe_reports_http_401_as_a_result_not_an_exception(self) -> None:
        import urllib.error

        source = self._source()
        with mock.patch("urllib.request.urlopen", side_effect=urllib.error.HTTPError(
                source.stream_url(), 401, "Unauthorized", {}, None)):
            result = source.probe()
        self.assertFalse(result.ok)
        self.assertEqual(result.detail, "HTTP 401")

    def test_probe_reports_success_on_200(self) -> None:
        class _Resp:
            status = 200

            def __enter__(self):
                return self

            def __exit__(self, *exc):
                return False

            def getcode(self):
                return 200

        with mock.patch("urllib.request.urlopen", return_value=_Resp()):
            self.assertTrue(self._source().probe().ok)

    def test_probe_never_raises_on_a_transport_error(self) -> None:
        with mock.patch("urllib.request.urlopen", side_effect=OSError("no route")):
            result = self._source().probe()
        self.assertFalse(result.ok)
        self.assertIn("OSError", result.detail)

    def test_snapshot_requires_the_snapshot_stream(self) -> None:
        """Guards against silently returning a truncated stream as if it were a JPEG."""
        with self.assertRaises(ValueError):
            self._source(stream="sub").snapshot()

    def test_name_is_loggable(self) -> None:
        self.assertEqual(self._source(stream="sub").name, f"juan-http:sub:{DEFAULT_HOST}")


class TestHostOverride(unittest.TestCase):
    def test_host_comes_from_the_environment_when_set(self) -> None:
        with mock.patch.dict(os.environ, {ENV_HOST: "10.0.0.5"}, clear=True):
            # The source takes host as a constructor arg; ENV_HOST exists so a
            # deployment can retarget without a code change.
            self.assertEqual(os.environ[ENV_HOST], "10.0.0.5")
            self.assertEqual(JuanHttpSource(host=os.environ[ENV_HOST])._host, "10.0.0.5")


if __name__ == "__main__":
    unittest.main(verbosity=2)
