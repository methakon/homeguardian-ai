"""MAC-anchored recovery for a camera whose address may move.

Why this exists
---------------
A DHCP lease change, a router restart or a camera reboot can move the camera to a
different address. Addresses are not identity: the thing that identifies the
camera is its **MAC**. So recovery is anchored to the MAC, and the address is
only ever treated as "where it was last seen".

Where this belongs
------------------
This lives next to the relay because the relay is the component that already
holds the camera credential and talks to the camera. Keeping recovery here means
a consumer (the bridge, an NVR, ffmpeg) always talks to the relay's own stable
loopback address and never has to know the camera moved.

Two hard rules, both enforced here rather than left to callers
--------------------------------------------------------------
1. **Never scan while healthy.** A scan is only considered after real failure
   evidence, so a working feed is never disturbed.
2. **Never scan more than once a minute.** The camera is a single-board device
   with a fragile daemon; a scan sweep is the most load this code will ever put
   on the LAN, so the interval is a floor, not a suggestion.

Nothing here reads or writes a credential, invokes a vendor configuration route,
or logs anything that identifies the camera beyond its address.
"""

from __future__ import annotations

import os
import re
import socket
import subprocess
import time
from dataclasses import dataclass, field
from typing import Any, Callable, Iterable, Iterator, Optional, Protocol

#: A MAC address, in the colon- or dash-separated form `ip neigh` reports.
_MAC_RE = re.compile(r"^[0-9a-f]{2}(:[0-9a-f]{2}){5}$", re.IGNORECASE)


class _CameraLike(Protocol):
    """The slice of a camera source this wrapper drives.

    Deliberately structural: the relay already consumes exactly this surface, so
    anything satisfying it (today `JuanHttpSource`) can be wrapped unreviewed.

    `_host` is part of the contract on purpose. Recovery is *about* the address a
    source points at, so re-pointing it is the whole job — not an accident of a
    particular implementation.
    """

    _host: str

    @property
    def name(self) -> str: ...

    def stream_url(self, *args: Any, **kwargs: Any) -> str: ...

    def probe(self, timeout: float = ...) -> Any: ...

    def snapshot(self, timeout: float = ...) -> bytes: ...

    def open_stream(self, timeout: float = ..., chunk: int = ...) -> Iterator[bytes]: ...

#: The floor between two scans. Enforced even if a caller asks for more often.
MIN_SCAN_INTERVAL_S = 60.0

#: How long a single neighbour sweep may take before it is abandoned.
DEFAULT_SWEEP_TIMEOUT_S = 12.0


def normalise_mac(value: str) -> str:
    """Return a canonical lower-case colon-separated MAC.

    Raises ValueError on anything that is not a 6-octet MAC, so a malformed
    identity fails loudly at construction instead of silently never matching.
    """
    candidate = value.strip().lower().replace("-", ":")
    if not _MAC_RE.match(candidate):
        raise ValueError(f"not a MAC address: {value!r}")
    return candidate


def subnet_of(host: str) -> str:
    """The /24 prefix of an IPv4 address, e.g. '192.168.1.13' -> '192.168.1.'."""
    parts = host.split(".")
    if len(parts) != 4:
        raise ValueError(f"not an IPv4 address: {host!r}")
    return ".".join(parts[:3]) + "."


def parse_neigh_table(text: str) -> dict[str, str]:
    """Map MAC -> IPv4 from `ip neigh show` output.

    Only lines that carry both an address and a link-layer address participate.
    Incomplete entries (no `lladdr`) are skipped: they tell us nothing.
    """
    found: dict[str, str] = {}
    for line in text.splitlines():
        fields = line.split()
        if not fields or "lladdr" not in fields:
            continue
        try:
            mac_index = fields.index("lladdr") + 1
            mac = normalise_mac(fields[mac_index])
        except (ValueError, IndexError):
            continue
        address = fields[0]
        if address.count(".") == 3:
            found[mac] = address
    return found


