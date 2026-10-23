"""Tests for the bubble-protocol source (video AND audio on one connection).

Two layers:

  * Parser/codec tests are pure: synthetic bytes, no socket, no camera.
  * Handshake tests run against a FAKE camera that speaks the real framing, so
    connect/auth/start/media are exercised end to end without touching a device.
  * One opt-in live test hits the real camera and self-skips when it is absent.

Run:  python3 -m unittest camera_ingest.test_bubble -v
"""

from __future__ import annotations

import io
import logging
import socket
import struct
import threading
import unittest
from unittest import mock

from camera_ingest.bubble import (
    AUTH_TIMESTAMP,
    HEADER_BYTES,
    MAX_FRAME_BYTES,
    MANIFEST_BYTES,
    PACKET_AUTH,
    PACKET_MEDIA,
    PACKET_START,
    START_TIMESTAMP,
    SYNC_BYTE,
    TYPE_AUDIO,
    TYPE_FRAME,
    TYPE_KEYFRAME,
    BubbleProtocolError,
    BubbleSource,
    build_frame,
    parse_header,
    parse_media_payload,
)
from camera_ingest.source import CameraCredential, default_dotenv_path

FAKE_MANIFEST = (
    '<bubble version="1.0" vin="1"><vin0 stream="2">\r\n'
    '<stream0 name="720p.265" size="2304x1296" x1="yes" x2="yes" x4="yes" />\r\n'
    '<stream1 name="360p.264" size="640x360" x1="yes" x2="yes" x4="yes" />\r\n'
    '<vin0>\r\n</bubble>'
).encode("latin-1")


class TestFraming(unittest.TestCase):
    def test_build_then_parse_header_round_trips(self) -> None:
        payload = b"hello"
        wire = build_frame(PACKET_MEDIA, 0x11223344, payload)
        self.assertEqual(wire[0], SYNC_BYTE)
        size, command, ts = parse_header(wire[:HEADER_BYTES])
        self.assertEqual(command, PACKET_MEDIA)
        self.assertEqual(ts, 0x11223344)
        self.assertEqual(size, 5 + len(payload))
        self.assertEqual(wire[HEADER_BYTES:], payload)

    def test_size_counts_command_timestamp_and_payload(self) -> None:
        """The protocol's size field is 5+len, not len. Getting this wrong desyncs the stream."""
        wire = build_frame(PACKET_AUTH, 1, b"x" * 10)
        size, _, _ = parse_header(wire[:HEADER_BYTES])
        self.assertEqual(size, 15)

    def test_rejects_a_short_header(self) -> None:
        with self.assertRaises(BubbleProtocolError):
            parse_header(b"\xaa\x00\x00")

    def test_rejects_a_wrong_sync_byte(self) -> None:
        bad = build_frame(PACKET_MEDIA, 1, b"x")
        with self.assertRaises(BubbleProtocolError):
            parse_header(b"\x55" + bad[1:HEADER_BYTES])


