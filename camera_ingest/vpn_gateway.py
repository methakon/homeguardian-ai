"""VPN-facing camera gateway: the second hop that makes the camera reachable off-LAN.

Architecture (two hops, deliberately):

    camera (LAN, holds no secret)
        |
        |  the ONLY holder of the camera credential
        v
    loopback relay on 127.0.0.1        camera_ingest/relay.py
        |
        |  the ONLY thing that talks to the relay, using the RELAY token
        v
    this gateway, bound to the TUNNEL address only
        |
        |  the remote peer holds the GATEWAY token, a different secret
        v
    remote consumer (phone on the VPN)

Why a second hop instead of binding the relay to the tunnel directly:

  * The relay holds the camera password. Keeping it on loopback means a
    compromise of a VPN peer, or a routing mistake, cannot reach the credential
    holder. The gateway holds only the relay token, which is revocable and grants
    no camera access by itself.
  * The camera daemon is fragile: one malformed request can kill HTTP, then ICMP,
    then every port, requiring a physical power cycle. A narrow, authenticated,
    allowlisted gateway is far less likely to be poked into that state than a
    directly-exposed relay.

Why the bind is validated so aggressively:

  * Binding 0.0.0.0 would publish the camera on the LAN and (depending on routing)
    anywhere else. That is the one outcome this module exists to prevent.
  * The rule enforced here is mechanical, not a convention: the bind address must
    be a private/CGNAT address assigned to a local interface that is NOT the
    interface carrying the default route. So a LAN address, a public address, a
    wildcard, or a loopback address are all refused, by construction, before any
    socket is created.

Video path note: this module never introduces a relay through any third party. It
forwards over the tunnel, and the tunnel is expected to be peer-to-peer. Nothing
here is designed to carry media through a shared VPS; doing so would violate the
standing constraint that a cloud host must not carry continuous camera video.
"""

from __future__ import annotations

import base64
import hmac
import http.server
import ipaddress
import json
import logging
import secrets
import subprocess
import threading
import urllib.error
import urllib.parse
import urllib.request
from dataclasses import dataclass, field
from typing import Iterable, Sequence

LOG = logging.getLogger("camera_ingest.vpn_gateway")

ENV_GATEWAY_TOKEN = "HG_CAMERA_GATEWAY_TOKEN"
ENV_GATEWAY_BIND = "HG_CAMERA_GATEWAY_BIND"
ENV_RELAY_URL = "HG_CAMERA_RELAY_URL"
ENV_RELAY_TOKEN = "HG_CAMERA_RELAY_TOKEN"
ENV_ALLOWED_PEERS = "HG_CAMERA_ALLOWED_PEERS"

SERVED_PATHS = ("/stream", "/snapshot")


# Sentinel: omitting the argument means "detect it"; passing None asserts there is
# none, which must fail closed rather than silently falling back to detection.
_AUTO = object()


class VpnBindRefused(RuntimeError):
    """The requested bind would expose the camera beyond the tunnel."""


# --------------------------------------------------------------------------- #
# Host facts (injectable so tests never depend on this machine's layout)
# --------------------------------------------------------------------------- #


def default_route_interface() -> str | None:
    """Name of the interface carrying the default route, or None if unknown."""
    try:
        out = subprocess.run(
            ["ip", "-j", "route", "show", "default"],
            capture_output=True, text=True, timeout=8, check=False,
        )
        if out.returncode == 0 and out.stdout.strip():
            routes = json.loads(out.stdout)
            if routes:
                return routes[0].get("dev")
    except Exception:
        pass
    return None


def interface_addresses() -> dict[str, list[str]]:
    """{interface_name: [address, ...]} for every local interface."""
    result: dict[str, list[str]] = {}
    try:
        out = subprocess.run(
            ["ip", "-j", "addr", "show"],
            capture_output=True, text=True, timeout=8, check=False,
        )
        if out.returncode == 0 and out.stdout.strip():
            for link in json.loads(out.stdout):
                name = link.get("ifname", "")
                addrs = [
                    a.get("local")
                    for a in link.get("addr_info", [])
                    if a.get("local")
                ]
                if name:
                    result[name] = addrs
    except Exception:
        pass
    return result