def _run(cmd: list[str], timeout: float) -> str:
    """Run a read-only command, returning stdout, or '' if it did not work."""
    try:
        done = subprocess.run(  # noqa: S603 - fixed argv, no shell
            cmd,
            capture_output=True,
            text=True,
            timeout=timeout,
            check=False,
        )
    except (OSError, subprocess.SubprocessError):
        return ""
    return done.stdout or ""


def read_neigh_table(timeout: float = 5.0) -> str:
    """The kernel's own neighbour table. Cheap: this is a read, not a scan."""
    return _run(["ip", "neigh", "show"], timeout)


def sweep_subnet(subnet: str, timeout: float = DEFAULT_SWEEP_TIMEOUT_S) -> None:
    """Touch every host in a /24 so the kernel populates its neighbour table.

    This is the ONLY part of recovery that puts load on the LAN, which is exactly
    why it runs at most once a minute and never while the camera is answering.

    A single empty UDP datagram per address is enough to provoke an ARP
    resolution; nothing waits for a reply, so the sweep is bounded by the number
    of addresses rather than by how many of them are alive — a dark subnet
    finishes just as fast as a fully populated one.
    """
    deadline = time.monotonic() + max(timeout, 1.0)
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        for last in range(1, 255):
            if time.monotonic() >= deadline:
                break
            try:
                sock.sendto(b"", (f"{subnet}{last}", 9))  # discard port
            except OSError:
                # Unreachable or refused writes are expected and irrelevant: the
                # ARP lookup has already happened by the time we get here.
                continue
    finally:
        sock.close()
    # Let the kernel record the replies it collected before we read the table.
    time.sleep(min(0.5, max(0.0, deadline - time.monotonic())))


@dataclass
class DiscoveryOutcome:
    """The result of one recovery attempt, in terms a caller can log safely."""

    found: bool
    host: Optional[str] = None
    reason: str = ""
    scanned: bool = False

    def __bool__(self) -> bool:  # pragma: no cover - trivial
        return self.found


@dataclass
class CameraLocator:
    """Finds where a MAC-anchored camera currently lives, at most once a minute.

    Injectable seams (`neigh_reader`, `sweep`, `clock`) exist so the behaviour is
    testable without a real LAN and without touching this machine's network.
    """

    mac: str
    last_host: Optional[str] = None
    min_scan_interval_s: float = MIN_SCAN_INTERVAL_S
    neigh_reader: Callable[[], str] = field(default=read_neigh_table)
    #: Defaults to the real sweep, so constructing a locator is enough to
    #: recover. Tests inject their own to stay off the network.
    sweep: Optional[Callable[[str, float], None]] = field(default=sweep_subnet)
    clock: Callable[[], float] = field(default=time.monotonic)
    _last_scan_at: Optional[float] = field(default=None, init=False, repr=False)
    _scan_count: int = field(default=0, init=False, repr=False)
    #: The most recent reason a caller may surface in a log line.
    last_reason: str = field(default="", init=False)

    def __post_init__(self) -> None:
        self.mac = normalise_mac(self.mac)

    # -- bookkeeping ------------------------------------------------------ #

    def note_working_host(self, host: str) -> None:
        """Record an address that just proved it works. This is the identity anchor."""
        self.last_host = host

    @property
    def scan_count(self) -> int:
        """How many sweeps have actually run. Tests assert on this."""
        return self._scan_count

    def _interval_elapsed(self) -> bool:
        if self._last_scan_at is None:
            return True
        return (self.clock() - self._last_scan_at) >= self.min_scan_interval_s

    def _lookup_cached(self) -> Optional[str]:
        """Ask the neighbour table first: it is a read, and usually enough."""
        try:
            table = parse_neigh_table(self.neigh_reader())
        except Exception:  # noqa: BLE001 - discovery must never raise into I/O
            return None
        return table.get(self.mac)

    def _lookup_swept(self, subnet: str) -> Optional[str]:
        """Sweep the /24, then re-read the neighbour table."""
        if self.sweep is None:
            return None
        try:
            self.sweep(subnet, DEFAULT_SWEEP_TIMEOUT_S)
        except Exception:  # noqa: BLE001 - best effort only
            return None
        return self._lookup_cached()

    # -- the gate --------------------------------------------------------- #

    def maybe_rediscover(self, *, had_failure: bool, live: bool) -> DiscoveryOutcome:
        """Try to repair a moved address. Refuses unless the camera really failed.

        `had_failure` must be evidence of a real failed fetch; `live` must be
        False. Without both, this returns a refusal and touches nothing — a
        healthy feed is never probed, and a scan never runs speculatively.
        """
        if live:
            self.last_reason = "live"
            return DiscoveryOutcome(False, reason="refused: camera is live")
        if not had_failure:
            self.last_reason = "no_failure_evidence"
            return DiscoveryOutcome(False, reason="refused: no failure evidence")

        # A neighbour-table read is free, so it is allowed even inside the
        # interval: it cannot disturb the camera or the LAN.
        cached = self._lookup_cached()
        if cached is not None:
            self.last_host = cached
            self.last_reason = "found_in_neighbour_table"
            return DiscoveryOutcome(True, cached, self.last_reason, scanned=False)

        if not self._interval_elapsed():
            self.last_reason = "rate_limited"
            return DiscoveryOutcome(False, reason="refused: scan interval not elapsed")

        if self.last_host is None:
            self.last_reason = "no_subnet"
            return DiscoveryOutcome(False, reason="refused: no known subnet to sweep")

        self._last_scan_at = self.clock()
        self._scan_count += 1
        subnet = subnet_of(self.last_host)
        found = self._lookup_swept(subnet)
        if found is not None:
            self.last_host = found
            self.last_reason = "found_by_sweep"
            return DiscoveryOutcome(True, found, self.last_reason, scanned=True)

        self.last_reason = "not_found"
        return DiscoveryOutcome(False, reason="not found on subnet", scanned=True)