class TestMediaParsing(unittest.TestCase):
    def test_parses_a_keyframe(self) -> None:
        payload = BubbleSource.synthesize_media_frame(TYPE_KEYFRAME, b"\x00\x00\x00\x01\x65data")
        frame = parse_media_payload(payload)
        self.assertEqual(frame.kind, "video")
        self.assertTrue(frame.is_keyframe)
        self.assertEqual(frame.payload, b"\x00\x00\x00\x01\x65data")

    def test_parses_an_inter_frame_as_not_keyframe(self) -> None:
        frame = parse_media_payload(BubbleSource.synthesize_media_frame(TYPE_FRAME, b"\x41x"))
        self.assertEqual(frame.kind, "video")
        self.assertFalse(frame.is_keyframe)

    def test_parses_audio_and_its_metadata(self) -> None:
        """Audio is type 0, with a fixed 36-byte header before the G.711 bytes."""
        g711 = bytes(range(64))
        payload = BubbleSource.synthesize_audio_media(g711, rate=8000, width=16, pts=1234)
        frame = parse_media_payload(payload)
        self.assertEqual(frame.kind, "audio")
        self.assertEqual(frame.payload, g711)
        self.assertEqual(frame.audio_name, "g711")
        self.assertEqual(frame.sample_rate, 8000)
        self.assertEqual(frame.sample_width, 16)
        self.assertEqual(frame.pts, 1234)

    def test_audio_payload_is_exactly_the_g711_bytes(self) -> None:
        """The header must be stripped, not passed through as audio."""
        g711 = b"\xd5" * 160
        frame = parse_media_payload(BubbleSource.synthesize_audio_media(g711))
        self.assertEqual(len(frame.payload), 160)
        self.assertNotIn(b"g711", frame.payload)

    def test_rejects_an_unknown_media_type(self) -> None:
        with self.assertRaises(BubbleProtocolError):
            parse_media_payload(BubbleSource.synthesize_media_frame(9, b"x"))

    def test_rejects_a_payload_shorter_than_its_own_prefix(self) -> None:
        with self.assertRaises(BubbleProtocolError):
            parse_media_payload(b"\x00\x01\x02")

    def test_rejects_audio_smaller_than_its_header(self) -> None:
        payload = BubbleSource.synthesize_media_frame(TYPE_AUDIO, b"\x00" * 10)
        with self.assertRaises(BubbleProtocolError):
            parse_media_payload(payload)

    def test_oversized_frame_is_refused_before_allocating(self) -> None:
        """A hostile or broken size field must not make us allocate it."""
        src = BubbleSource("127.0.0.1")
        src._sock = mock.Mock()
        header = build_frame(PACKET_MEDIA, 0, b"")[:HEADER_BYTES]
        huge = struct.pack(">B", SYNC_BYTE) + struct.pack(
            ">I", MAX_FRAME_BYTES + 1000
        ) + header[5:]
        src._recv_exact = lambda n: huge  # type: ignore[method-assign]
        with self.assertRaises(BubbleProtocolError):
            src._read_frame()


class TestCredentialContainment(unittest.TestCase):
    def test_url_carries_no_credential(self) -> None:
        src = BubbleSource("10.0.0.9", CameraCredential("admin", "sup3r-s3cret"))
        self.assertNotIn("sup3r-s3cret", src.stream_url())
        self.assertNotIn("@", src.stream_url())

    def test_repr_carries_no_credential(self) -> None:
        src = BubbleSource("10.0.0.9", CameraCredential("admin", "sup3r-s3cret"))
        self.assertNotIn("sup3r-s3cret", repr(src))

    def test_auth_payload_places_user_and_password_in_their_fields(self) -> None:
        src = BubbleSource("10.0.0.9", CameraCredential("bob", "pw"))
        payload = src._auth_payload()
        self.assertEqual(len(payload), 48)
        self.assertEqual(struct.unpack(">I", payload[0:4])[0], 44)
        self.assertEqual(payload[8:11], b"bob")
        self.assertEqual(payload[28:30], b"pw")

    def test_auth_payload_handles_an_empty_password(self) -> None:
        """The camera currently uses admin with an empty password."""
        src = BubbleSource("10.0.0.9", CameraCredential("admin", ""))
        payload = src._auth_payload()
        self.assertEqual(payload[8:13], b"admin")
        self.assertEqual(payload[28:48], b"\x00" * 20)

    def test_a_long_password_does_not_overflow_its_field(self) -> None:
        src = BubbleSource("10.0.0.9", CameraCredential("admin", "x" * 200))
        self.assertEqual(len(src._auth_payload()), 48)


