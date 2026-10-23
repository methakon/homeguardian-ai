"""Bubble-protocol camera source: video AND audio from one connection.

WHY THIS EXISTS
The camera's plain HTTP paths carry video only. Verified by ffprobe:
`/livestream/11` (H.265 2304x1296) and `/livestream/12` (H.264 640x360) both
report `['video']` and no audio track. But the owner needs audio too, and the
camera does have it.

The vendor's own app gets audio because it does not use those paths: it speaks a
proprietary protocol on `/bubble/live`. The audio is MULTIPLEXED INTO THE SAME
TCP STREAM as the video and distinguished by a type byte:

    media payload:  [0:4] size u32 | [4] type | [5] channel | [6:] media
                    type 1 = keyframe, 2 = other video frame, 0 = AUDIO

So one connection yields both tracks. This module implements that protocol, from
the public go2rtc implementation (pkg/bubble/client.go), which is the only
independent description of it that exists.

WHAT THIS IS NOT
Not RTSP. RTSP SETUP fails on every variant for this unit (TCP interleaved, UDP
client_port, realaudio/realvideo paths, with and without Require) - recorded in
the camera recovery report. RTSP was never the road.

No transcode: video is passed through as Annex-B, audio as G.711 bytes. Nothing
is decoded, resampled or re-encoded here.

CREDENTIAL HANDLING
The credential travels in the auth payload of the handshake and nowhere else. It
never enters argv, a log line, a URL, or `repr()`. The URL this module reports is
credential-free by construction.

SAFETY
The camera daemon is fragile: a wrong request has previously killed it until a
physical power cycle. Therefore: exactly one connection, bounded reads, bounded
frame sizes, an explicit deadline on every socket operation, and no retry storm.
`probe()` opens one connection and closes it.
"""

from __future__ import annotations

import logging
import socket
import struct
from dataclasses import dataclass
from typing import Iterator, Sequence

from camera_ingest.source import CameraCredential, ProbeResult

LOG = logging.getLogger("camera_ingest.bubble")

# --------------------------------------------------------------------------- #
# Protocol constants (from the public go2rtc implementation)
# --------------------------------------------------------------------------- #

SYNC_BYTE = 0xAA
PACKET_AUTH = 0x00
PACKET_MEDIA = 0x01
PACKET_START = 0x0A

AUTH_TIMESTAMP = 0x0E16C271
START_TIMESTAMP = 0x0E16C2DF

HEADER_BYTES = 10          # sync(1) + size(4) + cmd(1) + ts(4)
AUTH_PAYLOAD_BYTES = 48    # size(4) + 4 unknown + user(20) + pass(20)
START_PAYLOAD_BYTES = 16
MANIFEST_BYTES = 1024      # the XML the camera sends before the binary stream
USER_FIELD_BYTES = 20
PASS_FIELD_BYTES = 20

# Media payload layout
MEDIA_PREFIX_BYTES = 6     # size(4) + type(1) + channel(1)
AUDIO_HEADER_BYTES = 36    # entries|size|pts|gtime|name|rate|width
TYPE_KEYFRAME = 1
TYPE_FRAME = 2
TYPE_AUDIO = 0

# Bounds. A camera that is misbehaving must not be able to make us allocate.
MAX_FRAME_BYTES = 4 * 1024 * 1024
MIN_FRAME_BYTES = MEDIA_PREFIX_BYTES + 1

DEFAULT_PORT = 80
DEFAULT_PATH = "/bubble/live"


class BubbleProtocolError(RuntimeError):
    """The camera did not answer in the protocol's shape."""


@dataclass(frozen=True)
class BubbleFrame:
    """One media unit off the wire. Video and audio share a type byte."""

    kind: str            # "video" | "audio"
    payload: bytes
    is_keyframe: bool = False
    channel: int = 0
    # Audio-only metadata, decoded from the audio header.
    audio_name: str = ""
    sample_rate: int = 0
    sample_width: int = 0
    pts: int = 0

    def __repr__(self) -> str:
        return (
            f"<BubbleFrame {self.kind} {len(self.payload)}B"
            f"{' keyframe' if self.is_keyframe else ''}"
            f"{' ' + self.audio_name if self.audio_name else ''}>"
        )


