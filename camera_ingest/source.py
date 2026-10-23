"""Camera ingestion — modular source layer (isolated; not wired into the project).

Scope: this package acquires frames from the JUAN/EseeCloud camera on the LAN using
its NATIVE HTTP media path. It deliberately does NOT use RTSP: on this unit RTSP
advertises Digest and answers DESCRIBE, but every SETUP variant fails, so no media
ever arrives (voir the camera recovery report for SN Z7C64989722211).

Verified transports on this camera (all HTTP, port 80):

    GET /snapshot                       -> JPEG still
    GET /bubble/live?ch=0&stream=0      -> XML manifest naming both streams
    GET /livestream/12                  -> H.264 640x360 25fps   <- PREFERRED
    GET /livestream/11                  -> H.265 2304x1296       <- needs transcode

The preferred ingestion source is /livestream/12: H.264, the codec that needs no
transcoding for WebRTC/Echo pipelines, and the smaller of the two streams (so the
least CPU and bandwidth).

Design constraints honoured here:
  * modular — CameraSource is a protocol; JuanHttpSource is one implementation.
    A different camera only needs a new subclass.
  * minimal CPU — this layer copies bytes. It never decodes, and never transcodes.
  * no credential leakage — the credential is read from the environment (populated
    from the repo-root .env, which is gitignored). It is never taken from argv,
    never written to a log, and never placed in a URL that gets printed.
  * non-empty passwords are supported from the start — see CameraCredential.

Nothing in this module starts a listener, opens a public port, or writes media to
disk.
"""

from __future__ import annotations

import base64
import os
import pathlib
import re
import urllib.error
import urllib.request
from dataclasses import dataclass
from typing import Iterator, Optional, Protocol


# --------------------------------------------------------------------------- #
# Credentials
# --------------------------------------------------------------------------- #

#: Environment variable names. These are the ONLY places a credential is read
#: from. The repo-root .env is gitignored, so a value placed there stays out of
#: git; and because we read the environment rather than a command line, the
#: secret never appears in `ps`, in shell history, or in a log line.
ENV_USER = "HG_CAMERA_USER"
ENV_PASSWORD = "HG_CAMERA_PASSWORD"
ENV_HOST = "HG_CAMERA_HOST"

DEFAULT_HOST = "192.168.1.13"
DEFAULT_USER = "admin"

#: Matches KEY=VALUE in a .env file. Values may be single- or double-quoted.
_DOTENV_LINE = re.compile(r"^\s*([A-Za-z_][A-Za-z0-9_]*)\s*=\s*(.*)$")


def load_dotenv(path: str | os.PathLike[str]) -> dict[str, str]:
    """Parse a .env file into a dict, WITHOUT exporting or logging anything.

    Deliberately a small stdlib parser rather than a dependency: this package is
    meant to add as little as possible. Blank lines and #-comments are skipped;
    surrounding quotes are stripped; an inline `#` comment after an unquoted
    value is dropped.
    """
    values: dict[str, str] = {}
    p = pathlib.Path(path)
    if not p.is_file():
        return values
    for raw in p.read_text(encoding="utf-8").splitlines():
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        match = _DOTENV_LINE.match(line)
        if not match:
            continue
        key, value = match.group(1), match.group(2).strip()
        if len(value) >= 2 and value[0] == value[-1] and value[0] in "\"'":
            value = value[1:-1]
        elif " #" in value:
            value = value.split(" #", 1)[0].rstrip()
        values[key] = value
    return values


@dataclass(frozen=True)
class CameraCredential:
    """A camera credential.

    Supports an EMPTY password on purpose: this unit is currently provisioned with
    `admin` and no password, so refusing empty would make the adapter unusable
    against the camera as it stands. `is_empty` is here so callers can *report*
    that state (it is a security finding) rather than silently accept it.

    `__repr__`/`__str__` are overridden so an accidental print or a traceback
    frame cannot spill the secret.
    """

    user: str
    password: str = ""

    @property
    def is_empty(self) -> bool:
        return self.password == ""

    @classmethod
    def from_env(cls, dotenv_path: Optional[str | os.PathLike[str]] = None) -> "CameraCredential":
        """Build from the environment, falling back to a .env file.

        Precedence: real environment first, then the .env file. Environment wins
        so that a service manager can override the file without editing it.
        """
        file_values = load_dotenv(dotenv_path) if dotenv_path is not None else {}
        user = os.environ.get(ENV_USER) or file_values.get(ENV_USER) or DEFAULT_USER
        password = os.environ.get(ENV_PASSWORD)
        if password is None:
            password = file_values.get(ENV_PASSWORD, "")
        return cls(user=user, password=password)

    def basic_header(self) -> str:
        """The value for an `Authorization: Basic ...` header.

        Returned to the caller rather than logged. Base64 is encoding, not
        encryption — the caller is expected to send this over the LAN only.
        """
        raw = f"{self.user}:{self.password}".encode("utf-8")
        return "Basic " + base64.b64encode(raw).decode("ascii")

    def __repr__(self) -> str:  # pragma: no cover - trivial
        state = "empty" if self.is_empty else "set"
        return f"CameraCredential(user={self.user!r}, password=<{state}>)"

    __str__ = __repr__


