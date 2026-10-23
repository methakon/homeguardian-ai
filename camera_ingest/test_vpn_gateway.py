"""Tests for the VPN-facing gateway.

Two layers:

  * Validation tests inject a fake interface map, so every refusal is exercised
    anywhere without needing a tunnel. These are the safety tests.
  * Serving tests bind a REAL private address that is NOT on the default-route
    interface (e.g. a docker bridge), and skip when the host has none. They prove
    the proxy and the auth actually work end to end.

Run:  python3 -m unittest camera_ingest.test_vpn_gateway -v
"""

from __future__ import annotations

import base64
import io
import ipaddress
import os
import socket
import threading
import unittest
import urllib.error
import urllib.request
from unittest import mock

from camera_ingest.relay import LoopbackRelay, RelayConfig
from camera_ingest.vpn_gateway import (
    ENV_ALLOWED_PEERS,
    ENV_GATEWAY_BIND,
    ENV_GATEWAY_TOKEN,
    ENV_RELAY_TOKEN,
    ENV_RELAY_URL,
    GatewayConfig,
    VpnBindRefused,
    VpnGateway,
    default_route_interface,
    gateway_token_from_env,
    interface_addresses,
)

FAKE_JPEG = b"\xff\xd8\xff\xe0" + b"payload" * 8 + b"\xff\xd9"

# A plausible tunnel layout, independent of this machine.
FAKE_INTERFACES = {
    "lo": ["127.0.0.1", "::1"],
    "wlp6s0": ["192.168.1.9"],       # the LAN, which also carries the default route
    "wg0": ["10.77.0.1"],            # the tunnel
}
FAKE_DEFAULT_IFACE = "wlp6s0"


class FakeSource:
    def __init__(self) -> None:
        self.snapshot_calls = 0

    @property
    def name(self) -> str:
        return "fake"

    def stream_url(self) -> str:
        return "http://fake.invalid/stream"

    def open_stream(self, timeout: float = 10.0):
        yield b"chunk-one"
        yield b"chunk-two"

    def probe(self, timeout: float = 8.0):
        from camera_ingest.source import ProbeResult

        return ProbeResult(True, "ok")

    def snapshot(self, timeout: float = 8.0) -> bytes:
        self.snapshot_calls += 1
        return FAKE_JPEG