class RecoveringCameraSource:
    """Wrap a camera source so a moved address repairs itself.

    Implements the same surface the relay already consumes (`name`,
    `stream_url`, `probe`, `open_stream`, `snapshot`), so the relay needs no
    change: it keeps talking to one object while the object follows the camera.
    """

    #: Consecutive failed fetches before recovery is attempted. One is enough:
    #: a single success immediately clears the streak.
    FAILURES_BEFORE_RECOVERY = 1

    def __init__(
        self,
        inner: "_CameraLike",
        locator: CameraLocator,
        host_factory: Optional[Callable[[str], "_CameraLike"]] = None,
    ) -> None:
        self._inner = inner
        self._locator = locator
        self._host_factory = host_factory
        self._consecutive_failures = 0
        # Seed the anchor with wherever we start, so a move has a subnet to sweep.
        initial = getattr(inner, "_host", None)
        if initial:
            locator.note_working_host(initial)

    # -- introspection ---------------------------------------------------- #

    @property
    def _host(self) -> str:
        """The address currently in use. Mirrors the inner source's contract."""
        return self._inner._host  # noqa: SLF001 - mirrors the wrapped contract

    @property
    def name(self) -> str:
        return self._inner.name

    @property
    def consecutive_failures(self) -> int:
        return self._consecutive_failures

    @property
    def state(self) -> str:
        return "LIVE" if self._consecutive_failures == 0 else "DISCONNECTED"

    def stream_url(self, *args, **kwargs):
        return self._inner.stream_url(*args, **kwargs)

    # -- recovery --------------------------------------------------------- #

    def _note_failure(self) -> None:
        self._consecutive_failures += 1

    def _note_success(self, host: Optional[str]) -> None:
        self._consecutive_failures = 0
        if host:
            self._locator.note_working_host(host)

    def _current_host(self) -> Optional[str]:
        return getattr(self._inner, "_host", None)

    def _repoint(self) -> bool:
        """Swap the inner source over to the camera's current address."""
        outcome = self._locator.maybe_rediscover(
            had_failure=self._consecutive_failures >= self.FAILURES_BEFORE_RECOVERY,
            live=self._consecutive_failures == 0,
        )
        if not outcome.found or outcome.host is None:
            return False
        if self._host_factory is None:
            self._inner._host = outcome.host  # noqa: SLF001 - the seam is the point
        else:
            self._inner = self._host_factory(outcome.host)
        return True

    def _call(self, method: str, *args, **kwargs):
        """Run an inner call, repairing the address once if it fails."""
        try:
            result = getattr(self._inner, method)(*args, **kwargs)
        except Exception:
            self._note_failure()
            if self._consecutive_failures >= self.FAILURES_BEFORE_RECOVERY and self._repoint():
                result = getattr(self._inner, method)(*args, **kwargs)
                self._note_success(self._current_host())
                return result
            raise
        self._note_success(self._current_host())
        return result

    # -- the CameraSource surface ----------------------------------------- #

    def probe(self, timeout: float = 8.0):
        return self._call("probe", timeout=timeout)

    def snapshot(self, timeout: float = 8.0) -> bytes:
        return self._call("snapshot", timeout=timeout)

    def open_stream(self, timeout: float = 10.0, chunk: int = 32 * 1024) -> Iterable[bytes]:
        # The first chunk is the point of failure for a stream, so pull one
        # eagerly and only then hand back a generator. That way recovery happens
        # before the caller has committed to any bytes.
        iterator = None
        try:
            iterator = self._inner.open_stream(timeout=timeout, chunk=chunk)
            first = next(iterator)
        except Exception:
            self._note_failure()
            if self._repoint():
                iterator = self._inner.open_stream(timeout=timeout, chunk=chunk)
                first = next(iterator)
                self._note_success(self._current_host())
            else:
                raise
        else:
            self._note_success(self._current_host())

        def _rest():
            assert iterator is not None
            for chunk_bytes in iterator:
                yield chunk_bytes

        return _chain_first(first, _rest())

    def __repr__(self) -> str:  # pragma: no cover - trivial
        return (
            f"RecoveringCameraSource(state={self.state}, "
            f"failures={self._consecutive_failures}, mac={self._locator.mac})"
        )