class FakeCamera:
    """A TCP server that speaks the real framing. No device involved."""

    def __init__(self, *, accept_auth: bool = True, frames=None, manifest=FAKE_MANIFEST):
        self.accept_auth = accept_auth
        self.manifest = manifest
        self.frames = frames if frames is not None else [
            build_frame(PACKET_MEDIA, 0, BubbleSource.synthesize_media_frame(TYPE_KEYFRAME, b"\x00\x00\x00\x01\x65K")),
            build_frame(PACKET_MEDIA, 0, BubbleSource.synthesize_media_frame(TYPE_FRAME, b"\x41V")),
            build_frame(PACKET_MEDIA, 0, BubbleSource.synthesize_audio_media(b"\xd5" * 160)),
        ]
        self.received_command: int | None = None
        self.received_payload: bytes = b""
        self.request_line = ""
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.sock.bind(("127.0.0.1", 0))
        self.sock.listen(1)
        self.port = self.sock.getsockname()[1]
        self._thread = threading.Thread(target=self._serve, daemon=True)
        self._thread.start()

    def _read_frame(self, conn) -> tuple[int, bytes]:
        header = b""
        while len(header) < HEADER_BYTES:
            chunk = conn.recv(HEADER_BYTES - len(header))
            if not chunk:
                raise ConnectionError("client went away")
            header += chunk
        size, command, _ = parse_header(header)
        body = size - 5
        payload = b""
        while len(payload) < body:
            chunk = conn.recv(body - len(payload))
            if not chunk:
                break
            payload += chunk
        return command, payload

    def _serve(self) -> None:
        try:
            conn, _ = self.sock.accept()
        except OSError:
            return
        try:
            line = b""
            while b"\r\n\r\n" not in line:
                chunk = conn.recv(1)
                if not chunk:
                    return
                line += chunk
            self.request_line = line.split(b"\r\n", 1)[0].decode("latin-1")

            conn.sendall(b"HTTP/1.1 200 OK\r\nContent-Type: video/bubble\r\n\r\n")
            conn.sendall(self.manifest.ljust(MANIFEST_BYTES, b"#")[:MANIFEST_BYTES])

            command, payload = self._read_frame(conn)
            self.received_command = command
            self.received_payload = payload
            if not self.accept_auth:
                # Reply in the protocol's shape but with the reject marker.
                reply = bytearray(44)
                reply[4] = 3
                reply[8] = 0
                conn.sendall(build_frame(PACKET_AUTH, 0, bytes(reply)))
                return
            reply = bytearray(44)
            reply[4] = 3
            reply[8] = 1
            conn.sendall(build_frame(PACKET_AUTH, 0, bytes(reply)))

            self._read_frame(conn)   # start
            for f in self.frames:
                conn.sendall(f)
        except Exception:
            pass
        finally:
            try:
                conn.close()
            except Exception:
                pass

    def close(self) -> None:
        try:
            self.sock.close()
        except Exception:
            pass


class TestHandshakeAgainstFakeCamera(unittest.TestCase):
    def setUp(self) -> None:
        self.cam = FakeCamera()
        self.addCleanup(self.cam.close)

    def _source(self, password: str = "pw") -> BubbleSource:
        return BubbleSource(
            "127.0.0.1",
            CameraCredential("admin", password),
            port=self.cam.port,
            timeout=5,
        )

    def test_probe_reports_a_frame_with_the_stream_codec(self) -> None:
        src = self._source()
        result = src.probe()
        self.assertTrue(result.ok, result.detail)
        self.assertIn("video", result.detail)
        # The fake manifest's stream0 is "720p.265", so stream 0 must read as h265.
        self.assertEqual(src.video_codec, "h265")

    def test_the_codec_follows_the_requested_stream(self) -> None:
        """stream0 is .265 and stream1 is .264 in the manifest - the label must track it."""
        src = BubbleSource(
            "127.0.0.1", CameraCredential("admin", "pw"), port=self.cam.port, stream=1, timeout=5
        )
        src.probe()
        self.assertEqual(src.video_codec, "h264")

    def test_the_request_line_carries_no_credentials(self) -> None:
        with self._source() as src:
            src.probe()
        self.assertNotIn("pw", self.cam.request_line)
        self.assertNotIn("@", self.cam.request_line)

    def test_audio_alone_does_not_make_the_probe_fail(self) -> None:
        """The first frame off a real camera can be audio; that is still a live link."""
        cam = FakeCamera(frames=[
            build_frame(PACKET_MEDIA, 0, BubbleSource.synthesize_audio_media(b"\xd5" * 160))
        ])
        self.addCleanup(cam.close)
        src = BubbleSource("127.0.0.1", CameraCredential("admin", "pw"), port=cam.port, timeout=5)
        result = src.probe()
        self.assertTrue(result.ok, result.detail)
        self.assertIn("audio", result.detail)