class TestBindValidation(unittest.TestCase):
    """The core safety property: only a tunnel-side address may be bound."""

    def _cfg(self, bind: str, **kw) -> GatewayConfig:
        base: dict[str, object] = {
            "bind": bind, "port": 0, "token": "t",
            "relay_token": "relay-tok", "relay_url": "http://127.0.0.1:8791",
        }
        base.update(kw)
        return GatewayConfig(**base)  # type: ignore[arg-type]

    def _check(self, bind: str, **kw):
        return self._cfg(bind, **kw).validate(
            interfaces=FAKE_INTERFACES, default_iface=FAKE_DEFAULT_IFACE
        )

    def test_accepts_the_tunnel_address(self) -> None:
        self.assertEqual(self._check("10.77.0.1").bind, "10.77.0.1")

    def test_refuses_the_lan_address_even_though_it_is_private(self) -> None:
        """The decisive case: private is not enough, it must not be the routed interface."""
        with self.assertRaises(VpnBindRefused) as caught:
            self._check("192.168.1.9")
        self.assertIn("default route", str(caught.exception))

    def test_refuses_the_wildcard(self) -> None:
        for bad in ("0.0.0.0", "::"):
            with self.subTest(bind=bad):
                with self.assertRaises(VpnBindRefused):
                    self._check(bad)

    def test_refuses_the_lan_subnet_even_if_unassigned(self) -> None:
        """Adding a LAN-looking address later must not quietly become bindable."""
        with self.assertRaises(VpnBindRefused):
            self._check("192.168.1.50")

    def test_refuses_a_public_address(self) -> None:
        for bad in ("8.8.8.8", "1.1.1.1"):
            with self.subTest(bind=bad):
                with self.assertRaises(VpnBindRefused):
                    self._check(bad)

    def test_refuses_loopback_because_that_is_the_relays_job(self) -> None:
        with self.assertRaises(VpnBindRefused):
            self._check("127.0.0.1")

    def test_refuses_a_non_ip_bind(self) -> None:
        with self.assertRaises(VpnBindRefused):
            self._check("wg0")            # interface name, not an address
        with self.assertRaises(VpnBindRefused):
            self._check("localhost")

    def test_refuses_when_the_address_is_on_no_interface(self) -> None:
        with self.assertRaises(VpnBindRefused) as caught:
            self._check("10.99.0.1")
        self.assertIn("not assigned", str(caught.exception))

    def test_refuses_cgnat_style_tunnel_that_is_on_no_interface(self) -> None:
        with self.assertRaises(VpnBindRefused):
            self._check("100.64.0.1")

    def test_accepts_a_cgnat_address_when_the_tunnel_has_it(self) -> None:
        ifs = dict(FAKE_INTERFACES)
        ifs["wg0"] = ["100.64.0.1"]
        cfg = self._cfg("100.64.0.1")
        self.assertEqual(
            cfg.validate(interfaces=ifs, default_iface=FAKE_DEFAULT_IFACE).bind, "100.64.0.1"
        )

    def test_refuses_when_the_default_route_interface_is_unknown(self) -> None:
        """Unprovable ownership must fail closed, not fall open."""
        with self.assertRaises(VpnBindRefused) as caught:
            self._cfg("10.77.0.1").validate(interfaces=FAKE_INTERFACES, default_iface=None)
        self.assertIn("cannot determine", str(caught.exception))
        # And the sentinel itself must not be mistaken for an interface name.

    def test_refuses_when_interfaces_cannot_be_enumerated(self) -> None:
        with self.assertRaises(VpnBindRefused):
            self._cfg("10.77.0.1").validate(interfaces={}, default_iface="wlp6s0")

    def test_the_constructor_validates_so_start_cannot_bypass_it(self) -> None:
        with self.assertRaises(VpnBindRefused):
            VpnGateway(
                self._cfg("0.0.0.0"), interfaces=FAKE_INTERFACES, default_iface=FAKE_DEFAULT_IFACE
            )


class TestRelayUrlValidation(unittest.TestCase):
    def test_refuses_a_remote_relay(self) -> None:
        """A non-local relay would move the credential holder off this host."""
        with self.assertRaises(VpnBindRefused) as caught:
            GatewayConfig(
                bind="10.77.0.1", token="t", relay_token="rt",
                relay_url="http://10.0.0.9:8791"
            ).validate(interfaces=FAKE_INTERFACES, default_iface=FAKE_DEFAULT_IFACE)
        self.assertIn("loopback", str(caught.exception))

    def test_refuses_a_non_http_relay(self) -> None:
        with self.assertRaises(VpnBindRefused):
            GatewayConfig(
                bind="10.77.0.1", token="t", relay_token="rt",
                relay_url="https://127.0.0.1:8791"
            ).validate(interfaces=FAKE_INTERFACES, default_iface=FAKE_DEFAULT_IFACE)


class TestPeerAllowlist(unittest.TestCase):
    def _cfg(self, peers) -> GatewayConfig:
        return GatewayConfig(
            bind="10.77.0.1", token="t", relay_token="rt", allowed_peers=tuple(peers)
        ).validate(interfaces=FAKE_INTERFACES, default_iface=FAKE_DEFAULT_IFACE)

    def test_accepts_a_well_formed_allowlist(self) -> None:
        self.assertEqual(len(self._cfg(["10.77.0.2/32", "10.77.0.0/24"]).allowed_peers), 2)

    def test_refuses_a_default_route_allowlist(self) -> None:
        with self.assertRaises(VpnBindRefused) as caught:
            self._cfg(["0.0.0.0/0"])
        self.assertIn("0.0.0.0/0", str(caught.exception))

    def test_refuses_a_malformed_entry(self) -> None:
        with self.assertRaises(VpnBindRefused):
            self._cfg(["not-a-cidr"])