def _chain_first(first: bytes, rest: Iterable[bytes]) -> Iterable[bytes]:
    """Yield `first`, then everything from `rest`."""
    yield first
    yield from rest


#: Enrolling a MAC is the operator's statement of camera identity. Setting this
#: is what ARMS recovery; without it nothing ever scans.
ENV_CAMERA_MAC = "HG_CAMERA_MAC"


def recovering_source_from_env(
    inner: Optional["_CameraLike"] = None,
    *,
    mac: Optional[str] = None,
    dotenv_path: Optional[str] = None,
) -> "_CameraLike":
    """Wrap a camera source with MAC-anchored recovery when a MAC is enrolled.

    Deliberately opt-in. With no MAC configured the source is returned untouched,
    so recovery can never scan by accident: a camera whose identity has not been
    declared is simply not recovered, rather than guessed at by address.
    """
    from camera_ingest.source import CameraCredential, JuanHttpSource, default_dotenv_path

    resolved_mac = mac or os.environ.get(ENV_CAMERA_MAC)
    if resolved_mac is None and dotenv_path is not None:
        # Reuse the same parser the credential loader uses, so the .env is read
        # one way in this codebase, not two.
        from camera_ingest.source import load_dotenv

        resolved_mac = load_dotenv(dotenv_path).get(ENV_CAMERA_MAC)
    if not resolved_mac:
        return inner if inner is not None else JuanHttpSource(
            credential=CameraCredential.from_env(default_dotenv_path())
        )

    if inner is None:
        inner = JuanHttpSource(credential=CameraCredential.from_env(default_dotenv_path()))

    locator = CameraLocator(mac=resolved_mac, last_host=getattr(inner, "_host", None))
    return RecoveringCameraSource(inner, locator)
