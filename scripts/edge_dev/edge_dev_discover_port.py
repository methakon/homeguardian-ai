#!/usr/bin/env python3
"""Bounded port discovery for Android wireless debugging - the VERIFIED FALLBACK
for when mDNS does not answer.

A fixed IP does not mean a fixed port: measured on this device, the
wireless-debugging port ROTATES when Wi-Fi reconnects (it was 41639, and after a
Wi-Fi drop the same IP refused 41639 and the toggle still showed ON). So a
discovery path that does not depend on mDNS is required.

Scope of the probe, deliberately narrow:
  * ONE host - the phone's own address, passed in; never the /24;
  * the ephemeral range Android picks from (32768-49151 by default);
  * a concurrency cap and a short timeout so it is a few seconds, not a sweep.

Every open candidate is then presented for `adb connect`, and the caller is
expected to verify identity afterwards - an open port is NOT proof of an adb
endpoint, so this script never claims one.

Usage: edge_dev_discover_port.py <ip> [lo] [hi] [--json]
Exit 0 if at least one open candidate was found, 1 if none.
"""
import asyncio
import json
import sys

DEFAULT_LO, DEFAULT_HI = 32768, 49151
CONCURRENCY = 150
TIMEOUT = 2.5


async def probe(ip: str, port: int, sem: asyncio.Semaphore, found: list) -> None:
    async with sem:
        try:
            fut = asyncio.open_connection(ip, port)
            reader, writer = await asyncio.wait_for(fut, timeout=TIMEOUT)
            found.append(port)
            writer.close()
            try:
                await writer.wait_closed()
            except Exception:
                pass
        except (OSError, asyncio.TimeoutError):
            pass


async def main_async(ip: str, lo: int, hi: int) -> list:
    sem = asyncio.Semaphore(CONCURRENCY)
    found: list = []
    await asyncio.gather(*(probe(ip, p, sem, found) for p in range(lo, hi + 1)))
    return sorted(found)


def main() -> int:
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    as_json = "--json" in sys.argv
    if not args:
        print(__doc__)
        return 2
    ip = args[0]
    lo = int(args[1]) if len(args) > 1 else DEFAULT_LO
    hi = int(args[2]) if len(args) > 2 else DEFAULT_HI

    ports = asyncio.run(main_async(ip, lo, hi))
    if as_json:
        print(json.dumps({"ip": ip, "range": [lo, hi], "open_ports": ports}))
    else:
        print(f"probed {ip} ports {lo}-{hi} ({hi - lo + 1} ports)")
        if ports:
            print(f"  open: {' '.join(str(p) for p in ports)}")
            print("  NOTE: open != adb. Each must be confirmed with `adb connect` + identity check.")
        else:
            print("  no open ports in range - the phone may be off this network, or wireless "
                  "debugging was turned off rather than merely re-ported.")
    return 0 if ports else 1


if __name__ == "__main__":
    sys.exit(main())
