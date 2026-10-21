#!/usr/bin/env python3
"""Query Android's wireless-debugging mDNS services directly, stdlib only.

`adb mdns services` depends on a working mDNS path between host and device. On a
consumer AP that path is often broken (multicast filtered, or adb's own mDNS
client not wired to the host's resolver). This does the query itself so the
answer is unambiguous: either the device answers over multicast, or it does not.

Queries the two services Android 11 advertises while wireless debugging is on:
  _adb-tls-connect._tcp.local   -> the port for `adb connect`  (persistent)
  _adb-tls-pairing._tcp.local   -> only while the pairing dialog is open

Prints one JSON object so the caller can consume it.
Exit 0 if any _adb-tls-connect record was found, 1 if none, 2 on socket error.
"""
import json
import socket
import struct
import sys
import time

MDNS_ADDR = "224.0.0.251"
MDNS_PORT = 5353
SERVICES = ["_adb-tls-connect._tcp.local", "_adb-tls-pairing._tcp.local"]


def encode_name(name: str) -> bytes:
    out = b""
    for label in name.rstrip(".").split("."):
        out += bytes([len(label)]) + label.encode()
    return out + b"\x00"


def build_query(name: str, qtype: int = 12) -> bytes:
    # ID 0, flags 0, QDCOUNT 1. QU bit set in QCLASS so a unicast reply is allowed.
    hdr = struct.pack("!HHHHHH", 0, 0, 1, 0, 0, 0)
    q = encode_name(name) + struct.pack("!HH", qtype, 0x8001)
    return hdr + q


def decode_name(data: bytes, off: int) -> tuple:
    labels, guard = [], 0
    while off < len(data) and guard < 64:
        ln = data[off]
        if ln == 0:
            off += 1
            break
        if ln & 0xC0 == 0xC0:                      # compression pointer
            off = struct.unpack("!H", data[off:off + 2])[0] & 0x3FFF
            guard += 1
            continue
        labels.append(data[off + 1:off + 1 + ln].decode("utf-8", "replace"))
        off += 1 + ln
        guard += 1
    return ".".join(labels), off


def parse(data: bytes) -> list:
    """Return [(name, rtype, rdata_repr, port_or_None, target_or_None)]."""
    out = []
    if len(data) < 12:
        return out
    qd, an, ns, ar = struct.unpack("!HHHH", data[4:12])
    off = 12
    for _ in range(qd):                            # skip questions
        _, off = decode_name(data, off)
        off += 4
    for _ in range(an + ns + ar):
        if off >= len(data):
            break
        name, off = decode_name(data, off)
        if off + 10 > len(data):
            break
        rtype, _cls, _ttl, rdlen = struct.unpack("!HHIH", data[off:off + 10])
        off += 10
        rd = data[off:off + rdlen]
        port = target = None
        if rtype == 12:                            # PTR
            target, _ = decode_name(data, off)
        elif rtype == 33 and len(rd) >= 6:         # SRV: prio, weight, port, target
            port = struct.unpack("!H", rd[2:4])[0]
            target, _ = decode_name(data, off + 6)
        elif rtype == 1 and len(rd) == 4:          # A
            target = socket.inet_ntoa(rd)
        out.append((name, rtype, rd[:40].hex(), port, target))
        off += rdlen
    return out


def main() -> int:
    host = sys.argv[1] if len(sys.argv) > 1 else None
    wait = float(sys.argv[2]) if len(sys.argv) > 2 else 6.0
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        s.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_TTL, 2)
        try:
            s.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_LOOP, 1)
        except OSError:
            pass
        try:
            s.bind(("", MDNS_PORT))                # mDNS expects source port 5353
        except OSError:
            s.bind(("", 0))
        s.settimeout(0.5)
    except OSError as e:
        print(json.dumps({"error": f"socket: {e}"}))
        return 2

    for svc in SERVICES:
        try:
            s.sendto(build_query(svc), (MDNS_ADDR, MDNS_PORT))
        except OSError as e:
            print(json.dumps({"error": f"send: {e}"}))
            return 2

    records, deadline = [], time.time() + wait
    while time.time() < deadline:
        try:
            data, addr = s.recvfrom(9000)
        except socket.timeout:
            continue
        except OSError:
            break
        for name, rtype, rd, port, target in parse(data):
            records.append({
                "from": addr[0],
                "name": name,
                "type": {1: "A", 12: "PTR", 33: "SRV", 16: "TXT"}.get(rtype, str(rtype)),
                "port": port,
                "target": target,
            })

    connect_ports, connect_targets = [], []
    for r in records:
        if "_adb-tls-connect" in r["name"] or (r["target"] and "_adb-tls-connect" in str(r["target"])):
            if r["port"]:
                connect_ports.append(r["port"])
            if r["target"] and r["type"] in ("A", "SRV"):
                connect_targets.append(r["target"])

    # Only accept a port advertised by the device we are asking about, when known.
    result = {
        "responders": sorted({r["from"] for r in records}),
        "records": len(records),
        "tls_connect_ports": sorted(set(connect_ports)),
        "tls_connect_targets": sorted(set(connect_targets)),
        "all": records[:40],
    }
    if host:
        result["mdns_works_for_host"] = host in {r["from"] for r in records} or bool(connect_ports)
    print(json.dumps(result, indent=2))
    return 0 if connect_ports else 1


if __name__ == "__main__":
    sys.exit(main())