def default_dotenv_path() -> pathlib.Path:
    """The repo-root .env, which is gitignored.

    Resolved relative to this file (camera_ingest/ -> worktree root) so the package
    works from any working directory. The main checkout's .env is accepted as a
    fallback because that is where the owner's real file lives.
    """
    here = pathlib.Path(__file__).resolve()
    worktree_root = here.parent.parent
    candidates = [
        worktree_root / ".env",
        pathlib.Path("/home/swarna-sekhar-dhar/projects/homeguardian-ai/.env"),
    ]
    for candidate in candidates:
        if candidate.is_file():
            return candidate
    return candidates[0]


# --------------------------------------------------------------------------- #
# Sources
# --------------------------------------------------------------------------- #


class CameraSource(Protocol):
    """A frame-bearing camera.

    Deliberately tiny: a new camera family implements this and nothing else needs
    to change. Implementations must not decode, transcode, or persist media.
    """

    @property
    def name(self) -> str:
        """Stable identifier for this source, safe to log."""
        ...

    def stream_url(self) -> str:
        """The media URL WITH credentials removed, safe to log."""
        ...

    def open_stream(self, timeout: float = 10.0) -> Iterator[bytes]:
        """Yield raw, undecoded bytes from the media path.

        Raw bytes on purpose: the bytes are already H.264/JPEG, so handing them
        straight to a consumer (or a file) costs no CPU here at all.
        """
        ...

    def probe(self, timeout: float = 8.0) -> "ProbeResult":
        """Cheap reachability + metadata check that does not read media."""
        ...


@dataclass(frozen=True)
class ProbeResult:
    ok: bool
    detail: str

    def __bool__(self) -> bool:  # pragma: no cover - trivial
        return self.ok


class JuanHttpSource:
    """Native HTTP media source for the JUAN/EseeCloud camera.

    Two media paths, both over plain HTTP on port 80:

      * ``"sub"``  -> /livestream/12  H.264 640x360 25fps   (preferred)
      * ``"main"`` -> /livestream/11  H.265 2304x1296       (needs a transcode)
      * ``"snapshot"`` -> /snapshot   JPEG stills           (cheapest of all)

    No RTSP. No ONVIF. No undocumented configuration routes.
    """

    _PATHS = {
        "sub": "/livestream/12",
        "main": "/livestream/11",
        "snapshot": "/snapshot",
    }

    def __init__(
        self,
        host: str = DEFAULT_HOST,
        credential: Optional[CameraCredential] = None,
        stream: str = "sub",
        port: int = 80,
    ) -> None:
        if stream not in self._PATHS:
            raise ValueError(f"stream must be one of {sorted(self._PATHS)}, got {stream!r}")
        self._host = host
        self._port = port
        self._stream = stream
        self._credential = credential if credential is not None else CameraCredential.from_env()

    # -- introspection ---------------------------------------------------- #

    @property
    def name(self) -> str:
        return f"juan-http:{self._stream}:{self._host}"

    @property
    def credential(self) -> CameraCredential:
        return self._credential

    @property
    def path(self) -> str:
        return self._PATHS[self._stream]

    def stream_url(self, include_credentials: bool = False) -> str:
        """The URL. By default WITHOUT the userinfo, so it is safe to log.

        include_credentials=True is provided for the rare case of handing a
        complete URL to a child process — but that path puts the secret in argv,
        which the constraints forbid, so callers should use headers instead.
        """
        userinfo = ""
        if include_credentials:
            userinfo = f"{self._credential.user}:{self._credential.password}@"
        return f"http://{userinfo}{self._host}:{self._port}{self.path}"

    # -- I/O -------------------------------------------------------------- #

    def _request(self, timeout: float) -> urllib.request.Request:
        return urllib.request.Request(
            self.stream_url(),
            headers={
                # The credential travels in a header, never in argv and never in
                # the URL that gets logged.
                "Authorization": self._credential.basic_header(),
                # Keep the vendor endpoint from returning a chunked/keep-alive
                # body we then have to disentangle.
                "Connection": "close",
                "User-Agent": "hg-camera-ingest/1",
            },
        )

    def probe(self, timeout: float = 8.0) -> ProbeResult:
        """Reachability check. Reads no media, so it is cheap and side-effect free."""
        try:
            with urllib.request.urlopen(self._request(timeout), timeout=timeout) as response:
                status = getattr(response, "status", None) or response.getcode()
                if status == 200:
                    return ProbeResult(True, "200 OK")
                return ProbeResult(False, f"unexpected status {status}")
        except urllib.error.HTTPError as exc:
            # 401 here means the credential is wrong or absent. Report the CODE,
            # never the header we sent.
            return ProbeResult(False, f"HTTP {exc.code}")
        except Exception as exc:  # noqa: BLE001 - surfaced as a result, not raised
            return ProbeResult(False, f"{type(exc).__name__}")

    def open_stream(self, timeout: float = 10.0, chunk: int = 32 * 1024) -> Iterator[bytes]:
        """Yield raw bytes from the media path. No decoding, no transcoding.

        For the livestream paths these bytes are the vendor's framed RTP payload
        (H.264 or H.265); for /snapshot they are a complete JPEG per request. In
        both cases the correct thing to do is pass them on untouched.
        """
        with urllib.request.urlopen(self._request(timeout), timeout=timeout) as response:
            while True:
                data = response.read(chunk)
                if not data:
                    return
                yield data

    def snapshot(self, timeout: float = 8.0) -> bytes:
        """One JPEG image. The cheapest possible read of this camera."""
        if self._stream != "snapshot":
            # Make the intent explicit rather than silently fetching a stream and
            # returning a truncated prefix of it.
            raise ValueError("snapshot() requires stream='snapshot'")
        with urllib.request.urlopen(self._request(timeout), timeout=timeout) as response:
            return response.read()