class TestTokenHandling(unittest.TestCase):
    def test_a_token_is_required_when_require_token_is_set(self) -> None:
        with self.assertRaises(ValueError):
            GatewayConfig(
                bind="10.77.0.1", token=None, relay_token="rt", require_token=True
            ).validate(
                interfaces=FAKE_INTERFACES, default_iface=FAKE_DEFAULT_IFACE
            )

    def test_env_token_is_used_when_present(self) -> None:
        with mock.patch.dict(os.environ, {ENV_GATEWAY_TOKEN: "gw-tok"}, clear=True):
            self.assertEqual(gateway_token_from_env(), "gw-tok")

    def test_a_fresh_token_is_generated_when_absent(self) -> None:
        with mock.patch.dict(os.environ, {}, clear=True):
            self.assertTrue(gateway_token_from_env())

    def test_from_env_reads_the_allowlist(self) -> None:
        env = {
            ENV_GATEWAY_BIND: "10.77.0.1",
            ENV_GATEWAY_TOKEN: "t",
            ENV_RELAY_URL: "http://127.0.0.1:8791",
            ENV_RELAY_TOKEN: "rt",
            ENV_ALLOWED_PEERS: "10.77.0.2/32, 10.77.0.3/32",
        }
        cfg = GatewayConfig.from_env(env)
        self.assertEqual(cfg.allowed_peers, ("10.77.0.2/32", "10.77.0.3/32"))


# --------------------------------------------------------------------------- #
# Serving tests: need a real private address off the default-route interface
# --------------------------------------------------------------------------- #


def _discover_bindable_tunnel_like_address() -> str | None:
    """A private, non-loopback address whose interface is not the default route."""
    try:
        ifaces = interface_addresses()
        def_iface = default_route_interface()
    except Exception:
        return None
    for name, addrs in ifaces.items():
        if name == "lo" or name == def_iface:
            continue
        for a in addrs:
            try:
                ip = ipaddress.ip_address(a)
            except ValueError:
                continue
            if ip.version == 4 and (ip.is_private or ip in ipaddress.ip_network("100.64.0.0/10")):
                return str(ip)
    return None


BIND_ADDR = _discover_bindable_tunnel_like_address()
SKIP_REASON = (
    "no private non-default-route address on this host "
    "(e.g. a docker bridge); cannot bind a real tunnel-side socket here"
)