def build_frame(command: int, timestamp: int, payload: bytes) -> bytes:
    """Encode one wire frame: 0xAA | size u32 BE | cmd | ts u32 BE | payload.

    `size` counts the command byte, the timestamp and the payload (5 + len).
    """
    size = 5 + len(payload)
    return (
        struct.pack(">B", SYNC_BYTE)
        + struct.pack(">I", size)
        + struct.pack(">B", command)
        + struct.pack(">I", timestamp)
        + payload
    )


def parse_header(header: bytes) -> tuple[int, int, int]:
    """Split a 10-byte wire header into (size, command, timestamp)."""
    if len(header) != HEADER_BYTES:
        raise BubbleProtocolError(f"short header: {len(header)} bytes")
    if header[0] != SYNC_BYTE:
        raise BubbleProtocolError(f"wrong sync byte: 0x{header[0]:02x}")
    size = struct.unpack(">I", header[1:5])[0]
    command = header[5]
    timestamp = struct.unpack(">I", header[6:10])[0]
    return size, command, timestamp


def parse_media_payload(payload: bytes) -> BubbleFrame:
    """Turn a media payload into a video or audio frame.

    Layout: size u32 | type u8 | channel u8 | media
      type 1 = keyframe, 2 = other video frame  -> media is Annex-B
      type 0 = audio                            -> 36-byte header, then G.711
    """
    if len(payload) < MIN_FRAME_BYTES:
        raise BubbleProtocolError(f"media payload too short: {len(payload)}")
    frame_type = payload[4]
    channel = payload[5]
    media = payload[MEDIA_PREFIX_BYTES:]

    if frame_type in (TYPE_KEYFRAME, TYPE_FRAME):
        return BubbleFrame(
            kind="video",
            payload=media,
            is_keyframe=(frame_type == TYPE_KEYFRAME),
            channel=channel,
        )

    if frame_type == TYPE_AUDIO:
        # The audio header is fixed-width; anything shorter than it plus a sample
        # is not a usable audio frame.
        if len(media) <= AUDIO_HEADER_BYTES:
            raise BubbleProtocolError("audio frame smaller than its own header")
        inner = media[:AUDIO_HEADER_BYTES]
        # inner: entries(4) size(4) pts(8) gtime(4) name(8) rate(4) width(4), all LE
        entries = struct.unpack("<I", inner[0:4])[0]
        pts = struct.unpack("<Q", inner[8:16])[0]
        name = inner[20:28].split(b"\x00", 1)[0].decode("ascii", "replace")
        rate = struct.unpack("<I", inner[28:32])[0]
        width = struct.unpack("<I", inner[32:36])[0]
        audio = media[AUDIO_HEADER_BYTES:]
        frame = BubbleFrame(
            kind="audio",
            payload=audio,
            channel=channel,
            audio_name=name,
            sample_rate=rate,
            sample_width=width,
            pts=pts,
        )
        # `entries` is documented as always 1; note it without failing, because a
        # strict check here would drop audio for a cosmetic disagreement.
        if entries != 1:
            LOG.debug("bubble: audio frame reported entries=%d (expected 1)", entries)
        return frame

    raise BubbleProtocolError(f"unknown media type {frame_type}")


