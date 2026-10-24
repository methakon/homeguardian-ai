"""Tests for reading the camera's own detection overlay.

The point of these tests is not just "it finds a box". It is that a *negative*
result is trustworthy. A detector that silently never fires would look identical
to a camera that never draws a box, so the tests draw boxes in every colour the
vendor could plausibly use and require each one to be caught.
"""

from __future__ import annotations

import shutil
import subprocess
import unittest

from camera_ingest.detection_overlay import (
    BRIGHT_RUN_PIXELS,
    OverlayReading,
    read_detection_overlay,
)

W = 640
H = 360


def blank(scene: int = 80) -> bytearray:
    """A flat grey frame, as an IR/no-colour scene looks."""
    return bytearray([scene, scene, scene] * (W * H))


def draw_box(buf: bytearray, colour, x0=180, y0=90, x1=380, y1=270, thickness=3) -> None:
    """Draw a hollow rectangle outline, the way a detector overlay appears."""
    r, g, b = colour

    def put(x: int, y: int) -> None:
        if 0 <= x < W and 0 <= y < H:
            i = (y * W + x) * 3
            buf[i], buf[i + 1], buf[i + 2] = r, g, b

    for x in range(x0, x1):
        for t in range(thickness):
            put(x, y0 + t)
            put(x, y1 - t)
    for y in range(y0, y1):
        for t in range(thickness):
            put(x0 + t, y)
            put(x1 - t, y)


def decode_from(buf: bytes):
    """An injected decoder: hands back whatever RGB we prepared."""
    return lambda _jpeg, _width: buf


class TestDetectorIsNotBlind(unittest.TestCase):
    """Each plausible overlay colour MUST be caught, or a negative means nothing."""

    def test_green_box(self) -> None:
        buf = blank()
        draw_box(buf, (0, 255, 0))
        r = read_detection_overlay(b"", decode=decode_from(bytes(buf)), width=W)
        self.assertTrue(r.present, "a green box must be seen")
        self.assertGreater(r.vivid_pixels, 0)

    def test_red_box(self) -> None:
        buf = blank()
        draw_box(buf, (255, 0, 0))
        r = read_detection_overlay(b"", decode=decode_from(bytes(buf)), width=W)
        self.assertTrue(r.present, "a red box must be seen")
        self.assertGreater(r.vivid_pixels, 0)

    def test_yellow_box(self) -> None:
        buf = blank()
        draw_box(buf, (255, 255, 0))
        r = read_detection_overlay(b"", decode=decode_from(bytes(buf)), width=W)
        self.assertTrue(r.present, "a yellow box must be seen")

    def test_white_box(self) -> None:
        """White is not 'vivid', so it must be caught by the bright-run signal."""
        buf = blank()
        draw_box(buf, (255, 255, 255))
        r = read_detection_overlay(b"", decode=decode_from(bytes(buf)), width=W)
        self.assertTrue(r.present, "a white box must be seen")
        self.assertGreater(r.bright_runs, 0, "white must be caught by bright runs")

    def test_a_plain_scene_is_negative(self) -> None:
        buf = blank()
        r = read_detection_overlay(b"", decode=decode_from(bytes(buf)), width=W)
        self.assertFalse(r.present)
        self.assertEqual(r.vivid_pixels, 0)
        self.assertEqual(r.bright_runs, 0)
        self.assertEqual(r.reason, "no overlay")

    def test_a_noisy_grey_scene_is_still_negative(self) -> None:
        """IR scenes have texture and bright spots; that must not read as a box."""
        buf = blank()
        for i, v in enumerate(buf):
            buf[i] = min(255, v + ((i * 37) % 60))   # deterministic texture
        for x in range(400, 460):                     # a bright blob, not an outline
            for y in range(200, 260):
                j = (y * W + x) * 3
                buf[j] = buf[j + 1] = buf[j + 2] = 230
        r = read_detection_overlay(b"", decode=decode_from(bytes(buf)), width=W)
        self.assertFalse(r.present, "texture and a blob must not be mistaken for an outline")


class TestFailuresAreReportedNotSwallowed(unittest.TestCase):
    """"Could not look" must never be reported as "no box"."""

    def test_undecodable_frame(self) -> None:
        r = read_detection_overlay(b"", decode=lambda _j, _w: None, width=W)
        self.assertFalse(r.present)
        self.assertEqual(r.reason, "could not decode the frame")

    def test_wrong_frame_size_is_flagged(self) -> None:
        r = read_detection_overlay(b"", decode=decode_from(b"\x00" * 100), width=W)
        self.assertFalse(r.present)
        self.assertIn("whole number of rows", r.reason)

    def test_reading_is_falsy_when_absent(self) -> None:
        self.assertFalse(OverlayReading(False, 0, 0, "no overlay"))
        self.assertTrue(OverlayReading(True, 5, 0, "overlay present"))


@unittest.skipUnless(shutil.which("ffmpeg"), "ffmpeg not installed")
class TestThroughAFullDecode(unittest.TestCase):
    """One case through the real decoder, so the ffmpeg path is exercised too."""

    def _jpeg_with_box(self, colour: str) -> bytes:
        return subprocess.run(  # noqa: S603
            [
                "ffmpeg", "-v", "error",
                "-f", "lavfi", "-i", f"color=c=gray:s={W}x{H}",
                "-vf", f"drawbox=x=180:y=90:w=200:h=180:color={colour}@1.0:t=3",
                "-frames:v", "1", "-f", "image2", "-c:v", "mjpeg", "-",
            ],
            capture_output=True, check=True,
        ).stdout

    def test_green_box_through_ffmpeg(self) -> None:
        r = read_detection_overlay(self._jpeg_with_box("green"), width=W)
        self.assertTrue(r.present, "the real decode path must see a green box")

    def test_white_box_through_ffmpeg(self) -> None:
        r = read_detection_overlay(self._jpeg_with_box("white"), width=W)
        self.assertTrue(r.present)

    def test_plain_frame_through_ffmpeg(self) -> None:
        jpeg = subprocess.run(  # noqa: S603
            ["ffmpeg", "-v", "error", "-f", "lavfi", "-i", f"color=c=gray:s={W}x{H}",
             "-frames:v", "1", "-f", "image2", "-c:v", "mjpeg", "-"],
            capture_output=True, check=True,
        ).stdout
        r = read_detection_overlay(jpeg, width=W)
        self.assertFalse(r.present, "a plain grey frame must not report a box")


class TestOverlayRunThreshold(unittest.TestCase):
    def test_a_short_bright_run_is_not_an_outline(self) -> None:
        buf = blank()
        for x in range(0, BRIGHT_RUN_PIXELS - 10):   # just under the threshold
            for y in range(50, 53):
                j = (y * W + x) * 3
                buf[j] = buf[j + 1] = buf[j + 2] = 255
        r = read_detection_overlay(b"", decode=decode_from(bytes(buf)), width=W)
        self.assertFalse(r.present, "a short bright line is not a box edge")


if __name__ == "__main__":
    unittest.main(verbosity=2)
