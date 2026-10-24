"""Loopback-only relay that keeps the camera credential out of every consumer.

Why this exists: the camera needs `Authorization: Basic ...` on every request. If a
consumer (an NVR, a browser, ffmpeg) is handed that credential, it ends up in argv,
in a config file, or in a log — all of which the constraints forbid. So exactly one
process holds the secret, and everything else talks to it over loopback.

Threat model and the two controls that answer it
------------------------------------------------
1. **Another local process must not be able to use the relay.** Binding to
   127.0.0.1 stops anything off-host, but any local process can still connect to a
   loopback port. So the relay REQUIRES a bearer token. The token is generated with
   `secrets.token_urlsafe` at startup and handed to the caller in memory, or read
   from `HG_CAMERA_RELAY_TOKEN` when one is managed externally. A request without
   the token gets 401 and no media.

2. **The credential must never be written down.** The credential is read once from
   the environment / repo-root .env (gitignored) and held in memory. It is never
   logged, never echoed, never placed in argv, and never included in a URL that is
   printed.

Binding rule enforced in code, not by convention: `RelayConfig.bind` is validated
to be a loopback address and the relay refuses to start otherwise. There is no
option to bind a public interface — that would be a different, unapproved change.

This module never decodes and never transcodes; it copies bytes from the camera to
the consumer, so its own CPU cost is negligible.
"""

from __future__ import annotations

import base64
import hmac
import http.server
import os
import secrets
import socket
import threading
import urllib.error
import urllib.request
from dataclasses import dataclass, field
from typing import Callable, Optional

from .recovery import (
    RecoveryWatcher,
    recovering_source_from_env,
    watch_enabled_from_env,
    watch_interval_from_env,
)
from .source import (
    DEFAULT_HOST,
    ENV_PASSWORD,
    ENV_USER,
    CameraCredential,
    CameraSource,
    JuanHttpSource,
    default_dotenv_path,
)

#: Environment variable holding an externally-managed relay token.
ENV_RELAY_TOKEN = "HG_CAMERA_RELAY_TOKEN"

#: Addresses we are willing to bind. Anything else is refused outright.
_LOOPBACK_NAMES = frozenset({"127.0.0.1", "localhost", "::1"})


class RelayBindRefused(RuntimeError):
    """Raised when asked to bind something other than loopback.

    Deliberately fatal: publishing this stream is a separate decision that has not
    been approved, so the failure mode must be "refuse to start", never "start
    anyway on a wider interface".
    """


@dataclass
class RelayConfig:
    bind: str = "127.0.0.1"
    port: int = 0  # 0 = let the OS pick a free loopback port
    token: Optional[str] = None
    require_token: bool = True
    stream: str = "sub"

    def validate(self) -> None:
        if self.bind not in _LOOPBACK_NAMES:
            raise RelayBindRefused(
                f"refusing to bind {self.bind!r}: only loopback is permitted "
                f"({sorted(_LOOPBACK_NAMES)})"
            )
        if self.require_token and not self.token:
            raise ValueError("require_token=True needs a token")


class _Handler(http.server.BaseHTTPRequestHandler):
    """Serves the camera's media to a local client, injecting the credential.

    The client's own URL carries NO credential: just the relay token.
    """

    # Injected by the server factory below.
    source: CameraSource
    token: str
    require_token: bool

    protocol_version = "HTTP/1.1"

    def log_message(self, format: str, *args) -> None:  # noqa: A002,A003
        """Suppress the default stderr access log.

        The stock handler writes the request line to stderr, which would put the
        request (and any token in a query string) into whatever captures stderr.
        Silence it; the caller can log its own summary if it wants one.
        """
        return

    def _authorised(self) -> bool:
        if not self.require_token:
            return True
        supplied = self.headers.get("Authorization", "")

        # Preferred: a bearer header, for callers that can set one.
        if supplied.startswith("Bearer "):
            return hmac.compare_digest(supplied[len("Bearer "):], self.token)

        # Also accepted: HTTP Basic, because ffmpeg/ffprobe cannot set headers
        # without putting them in argv. This lets them use
        #   http://<relay-token>@127.0.0.1:<port>/stream
        # The token here is the RELAY's own token, never the camera credential.
        if supplied.startswith("Basic "):
            try:
                decoded = base64.b64decode(supplied[len("Basic "):]).decode("utf-8", "replace")
            except Exception:
                return False
            user, _, password = decoded.partition(":")
            # Accept the token in either position, because the URL form an
            # external tool can build varies: ffmpeg needs a colon present
            # ("http://TOKEN:@host/") before it will send Basic at all, and
            # some clients put the token in the password field instead.
            return hmac.compare_digest(user, self.token) or hmac.compare_digest(password, self.token)

        return False

    def _deny(self) -> None:
        body = b"unauthorised\n"
        self.send_response(401)
        self.send_header("Content-Type", "text/plain")
        self.send_header("Content-Length", str(len(body)))
        # Advertise BOTH schemes. ffmpeg/ffprobe can only satisfy Basic, and they
        # will not retry with the credentials embedded in the URL unless a Basic
        # challenge is offered. Sending only "Bearer" made the relay unusable by
        # ffmpeg even though the URL carried a valid token. (RFC 7235 permits
        # multiple challenges; one header each is the clearest form.)
        self.send_header("WWW-Authenticate", 'Bearer realm="hg-camera-relay"')
        self.send_header("WWW-Authenticate", 'Basic realm="hg-camera-relay", charset="UTF-8"')
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self) -> None:  # noqa: N802 - stdlib naming
        if not self._authorised():
            self._deny()
            return

        # Only the media path is served. There is no passthrough of arbitrary
        # routes, so the relay cannot become a general proxy onto the camera.
        if self.path.split("?", 1)[0] not in ("/stream", "/snapshot"):
            self.send_error(404, "not found")
            return

        wants_snapshot = self.path.split("?", 1)[0] == "/snapshot"

        try:
            if wants_snapshot:
                jpeg = self.source.snapshot()  # type: ignore[attr-defined]
                self.send_response(200)
                self.send_header("Content-Type", "image/jpeg")
                self.send_header("Content-Length", str(len(jpeg)))
                self.end_headers()
                self.wfile.write(jpeg)
                return

            # Stream path: forward bytes as they arrive. Declared as an open-ended
            # octet stream because the vendor frames its own media.
            self.send_response(200)
            self.send_header("Content-Type", "application/octet-stream")
            self.send_header("Cache-Control", "no-store")
            self.end_headers()
            for chunk in self.source.open_stream():
                self.wfile.write(chunk)
        except (BrokenPipeError, ConnectionResetError):
            # The consumer went away. Normal for a stream; nothing to report.
            return
        except Exception:
            # Never surface an exception string: it can carry the request we made,
            # and therefore the credential. Code only.
            try:
                self.send_error(502, "upstream error")
            except Exception:
                pass


