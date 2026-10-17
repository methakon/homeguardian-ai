#!/usr/bin/env bash
# v380_probe.sh - bounded, read-only discovery probe for a V380 / Macro-video camera.
#
# Reproduces the observations recorded in docs/HG_V380_01_DISCOVERY.md and
# docs/HG_V380_02_MEDIA_PROTOCOLS.md so the discovery does not need the agent.
#
# SAFETY CONTRACT (do not relax without an explicit owner decision):
#   * requires the camera address as an explicit argument - no subnet sweep
#   * probes a FIXED list of ports; no full-range sweep, no mass scanning
#   * sends NO credential, NO login packet and NO proprietary protocol frame
#     (the only payloads are an HTTP GET, an RTSP OPTIONS and 16 junk bytes,
#      all of which the camera answers with a silent close)
#   * changes nothing: no device setting, no router setting, no port forwarding
#
# Usage: bash scripts/v380_probe.sh <camera-ip>

set -uo pipefail

CAM="${1:-}"
if [ -z "$CAM" ]; then
    echo "usage: bash scripts/v380_probe.sh <camera-ip>" >&2
    echo "  e.g. bash scripts/v380_probe.sh 192.168.1.19" >&2
    exit 2
fi

echo "== v380_probe: target $CAM (read-only, no credentials) =="
echo

python3 - "$CAM" <<'PY'
import socket, sys, time

CAM = sys.argv[1]
# Fixed, bounded candidate list: RTSP, ONVIF, HTTP, UPnP, and the Macro-video ports.
PORTS = [80, 443, 554, 8554, 3702, 5000, 5050, 5051, 8000, 8080, 8899, 34567, 8800, 9800]

print('-- TCP port state (RST => refused, timeout => filtered) --')
open_ports = []
for p in PORTS:
    s = socket.socket(); s.settimeout(2.0)
    t0 = time.time()
    try:
        s.connect((CAM, p))
        open_ports.append(p)
        print(f'   {p:6d}  OPEN')
    except socket.timeout:
        print(f'   {p:6d}  filtered (no answer in 2.0s)')
    except OSError as e:
        print(f'   {p:6d}  refused (errno={e.errno}, {time.time()-t0:.2f}s)')
    finally:
        s.close()

if not open_ports:
    print('\n   no open port in the candidate list -> this is not a reachable V380 surface')
    sys.exit(0)

print(f'\n-- protocol identification on {open_ports} --')
for p in open_ports:
    # 1. server-first banner?
    s = socket.socket(); s.settimeout(3.0)
    try:
        s.connect((CAM, p))
        try:
            d = s.recv(64)
            print(f'   {p}: server-first data {len(d)} bytes')
        except socket.timeout:
            print(f'   {p}: silent after connect (waits for a client hello)')
    except OSError as e:
        print(f'   {p}: connect failed errno={e.errno}')
    finally:
        s.close()

    # 2. HTTP probe
    s = socket.socket(); s.settimeout(3.0)
    try:
        s.connect((CAM, p))
        s.sendall(f'GET / HTTP/1.1\r\nHost: {CAM}\r\n\r\n'.encode())
        d = s.recv(256)
        print(f'   {p}: HTTP GET -> {len(d)} bytes' + (f' {d[:32]!r}' if d else ' (closed, not HTTP)'))
    except OSError as e:
        print(f'   {p}: HTTP GET failed errno={e.errno}')
    finally:
        s.close()

    # 3. RTSP probe
    s = socket.socket(); s.settimeout(3.0)
    try:
        s.connect((CAM, p))
        s.sendall(f'OPTIONS rtsp://{CAM}:{p}/ RTSP/1.0\r\nCSeq: 1\r\n\r\n'.encode())
        d = s.recv(256)
        print(f'   {p}: RTSP OPTIONS -> {len(d)} bytes' + (f' {d[:32]!r}' if d else ' (closed, not RTSP)'))
    except OSError as e:
        print(f'   {p}: RTSP OPTIONS failed errno={e.errno}')
    finally:
        s.close()

print(f'\n-- lifecycle behaviour of {open_ports[0]} (no credentials sent) --')
def close_time(payload=None, timeout=15.0):
    s = socket.socket(); s.settimeout(timeout)
    s.connect((CAM, open_ports[0])); t0 = time.time()
    try:
        if payload is not None:
            s.sendall(payload)
        d = s.recv(64)
        return round(time.time() - t0, 2), len(d)
    except socket.timeout:
        return None, None
    finally:
        s.close()

for i in range(3):
    t, n = close_time()
    print(f'   idle trial {i+1}: closed by device after {t}s, bytes received {n}')

for i in range(3):
    t, n = close_time(payload=b'\xde\xad\xbe\xef' * 4)
    print(f'   junk trial {i+1}: closed by device after {t}s, bytes received {n}')

socks = []
try:
    for _ in range(6):
        x = socket.socket(); x.settimeout(2.0)
        x.connect((CAM, open_ports[0])); socks.append(x)
    print(f'   concurrency: {len(socks)} simultaneous connections accepted')
except OSError as e:
    print(f'   concurrency: {len(socks)} accepted, then errno={e.errno}')
finally:
    for x in socks:
        x.close()

print('\n-- expected result for an as-shipped V380 (see HG_V380_02) --')
print('   OPEN: 8800, 9800 only; 554/8899/3702/80 refused')
print('   silent on connect; idle close ~5.0s; junk close ~0.2s; 0 bytes back')
print('   any deviation is a finding: record it before acting on it')
PY