@unittest.skipUnless(BIND_ADDR, SKIP_REASON)
class TestServing(unittest.TestCase):
    """End-to-end: a real gateway socket in front of a real loopback relay."""

    def setUp(self) -> None:
        self.source = FakeSource()
        self.relay = LoopbackRelay(
            self.source, RelayConfig(bind="127.0.0.1", port=0, token="relay-token")
        ).start()
        self.addCleanup(self.relay.stop)

        self.gateway = VpnGateway(
            GatewayConfig(
                bind=BIND_ADDR or "",   # type: ignore[arg-type]
                port=0,
                token="gw-token",
                relay_token="relay-token",
                relay_url=f"http://127.0.0.1:{self.relay.port}",
            )
        ).start()
        self.addCleanup(self.gateway.stop)

    def _get(self, path: str, token: str | None = None, auth: str | None = None):
        url = f"http://{self.gateway.address}:{self.gateway.port}{path}"
        req = urllib.request.Request(url)
        if token is not None:
            req.add_header("Authorization", f"Bearer {token}")
        if auth is not None:
            req.add_header("Authorization", auth)
        return urllib.request.urlopen(req, timeout=15)

    def test_it_binds_the_tunnel_address_not_the_wildcard(self) -> None:
        httpd = self.gateway._httpd
        self.assertIsNotNone(httpd)
        assert httpd is not None
        self.assertEqual(httpd.server_address[0], self.gateway.address)
        self.assertNotEqual(httpd.server_address[0], "0.0.0.0")

    def test_no_token_is_denied(self) -> None:
        with self.assertRaises(urllib.error.HTTPError) as caught:
            self._get("/snapshot")
        self.assertEqual(caught.exception.code, 401)

    def test_a_wrong_token_is_denied(self) -> None:
        with self.assertRaises(urllib.error.HTTPError) as caught:
            self._get("/snapshot", token="wrong")
        self.assertEqual(caught.exception.code, 401)

    def test_the_401_offers_BOTH_bearer_and_basic(self) -> None:
        with self.assertRaises(urllib.error.HTTPError) as caught:
            self._get("/snapshot")
        joined = " ".join(caught.exception.headers.get_all("WWW-Authenticate") or [])
        self.assertIn("Bearer", joined)
        self.assertIn("Basic", joined)

    def test_basic_auth_works_for_clients_that_cannot_set_headers(self) -> None:
        b64 = base64.b64encode(b"gw-token:").decode()
        with self._get("/snapshot", auth=f"Basic {b64}") as resp:
            self.assertEqual(resp.status, 200)
            self.assertEqual(resp.read(), FAKE_JPEG)

    def test_a_valid_token_proxies_the_snapshot(self) -> None:
        with self._get("/snapshot", token="gw-token") as resp:
            self.assertEqual(resp.status, 200)
            self.assertEqual(resp.read(), FAKE_JPEG)
        self.assertEqual(self.source.snapshot_calls, 1)

    def test_a_valid_token_proxies_the_stream(self) -> None:
        with self._get("/stream", token="gw-token") as resp:
            body = resp.read()
        self.assertIn(b"chunk-one", body)
        self.assertIn(b"chunk-two", body)

    def test_only_the_media_paths_exist(self) -> None:
        """A peer must not be able to use the gateway to reach camera config routes."""
        for path in ("/cgi-bin/gw2.cgi", "/custom/OEM", "/bubble/live?ch=0", "/", "/../etc/passwd"):
            with self.subTest(path=path):
                with self.assertRaises(urllib.error.HTTPError) as caught:
                    self._get(path, token="gw-token")
                self.assertEqual(caught.exception.code, 404)

    def test_the_gateway_token_is_never_logged(self) -> None:
        captures = io.StringIO()
        with mock.patch("sys.stderr", captures):
            self._get("/snapshot", token="gw-token").read()
            try:
                self._get("/snapshot")
            except urllib.error.HTTPError:
                pass
        self.assertNotIn("gw-token", captures.getvalue())

    def test_repr_exposes_no_secret(self) -> None:
        self.assertNotIn("gw-token", repr(self.gateway))

    def test_stopping_twice_is_safe(self) -> None:
        gw = VpnGateway(
            GatewayConfig(
                bind=BIND_ADDR or "", port=0, token="t2", relay_token="relay-token",
                relay_url=f"http://127.0.0.1:{self.relay.port}",
            )
        ).start()
        gw.stop()
        gw.stop()


@unittest.skipUnless(BIND_ADDR, SKIP_REASON)
class TestPeerAllowlistEnforced(unittest.TestCase):
    def test_a_peer_outside_the_allowlist_is_refused(self) -> None:
        """The gateway is reachable only from the tunnel peers we named."""
        source = FakeSource()
        relay = LoopbackRelay(source, RelayConfig(bind="127.0.0.1", port=0, token="rt")).start()
        self.addCleanup(relay.stop)
        # Allow a range that deliberately excludes 127.0.0.1, our test client.
        gateway = VpnGateway(
            GatewayConfig(
                bind=BIND_ADDR or "", port=0, token="gt", relay_token="rt",
                relay_url=f"http://127.0.0.1:{relay.port}",
                allowed_peers=("10.77.0.0/24",),
            )
        ).start()
        self.addCleanup(gateway.stop)

        req = urllib.request.Request(
            f"http://{gateway.address}:{gateway.port}/snapshot"
        )
        req.add_header("Authorization", "Bearer gt")
        with self.assertRaises(urllib.error.HTTPError) as caught:
            urllib.request.urlopen(req, timeout=10)
        self.assertEqual(caught.exception.code, 403)


if __name__ == "__main__":
    if not BIND_ADDR:
        print(f"SKIPPING serving tests: {SKIP_REASON}")
    else:
        print(f"serving tests will bind {BIND_ADDR}")
    unittest.main(verbosity=2)