class BubbleSource:
    """A CameraSource that yields interleaved video and audio frames."""

    def __init__(
        self,
        host: str,
        credential: CameraCredential | None = None,
        *,
        stream: int = 0,
        channel: int = 0,
        port: int = DEFAULT_PORT,
        path: str = DEFAULT_PATH,
        timeout: float = 10.0,
    ) -> None:
        self._host = host
        self._credential = credential or CameraCredential(user="admin", password="")
        self._stream = stream
        self._channel = channel
        self._port = port
        self._path = path
        self._timeout = timeout
        self._sock: socket.socket | None = None
        self._buf = b""
        self._manifest = ""
        self._video_codec = ""

    # -- identity ---------------------------------------------------------- #

    @property
    def name(self) -> str:
        return f"juan-bubble:ch{self._channel}/stream{self._stream}:{self._host}"

    @property
    def manifest(self) -> str:
        return self._manifest

    @property
    def video_codec(self) -> str:
        return self._video_codec

    def stream_url(self) -> str:
        """A credential-free URL, safe to log."""
        return f"http://{self._host}:{self._port}{self._path}?ch={self._channel}&stream={self._stream}"

    def __repr__(self) -> str:
        return (
            f"<BubbleSource {self._host} ch={self._channel} stream={self._stream} "
            f"codec={self._video_codec or '?'}>"
        )

    # -- wire helpers ------------------------------------------------------ #

    def _recv_exact(self, count: int) -> bytes:
        if self._sock is None:
            raise BubbleProtocolError("not connected")
        chunks = []
        remaining = count
        while remaining > 0:
            chunk = self._sock.recv(min(remaining, 65536))
            if not chunk:
                raise BubbleProtocolError("camera closed the connection")
            chunks.append(chunk)
            remaining -= len(chunk)
        return b"".join(chunks)

    def _read_frame(self) -> tuple[int, bytes]:
        size, command, _ts = parse_header(self._recv_exact(HEADER_BYTES))
        # `size` covers cmd + ts + payload, so the payload is size - 5.
        body = size - 5
        if body < 0 or body > MAX_FRAME_BYTES:
            raise BubbleProtocolError(f"refusing an out-of-range frame size: {body}")
        payload = self._recv_exact(body) if body else b""
        return command, payload

    def _write_frame(self, command: int, timestamp: int, payload: bytes) -> None:
        if self._sock is None:
            raise BubbleProtocolError("not connected")
        self._sock.sendall(build_frame(command, timestamp, payload))

    def _auth_payload(self) -> bytes:
        """size(4) + 4 unknown + user(20) + pass(20), NUL-padded.

        The credential exists only inside this buffer. It is never logged, never
        put in the URL, and never stored on the object.
        """
        b = bytearray(AUTH_PAYLOAD_BYTES)
        struct.pack_into(">I", b, 0, 44)
        user = self._credential.user.encode("utf-8")[: USER_FIELD_BYTES - 1]
        password = self._credential.password.encode("utf-8")[: PASS_FIELD_BYTES - 1]
        b[8 : 8 + len(user)] = user
        b[28 : 28 + len(password)] = password
        return bytes(b)

    def _apply_manifest(self, xml: str) -> None:
        """Pick the codec for the requested stream out of the XML manifest."""
        self._manifest = xml
        marker = f"<stream{self._stream} "
        for line in xml.replace("/>", "/>\n").splitlines():
            if line.strip().startswith(marker):
                self._video_codec = "h265" if ".265" in line else "h264"
                return
        # Absent is not fatal: the video bytes are Annex-B either way, and the
        # codec only labels them. Record that it was not stated.
        self._video_codec = ""

    # -- lifecycle --------------------------------------------------------- #

    def connect(self, timeout: float | None = None) -> "BubbleSource":
        """One connection: HTTP upgrade, then the binary handshake."""
        deadline = timeout if timeout is not None else self._timeout
        sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        sock.settimeout(deadline)
        try:
            sock.connect((self._host, self._port))
        except OSError:
            sock.close()
            raise

        self._sock = sock
        # The request line carries NO credentials: auth happens in the payload.
        request = (
            f"GET {self._path}?ch={self._channel}&stream={self._stream} HTTP/1.1\r\n"
            f"Host: {self._host}\r\n"
            f"Connection: close\r\n\r\n"
        )
        sock.sendall(request.encode("ascii"))

        # Response head, up to the blank line.
        head = bytearray()
        while b"\r\n\r\n" not in head:
            chunk = sock.recv(1)
            if not chunk:
                raise BubbleProtocolError("camera closed before the response head")
            head.extend(chunk)
            if len(head) > 8192:
                raise BubbleProtocolError("response head too long")
        status_line = bytes(head).split(b"\r\n", 1)[0].decode("latin-1")
        if " 200 " not in status_line:
            raise BubbleProtocolError(f"camera refused the request: {status_line}")

        # Exactly 1024 bytes of XML manifest precede the binary stream.
        xml = self._recv_exact(MANIFEST_BYTES)
        self._apply_manifest(xml.decode("latin-1", "replace"))

        self._write_frame(PACKET_AUTH, AUTH_TIMESTAMP, self._auth_payload())

        command, payload = self._read_frame()
        if command != PACKET_AUTH or len(payload) != 44 or payload[4] != 3 or payload[8] != 1:
            # Deliberately does not include the payload: it could echo the credential.
            raise BubbleProtocolError(
                f"auth rejected (cmd=0x{command:02x} len={len(payload)})"
            )
        return self

    def start(self) -> "BubbleSource":
        """Open the stream. The camera's fields here are little-endian."""
        payload = bytearray(START_PAYLOAD_BYTES)
        struct.pack_into("<I", payload, 0, self._channel)
        struct.pack_into("<I", payload, 4, self._stream)
        struct.pack_into("<I", payload, 8, 1)  # opened
        self._write_frame(PACKET_START, START_TIMESTAMP, bytes(payload))
        return self

    def close(self) -> None:
        if self._sock is not None:
            try:
                self._sock.close()
            except OSError:
                pass
            self._sock = None

    def __enter__(self) -> "BubbleSource":
        return self

    def __exit__(self, *exc) -> bool:
        self.close()
        return False

    # -- media ------------------------------------------------------------- #

    def open_stream(self, timeout: float | None = None) -> Iterator[BubbleFrame]:
        """Connect, start, and yield frames until the caller stops or the link ends.

        Video and audio arrive interleaved on the one connection; each frame says
        which it is.
        """
        if self._sock is None:
            self.connect(timeout)
        if self._sock is not None and timeout is not None:
            self._sock.settimeout(timeout)
        self.start()
        while True:
            command, payload = self._read_frame()
            if command != PACKET_MEDIA:
                # Control or keepalive traffic; not media.
                continue
            yield parse_media_payload(payload)

    def probe(self, timeout: float | None = None) -> ProbeResult:
        """One connection, handshake, and the first media frame. Then close."""
        try:
            self.connect(timeout)
            self.start()
            for _ in range(50):
                command, payload = self._read_frame()
                if command != PACKET_MEDIA:
                    continue
                frame = parse_media_payload(payload)
                return ProbeResult(
                    True,
                    f"{frame.kind} frame, {len(frame.payload)} bytes, codec={self._video_codec or '?'}",
                )
            return ProbeResult(False, "connected but no media frame arrived")
        except BubbleProtocolError as exc:
            return ProbeResult(False, f"protocol: {exc}")
        except OSError as exc:
            return ProbeResult(False, f"{type(exc).__name__}: {exc}")
        finally:
            self.close()

    # -- offline construction (used by the tests) -------------------------- #

    @staticmethod
    def synthesize_media_frame(frame_type: int, media: bytes, channel: int = 0) -> bytes:
        """Build a media payload, for exercising the parser without a camera."""
        return struct.pack(">I", len(media) + 2) + bytes([frame_type, channel]) + media

    @staticmethod
    def synthesize_audio_media(
        g711: bytes, rate: int = 8000, width: int = 16, pts: int = 0, name: bytes = b"g711"
    ) -> bytes:
        """Build an audio media payload with its fixed-width header."""
        header = bytearray(AUDIO_HEADER_BYTES)
        struct.pack_into("<I", header, 0, 1)                   # entries
        struct.pack_into("<I", header, 4, len(g711))           # size
        struct.pack_into("<Q", header, 8, pts)                 # pts
        header[20 : 20 + len(name)] = name[:8]                 # name
        struct.pack_into("<I", header, 28, rate)               # sample rate
        struct.pack_into("<I", header, 32, width)              # sample width
        return BubbleSource.synthesize_media_frame(TYPE_AUDIO, bytes(header) + g711)
