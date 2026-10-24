"""Read the camera's OWN detection overlay out of a frame.

Why this exists
---------------
This camera has onboard human detection, and it can be told to draw the result
onto the video itself (the vendor flag is `hdDrawRegion`). When it does, the box
is burned into the frame and therefore reaches every consumer of the stream for
free - no client-side inference, nothing to decode specially.

That matters because the camera's detection *label* is not available over the
LAN at all. There is no event endpoint (``/event``, ``/alert``, ``/detect``,
``/notify`` and friends all 404), and the live protocol carries only auth, media
and start packets - no event packet. The notifications the phone app shows come
over the vendor's own cloud. So the box on screen is the only place the camera's
verdict is visible to us, and this module is how we read it.

What it is not
--------------
This does NOT classify objects. It answers one narrow question: "did the camera
draw a detection box on this frame?" The box is the camera's verdict; we are
merely transcribing it. Anything beyond that needs a real detection model.

How it decides
--------------
Two independent signals, because the vendor's box colour is not something we can
assume:

  * **vivid pixels** - a coloured outline (green, red, yellow) is strongly
    saturated in a way a grey/IR scene is not.
  * **bright runs** - a white or light outline shows as long unbroken runs of
    near-white pixels, which a natural scene does not produce.

Both were validated before being trusted: known green, red and white boxes were
drawn onto a real frame from this camera and each was caught. Without that check
a "no box" result would be indistinguishable from the detector being blind -
which is exactly the mistake this docstring exists to prevent.

Decoding is injectable and ffmpeg is optional, matching the rest of this package:
the code itself is stdlib-only.
"""

from __future__ import annotations

import subprocess
from dataclasses import dataclass
from typing import Callable, Optional

#: A pixel counts as "vivid" when it is bright enough to be an overlay and its
#: channels are far enough apart to be a colour rather than grey.
VIVID_MIN_BRIGHTNESS = 90
VIVID_MIN_SPREAD = 60

#: A pixel counts as "overlay white" at or above this level on every channel.
BRIGHT_LEVEL = 190

#: A horizontal run of overlay-white pixels this long is an outline, not noise.
BRIGHT_RUN_PIXELS = 80

#: The camera's sub-stream and snapshot size. Needed to walk rows.
DEFAULT_WIDTH = 640

#: How long a single decode may take before it is abandoned.
DECODE_TIMEOUT_S = 20.0


@dataclass(frozen=True)
class OverlayReading:
    """The answer to "did the camera draw a box on this frame?"."""

    present: bool
    vivid_pixels: int
    bright_runs: int
    reason: str

    def __bool__(self) -> bool:  # pragma: no cover - trivial
        return self.present


def decode_rgb(jpeg: bytes, width: int = DEFAULT_WIDTH) -> Optional[bytes]:
    """Decode a JPEG to packed RGB24 via ffmpeg.

    Returns None when ffmpeg is missing or produced nothing - never raises, so a
    caller can distinguish "no box" from "could not look" via the reason string.
    """
    try:
        done = subprocess.run(  # noqa: S603 - fixed argv, no shell
            [
                "ffmpeg",
                "-v", "error",
                "-i", "pipe:0",
                "-f", "rawvideo",
                "-pix_fmt", "rgb24",
                "-",
            ],
            input=jpeg,
            capture_output=True,
            timeout=DECODE_TIMEOUT_S,
        )
    except (OSError, subprocess.SubprocessError):
        return None
    return done.stdout or None


def read_detection_overlay(
    jpeg: bytes,
    *,
    decode: Callable[[bytes, int], Optional[bytes]] = decode_rgb,
    width: int = DEFAULT_WIDTH,
) -> OverlayReading:
    """Report whether the camera's detection box is drawn on this frame.

    `decode` is injectable so tests need neither ffmpeg nor a camera.
    """
    rgb = decode(jpeg, width)
    if not rgb:
        return OverlayReading(False, 0, 0, "could not decode the frame")

    if len(rgb) % (3 * width) != 0:
        return OverlayReading(False, 0, 0, "frame is not a whole number of rows")

    height = len(rgb) // (3 * width)

    vivid = 0
    for i in range(0, len(rgb), 3):
        r, g, b = rgb[i], rgb[i + 1], rgb[i + 2]
        high, low = max(r, g, b), min(r, g, b)
        if high >= VIVID_MIN_BRIGHTNESS and (high - low) >= VIVID_MIN_SPREAD:
            vivid += 1

    runs = 0
    for y in range(height):
        row = rgb[y * width * 3:(y + 1) * width * 3]
        run = 0
        for x in range(width):
            r, g, b = row[x * 3], row[x * 3 + 1], row[x * 3 + 2]
            if r >= BRIGHT_LEVEL and g >= BRIGHT_LEVEL and b >= BRIGHT_LEVEL:
                run += 1
                if run == BRIGHT_RUN_PIXELS:
                    runs += 1
            else:
                run = 0

    if vivid or runs:
        detail = []
        if vivid:
            detail.append(f"{vivid} vivid pixels")
        if runs:
            detail.append(f"{runs} bright runs")
        return OverlayReading(True, vivid, runs, "overlay present: " + ", ".join(detail))

    return OverlayReading(False, 0, 0, "no overlay")


#: Set this to arm box-reading in whatever process consumes the stream.
ENV_CAMERA_READ_OVERLAY = "HG_CAMERA_READ_OVERLAY"

_TRUTHY = {"1", "true", "yes", "on", "enabled"}


def overlay_reading_enabled_from_env(dotenv_path: Optional[str] = None) -> bool:
    """Whether the box reader should run. Explicit, never inferred."""
    import os

    raw = os.environ.get(ENV_CAMERA_READ_OVERLAY)
    if raw is None:
        from camera_ingest.source import default_dotenv_path, load_dotenv

        raw = load_dotenv(dotenv_path or default_dotenv_path()).get(ENV_CAMERA_READ_OVERLAY)
    return (raw or "").strip().lower() in _TRUTHY