class LoopbackRelay:
    """Runs the relay on a background thread; usable as a context manager.

    Typical use::

        with LoopbackRelay(JuanHttpSource(stream="sub")) as relay:
            # hand relay.url + relay.token to a consumer; never the camera secret
            ...
    """

    def __init__(self, source: Optional[CameraSource] = None, config: Optional[RelayConfig] = None):
        if source is not None:
            self._source = source
        else:
            # Default to a MAC-anchored source. This is opt-in and inert unless a
            # MAC is enrolled, in which case a camera whose address moves repairs
            # itself and consumers never notice: they only ever talk to this
            # relay's own loopback address.
            self._source = recovering_source_from_env(stream="sub")
        self._config = config or RelayConfig()
        if self._config.token is None:
            self._config.token = os.environ.get(ENV_RELAY_TOKEN) or secrets.token_urlsafe(32)
        self._config.validate()

        self._httpd: Optional[http.server.ThreadingHTTPServer] = None
        self._thread: Optional[threading.Thread] = None
        self._watcher: Optional[RecoveryWatcher] = None

    # -- lifecycle -------------------------------------------------------- #

    def start(self) -> "LoopbackRelay":
        if self._httpd is not None:
            return self

        handler = type(
            "_BoundHandler",
            (_Handler,),
            {
                "source": self._source,
                "token": self._config.token,
                "require_token": self._config.require_token,
            },
        )
        # Bind explicitly to the validated loopback address.
        self._httpd = http.server.ThreadingHTTPServer((self._config.bind, self._config.port), handler)
        self._thread = threading.Thread(target=self._httpd.serve_forever, daemon=True)
        self._thread.start()

        # Autonomous recovery, if the owner has switched it on. This is what
        # notices a camera that dropped while nobody was watching; without it,
        # recovery only ever fires on a request.
        if watch_enabled_from_env():
            self._watcher = RecoveryWatcher(
                self._source, interval=watch_interval_from_env()
            ).start()
        return self

    def stop(self, timeout: float = 5.0) -> None:
        # Stop the heartbeat first: it talks to the camera, so it must not
        # outlive the relay that owns the source.
        if self._watcher is not None:
            self._watcher.stop()
            self._watcher = None
        if self._httpd is None:
            return
        self._httpd.shutdown()
        self._httpd.server_close()
        if self._thread is not None:
            self._thread.join(timeout=timeout)
        self._httpd = None
        self._thread = None

    def __enter__(self) -> "LoopbackRelay":
        return self.start()

    def __exit__(self, *exc) -> None:
        self.stop()

    # -- addressing ------------------------------------------------------- #

    @property
    def port(self) -> int:
        if self._httpd is None:
            raise RuntimeError("relay not started")
        return self._httpd.server_address[1]

    @property
    def token(self) -> str:
        return self._config.token or ""

    @property
    def url(self) -> str:
        """The stream URL a consumer may use. Carries NO camera credential."""
        return f"http://{self._config.bind}:{self.port}/stream"

    @property
    def snapshot_url(self) -> str:
        return f"http://{self._config.bind}:{self.port}/snapshot"

    def __repr__(self) -> str:  # pragma: no cover - trivial
        state = "running" if self._httpd is not None else "stopped"
        port = self._httpd.server_address[1] if self._httpd is not None else "-"
        return f"LoopbackRelay({state}, bind={self._config.bind}, port={port})"


def relay_token_from_env() -> str:
    """A managed token, or a fresh one. Never read from argv."""
    return os.environ.get(ENV_RELAY_TOKEN) or secrets.token_urlsafe(32)


def assert_loopback_only(url: str) -> None:
    """Guard for callers: refuse to consume a non-loopback relay URL.

    Cheap, explicit protection against a future edit quietly pointing a consumer at
    a routable address.
    """
    from urllib.parse import urlparse

    host = urlparse(url).hostname or ""
    if host not in _LOOPBACK_NAMES:
        raise RelayBindRefused(f"refusing non-loopback relay URL: {host!r}")


__all__ = [
    "LoopbackRelay",
    "RelayBindRefused",
    "RelayConfig",
    "assert_loopback_only",
    "relay_token_from_env",
    "ENV_RELAY_TOKEN",
    "ENV_USER",
    "ENV_PASSWORD",
    "DEFAULT_HOST",
    "CameraCredential",
    "default_dotenv_path",
]