class TestStreamYieldsBothTracks(unittest.TestCase):
    def test_open_stream_yields_video_and_audio_from_one_connection(self) -> None:
        cam = FakeCamera()
        self.addCleanup(cam.close)
        src = BubbleSource("127.0.0.1", CameraCredential("admin", "pw"), port=cam.port, timeout=5)
        frames = []
        with src:
            for frame in src.open_stream():
                frames.append(frame)
                if len(frames) >= 3:
                    break
        kinds = [f.kind for f in frames]
        self.assertEqual(kinds, ["video", "video", "audio"], kinds)
        self.assertTrue(frames[0].is_keyframe)
        self.assertFalse(frames[1].is_keyframe)
        self.assertEqual(len(frames[2].payload), 160)
        self.assertEqual(frames[2].audio_name, "g711")


class TestAuthRejection(unittest.TestCase):
    def test_a_rejected_handshake_raises_and_does_not_echo_the_payload(self) -> None:
        cam = FakeCamera(accept_auth=False)
        self.addCleanup(cam.close)
        src = BubbleSource(
            "127.0.0.1", CameraCredential("admin", "the-password"), port=cam.port, timeout=5
        )
        with self.assertRaises(BubbleProtocolError) as caught:
            src.connect()
        message = str(caught.exception)
        self.assertIn("auth rejected", message)
        # The refusal must not carry the credential back out.
        self.assertNotIn("the-password", message)

    def test_probe_reports_a_rejection_as_a_result_not_an_exception(self) -> None:
        cam = FakeCamera(accept_auth=False)
        self.addCleanup(cam.close)
        src = BubbleSource("127.0.0.1", CameraCredential("admin", "pw"), port=cam.port, timeout=5)
        result = src.probe()
        self.assertFalse(result.ok)
        self.assertIn("auth rejected", result.detail)

    def test_a_non_200_is_reported_plainly(self) -> None:
        src = BubbleSource("127.0.0.1", CameraCredential("admin", "pw"), port=1, timeout=2)
        result = src.probe()
        self.assertFalse(result.ok)

    def test_no_socket_connection_leaks_the_password_into_logs(self) -> None:
        cam = FakeCamera(accept_auth=False)
        self.addCleanup(cam.close)
        captures = io.StringIO()
        handler = logging.StreamHandler(captures)
        logger = logging.getLogger("camera_ingest.bubble")
        logger.addHandler(handler)
        try:
            src = BubbleSource(
                "127.0.0.1", CameraCredential("admin", "leak-me-not"), port=cam.port, timeout=5
            )
            src.probe()
        finally:
            logger.removeHandler(handler)
        self.assertNotIn("leak-me-not", captures.getvalue())


class TestLiveCamera(unittest.TestCase):
    """Opt-in; skips when the camera is not reachable. Proves the real device."""

    CAMERA_HOST = "192.168.1.13"

    def _reachable(self) -> bool:
        try:
            with socket.create_connection((self.CAMERA_HOST, 80), timeout=4):
                return True
        except OSError:
            return False

    def test_live_handshake_yields_video_and_audio(self) -> None:
        if not self._reachable():
            self.skipTest(f"camera {self.CAMERA_HOST} not reachable")

        src = BubbleSource(
            self.CAMERA_HOST,
            CameraCredential.from_env(default_dotenv_path()),
            stream=0,          # the manifest's stream0 is the H.265 main stream
            timeout=15,
        )
        got_audio = False
        got_video = False
        audio_name = ""
        with src:
            for frame in src.open_stream(timeout=15):
                if frame.kind == "video":
                    got_video = True
                elif frame.kind == "audio":
                    got_audio = True
                    audio_name = frame.audio_name
                if got_video and got_audio:
                    break
        self.assertTrue(got_video, "no video frame from the real camera")
        self.assertTrue(got_audio, "no audio frame from the real camera")
        self.assertEqual(audio_name, "g711", f"unexpected audio codec name: {audio_name!r}")


if __name__ == "__main__":
    unittest.main(verbosity=2)