# --------------------------------------------------------------------------- #
# Configuration
# --------------------------------------------------------------------------- #


@dataclass
class GatewayConfig:
    """Everything the gateway needs, validated before a socket exists."""

    bind: str
    port: int = 0
    token: str | None = None
    # The token the gateway presents to the RELAY. This is a different secret from
    # `token` (which the remote peer presents to us). Using one for both made the
    # relay correctly answer 401 and the gateway faithfully pass that through.
    relay_token: str | None = None
    relay_url: str = "http://127.0.0.1:8791"
    allowed_peers: tuple[str, ...] = ()
    require_token: bool = True
    max_peers: int = 8

    def validate(
        self,
        interfaces: dict[str, list[str]] | None = None,
        default_iface: object = _AUTO,
    ) -> "GatewayConfig":
        if self.require_token and not self.token:
            raise ValueError("a gateway token is required when require_token is set")

        if not self.relay_token:
            raise ValueError(
                "a relay_token is required: the relay authenticates its callers, and "
                "the gateway is one of them"
            )

        if not 0 <= self.port <= 65535:
            raise ValueError(f"port out of range: {self.port}")

        # The relay must be local. A remote relay URL would mean the camera
        # credential lives somewhere else and this gateway is pointless.
        parsed = urllib.parse.urlsplit(self.relay_url)
        if parsed.scheme != "http":
            raise VpnBindRefused(f"relay_url must be http, got {parsed.scheme!r}")
        relay_host = parsed.hostname or ""
        if relay_host not in ("127.0.0.1", "localhost", "::1"):
            raise VpnBindRefused(
                f"relay_url must be loopback; the credential holder is local, got {relay_host!r}"
            )

        try:
            addr = ipaddress.ip_address(self.bind)
        except ValueError as exc:
            raise VpnBindRefused(f"bind is not an IP literal: {self.bind!r}") from exc

        # A wildcard bind is the failure mode this module exists to prevent.
        if addr.is_unspecified:
            raise VpnBindRefused("refusing a wildcard bind: that publishes the camera")

        # Loopback belongs to the relay, not here.
        if addr.is_loopback:
            raise VpnBindRefused(
                f"refusing a loopback bind ({self.bind}); the loopback hop is the relay"
            )

        # Public addresses are never acceptable for a camera feed.
        if addr.is_global:
            raise VpnBindRefused(f"refusing a publicly routable bind: {self.bind}")

        # Private/CGNAT only (RFC1918 and 100.64/10, which WireGuard commonly uses).
        if not (addr.is_private or addr in ipaddress.ip_network("100.64.0.0/10")):
            raise VpnBindRefused(
                f"bind {self.bind} is neither RFC1918 nor CGNAT; refusing"
            )

        # The decisive check: the address must belong to an interface that is NOT
        # the one carrying the default route. That is what makes "tunnel only"
        # mechanical rather than a convention someone can forget.
        ifs = interfaces if interfaces is not None else interface_addresses()
        if not ifs:
            raise VpnBindRefused(
                "cannot enumerate local interfaces; refusing to bind without proving "
                "the address is tunnel-side"
            )

        holder = None
        for name, addrs in ifs.items():
            if self.bind in addrs:
                holder = name
                break

        if holder is None:
            raise VpnBindRefused(
                f"bind {self.bind} is not assigned to any local interface; "
                "bring the tunnel up first"
            )

        # Resolve the default-route interface if the caller did not tell us.
        def_iface = default_route_interface() if default_iface is _AUTO else default_iface
        if def_iface is None:
            raise VpnBindRefused(
                "cannot determine the default-route interface; refusing to bind "
                "because tunnel-side ownership is unproven"
            )
        if holder == def_iface:
            raise VpnBindRefused(
                f"bind {self.bind} lives on {holder}, which carries the default route; "
                "that is the LAN/WAN path, not the tunnel. Refusing."
            )

        # Peer allowlist, when supplied, must be parseable and must not include the
        # whole internet.
        for entry in self.allowed_peers:
            try:
                net = ipaddress.ip_network(entry, strict=False)
            except ValueError as exc:
                raise VpnBindRefused(f"bad allowed_peers entry {entry!r}") from exc
            if net.prefixlen == 0:
                raise VpnBindRefused("allowed_peers must not be 0.0.0.0/0")

        return self

    @classmethod
    def from_env(cls, environ: dict[str, str] | None = None) -> "GatewayConfig":
        import os

        env = environ if environ is not None else os.environ
        peers = tuple(
            p.strip()
            for p in env.get(ENV_ALLOWED_PEERS, "").split(",")
            if p.strip()
        )
        return cls(
            bind=env.get(ENV_GATEWAY_BIND, ""),
            token=env.get(ENV_GATEWAY_TOKEN) or None,
            relay_token=env.get(ENV_RELAY_TOKEN) or None,
            relay_url=env.get(ENV_RELAY_URL, "http://127.0.0.1:8791"),
            allowed_peers=peers,
        )


