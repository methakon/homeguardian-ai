#!/usr/bin/env python3
"""Resolve Android's wireless-debugging mDNS service to an ip:port. Stdlib only.

WHY THIS EXISTS instead of `adb mdns services`:
  Measured on this LAN (Mi A3 + Airtel F670LV9.0):
    * a raw multicast mDNS query DOES get answered by the phone;
    * `adb mdns services` lists NOTHING, and `resolvectl service` fails.
  So the network is fine and adb's own mDNS client is the broken part. The fix is
  to do the query ourselves rather than declare mDNS unusable - a mistake made once
  already in this workstream, from a test that ran while wireless debugging was off.

Android advertises:
  _adb-tls-connect._tcp.local   -> PTR -> adb-<SERIAL>-<rand>._adb-tls-connect._tcp.local
                                   SRV -> port (+ target host), A -> address
  _adb-tls-pairing._tcp.local   -> same shape, only while the pairing dialog is open

The instance name embeds the device SERIAL, so discovery can also verify that it
found the intended handset and not another phone on the LAN.

Full resolution is two-stage: PTR to get the instance, then SRV/A for the instance.

Usage: edge_dev_mdns_resolve.py [--serial SERIAL] [--timeout SECONDS] [--json]
Exit: 0 = an endpoint was resolved, 1 = none, 2 = socket error.
"""
import argparse
import json
import socket
import struct
import sys
import time

MDNS_ADDR = "224.0.0.251"
MDNS_PORT = 5353
CONNECT_SVC = "_adb-tls-connect._tcp.local"
PAIRING_SVC = "_adb-tls-pairing._tcp.local"

QTYPE_A, QTYPE_PTR, QTYPE_SRV = 1, 12, 33


def enc_name(name: str) -> bytes:
    out = b""
    for label in name.rstrip(".").split("."):
        out += bytes([len(label)]) + label.encode()
    return out + b"\x00"


def build_query(name: str, qtype: int) -> bytes:
    return (struct.pack("!HHHHHH", 0, 0, 1, 0, 0, 0)
            + enc_name(name) + struct.pack("!HH", qtype, 0x8001))


def dec_name(data: bytes, off: int):
    labels, guard = [], 0
    while off < len(data) and guard < 64:
        ln = data[off]
        if ln == 0:
            off += 1
            break
        if ln & 0xC0 == 0xC0:
            off = struct.unpack("!H", data[off:off + 2])[0] & 0x3FFF
            guard += 1
            continue
        labels.append(data[off + 1:off + 1 + ln].decode("utf-8", "replace"))
        off += 1 + ln
        guard += 1
    return ".".join(labels), off


def parse(data: bytes):
    """Yield (name, rtype, port, target)."""
    if len(data) < 12:
        return
    qd, an, ns, ar = struct.unpack("!HHHH", data[4:12])
    off = 12
    for _ in range(qd):
        _, off = dec_name(data, off)
        off += 4
    for _ in range(an + ns + ar):
        if off >= len(data):
            return
        name, off = dec_name(data, off)
        if off + 10 > len(data):
            return
        rtype, _c, _t, rdlen = struct.unpack("!HHIH", data[off:off + 10])
        off += 10
        rd = data[off:off + rdlen]
        port = target = None
        if rtype == QTYPE_PTR:
            target, _ = dec_name(data, off)
        elif rtype == QTYPE_SRV and len(rd) >= 6:
            port = struct.unpack("!H", rd[2:4])[0]
            target, _ = dec_name(data, off + 6)
        elif rtype == QTYPE_A and len(rd) == 4:
            target = socket.inet_ntoa(rd)
        yield name, rtype, port, target
        off += rdlen


class Mdns:
    def __init__(self):
        self.s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.s.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_TTL, 2)
        try:
            self.s.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_LOOP, 1)
        except OSError:
            pass
        try:
            self.s.bind(("", MDNS_PORT))
        except OSError:
            self.s.bind(("", 0))       # avahi may hold 5353; ephemeral still gets replies
        self.s.settimeout(0.4)

    def ask(self, name: str, qtype: int, wait: float):
        try:
            self.s.sendto(build_query(name, qtype), (MDNS_ADDR, MDNS_PORT))
        except OSError as e:
            raise RuntimeError(f"send failed: {e}")
        out, end = [], time.time() + wait
        while time.time() < end:
            try:
                data, addr = self.s.recvfrom(9000)
            except socket.timeout:
                continue
            except OSError:
                break
            for rec in parse(data):
                out.append((addr[0],) + rec)
        return out


def resolve(want_serial=None, wait=4.0):
    m = Mdns()
    found = {}
    responders = set()

    for svc, kind in ((CONNECT_SVC, "connect"), (PAIRING_SVC, "pairing")):
        ptr = m.ask(svc, QTYPE_PTR, wait)
        for from_ip, _n, rtype, _p, target in ptr:
            responders.add(from_ip)
            if rtype == QTYPE_PTR and target:
                found.setdefault(target, {"instance": target, "kind": kind,
                                          "from": from_ip, "port": None, "ip": None})
        # stage 2: SRV (and A) for each instance we just learned about
        for inst in list(found):
            if found[inst]["port"]:
                continue
            for from_ip, _n, rtype, port, target in m.ask(inst, QTYPE_SRV, wait):
                responders.add(from_ip)
                if rtype == QTYPE_SRV and port:
                    found[inst]["port"] = port
                if rtype == QTYPE_A and target:
                    found[inst]["ip"] = target

    endpoints = []
    for inst, info in found.items():
        ip = info["ip"] or info["from"]
        serial_ok = True
        if want_serial:
            serial_ok = want_serial in inst
        if info["port"] and ip:
            endpoints.append({"instance": inst, "kind": info["kind"], "ip": ip,
                              "port": info["port"], "endpoint": f"{ip}:{info['port']}",
                              "serial_match": serial_ok})
    # If the A record never came back, the source address is still the device.
    for inst, info in found.items():
        if not info["port"]:
            continue
        if not any(e["instance"] == inst for e in endpoints):
            ep = f"{info['from']}:{info['port']}"
            endpoints.append({"instance": inst, "kind": info["kind"], "ip": info["from"],
                              "port": info["port"], "endpoint": ep,
                              "serial_match": (want_serial in inst) if want_serial else True})
    if want_serial:
        endpoints = [e for e in endpoints if e["serial_match"]]
    return {"responders": sorted(responders), "endpoints": endpoints,
            "instances_seen": list(found)}


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--serial", default=None, help="only accept instances naming this serial")
    ap.add_argument("--timeout", type=float, default=4.0)
    ap.add_argument("--json", action="store_true")
    a = ap.parse_args()
    try:
        res = resolve(a.serial, a.timeout)
    except RuntimeError as e:
        print(json.dumps({"error": str(e)}))
        return 2
    if a.json:
        print(json.dumps(res, indent=2))
    else:
        # stdout is a MACHINE-READABLE endpoint list; every diagnostic goes to stderr
        # so a caller parsing stdout can never mistake prose for a candidate.
        if not res["responders"]:
            print("no mDNS responders at all - multicast appears filtered here",
                  file=sys.stderr)
        for e in res["endpoints"]:
            print(f"{e['endpoint']}\t{e['kind']}\t{e['instance']}")
        if not res["endpoints"]:
            print("mDNS answered, but no _adb-tls-connect endpoint resolved "
                  "(is wireless debugging actually ON?)", file=sys.stderr)
    return 0 if res["endpoints"] else 1


if __name__ == "__main__":
    sys.exit(main())