def gateway_token_from_env(environ: dict[str, str] | None = None) -> str:
    import os

    env = environ if environ is not None else os.environ
    supplied = env.get(ENV_GATEWAY_TOKEN)
    if supplied:
        return supplied
    return secrets.token_urlsafe(32)


# --------------------------------------------------------------------------- #
# Server
# --------------------------------------------------------------------------- #


@dataclass
class _Runtime:
    config: GatewayConfig
    token: str          # what the remote peer must present
    relay_token: str    # what we present to the relay
    httpd: http.server.ThreadingHTTPServer | None = None
    thread: threading.Thread | None = None


class _GatewayHandler(http.server.BaseHTTPRequestHandler):
    """Authenticated, path-limited reverse proxy onto the loopback relay."""

    server_version = "hg-camera-gateway"
    protocol_version = "HTTP/1.1"
    runtime: _Runtime  # injected by the server factory

    # Never log the request line: it can carry a token in some client forms.
    def log_message(self, format: str, *args) -> None:  # noqa: A002
        return

    # -- auth ------------------------------------------------------------- #

    def _peer_allowed(self) -> bool:
        allowed = self.runtime.config.allowed_peers
        if not allowed:
            return True
        peer = self.client_address[0]
        try:
            addr = ipaddress.ip_address(peer)
        except ValueError:
            return False
        for entry in allowed:
            if addr in ipaddress.ip_network(entry, strict=False):
                return True
        return False

    def _authorised(self) -> bool:
        if not self.runtime.config.require_token:
            return True
        supplied = self.headers.get("Authorization", "")
        token = self.runtime.token

        if supplied.startswith("Bearer "):
            return hmac.compare_digest(supplied[len("Bearer "):], token)

        if supplied.startswith("Basic "):
            try:
                decoded = base64.b64decode(supplied[len("Basic "):]).decode("utf-8", "replace")
            except Exception:
                return False
            user, _, password = decoded.partition(":")
            return hmac.compare_digest(user, token) or hmac.compare_digest(password, token)

        return False

    def _challenge(self, code: int, message: str) -> None:
        body = (message + "\n").encode()
        self.send_response(code)
        self.send_header("Content-Type", "text/plain")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("WWW-Authenticate", 'Bearer realm="hg-camera-gateway"')
        self.send_header("WWW-Authenticate", 'Basic realm="hg-camera-gateway", charset="UTF-8"')
        self.end_headers()
        self.wfile.write(body)

    # -- proxying --------------------------------------------------------- #

    def do_GET(self) -> None:  # noqa: N802 - stdlib naming
        if not self._peer_allowed():
            LOG.warning("gateway: refused a connection from outside the peer allowlist")
            self._challenge(403, "forbidden")
            return

        if not self._authorised():
            self._challenge(401, "unauthorised")
            return

        path = self.path.split("?", 1)[0]
        if path not in SERVED_PATHS:
            # There is no general proxy here. Only the two media paths exist, so a
            # peer cannot use the gateway to reach camera config routes.
            self.send_error(404, "not found")
            return

        upstream = self.runtime.config.relay_url.rstrip("/") + path
        request = urllib.request.Request(upstream)
        # Present the RELAY's token upstream. The peer never sees this value, and
        # the peer's own token is never forwarded - the gateway is the boundary.
        request.add_header("Authorization", f"Bearer {self.runtime.relay_token}")

        try:
            with urllib.request.urlopen(request, timeout=30) as upstream_response:
                self.send_response(upstream_response.status)
                ctype = upstream_response.headers.get("Content-Type", "application/octet-stream")
                self.send_header("Content-Type", ctype)
                self.send_header("Cache-Control", "no-store")
                # Chunked: the stream has no length known in advance.
                self.send_header("Transfer-Encoding", "chunked")
                self.end_headers()
                while True:
                    chunk = upstream_response.read(64 * 1024)
                    if not chunk:
                        break
                    self.wfile.write(b"%x\r\n" % len(chunk) + chunk + b"\r\n")
                    self.wfile.flush()
                self.wfile.write(b"0\r\n\r\n")
        except urllib.error.HTTPError as exc:
            self._challenge(exc.code, f"relay returned {exc.code}")
        except Exception as exc:  # noqa: BLE001 - report, never leak internals
            LOG.warning("gateway: upstream failure: %s", type(exc).__name__)
            self._challenge(502, "relay unavailable")


class VpnGateway:
    """Binds the tunnel address and forwards authenticated peers to the relay."""

    def __init__(self, config: GatewayConfig, *, interfaces=None, default_iface=_AUTO) -> None:
        # Validate in the constructor so a bad bind cannot reach the wire through a
        # code path that forgot to call start().
        config.validate(interfaces=interfaces, default_iface=default_iface)
        self.config = config
        self.token = config.token or gateway_token_from_env()
        self._runtime = _Runtime(
            config=config,
            token=self.token,
            relay_token=config.relay_token or "",
        )
        self._httpd: http.server.ThreadingHTTPServer | None = None
        self._thread: threading.Thread | None = None

    @property
    def address(self) -> str:
        return self.config.bind

    @property
    def port(self) -> int:
        if self._httpd is None:
            return self.config.port
        return int(self._httpd.server_address[1])

    def start(self) -> "VpnGateway":
        handler = type("_BoundGatewayHandler", (_GatewayHandler,), {"runtime": self._runtime})
        self._httpd = http.server.ThreadingHTTPServer((self.config.bind, self.config.port), handler)
        self._runtime.httpd = self._httpd
        self._thread = threading.Thread(
            target=self._httpd.serve_forever, name="hg-camera-gateway", daemon=True
        )
        self._thread.start()
        LOG.info("camera gateway listening on the tunnel address only")
        return self

    def stop(self) -> None:
        if self._httpd is not None:
            try:
                self._httpd.shutdown()
            except Exception:
                pass
            try:
                self._httpd.server_close()
            except Exception:
                pass
            self._httpd = None
        if self._thread is not None:
            self._thread.join(timeout=5)
            self._thread = None

    def __enter__(self) -> "VpnGateway":
        return self.start()

    def __exit__(self, *exc) -> bool:
        self.stop()
        return False

    def __repr__(self) -> str:
        return f"<VpnGateway bind={self.address} port={self.port} peers={len(self.config.allowed_peers)}>"
