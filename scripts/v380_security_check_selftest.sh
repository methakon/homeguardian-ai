#!/usr/bin/env bash
# v380_security_check_selftest.sh - proves the classifier in
# scripts/v380_security_check.sh works, and that each of its four
# classifications is actually reachable.
#
# WHY: a checker that has only ever printed "DISABLED" against one camera has not
# been tested - it might be unable to say anything else. This harness starts local
# stub servers on 127.0.0.1 that deliberately behave in each way, runs the REAL
# checker against them, and asserts both the classification and the exit code.
#
# Nothing here touches a camera or the network: every stub binds to 127.0.0.1.
#
# Usage: bash scripts/v380_security_check_selftest.sh

set -uo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
CHECK="$HERE/v380_security_check.sh"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"; [ -f "$WORK/stub.pid" ] && kill "$(cat "$WORK/stub.pid")" 2>/dev/null' EXIT

if [ ! -x "$CHECK" ]; then
    echo "FAIL: checker not found or not executable: $CHECK" >&2
    exit 2
fi

cat > "$WORK/stub.py" <<'PY'
"""Stub listeners that behave in each way the checker must classify."""
import hashlib, os, re, socket, sys, threading, time

MODE = sys.argv[1]          # exposed | enforced_good | enforced_bad | mute
RTSP, ONVIF = int(sys.argv[2]), int(sys.argv[3])
USER, REALM, NONCE, PW = 'admin', 'cam', 'deadbeefcafe', 'correct-horse'

def md5(x): return hashlib.md5(x.encode()).hexdigest()
def ha1(u, p): return md5(f'{u}:{REALM}:{p}')

def digest_ok(header, method, uri):
    if not header or not header.lower().startswith('digest'):
        return False
    p = {}
    for m in re.finditer(r'(\w+)\s*=\s*(?:"([^"]*)"|([^,\s]+))', header[len('Digest'):]):
        p[m.group(1).lower()] = m.group(2) if m.group(2) is not None else m.group(3)
    if p.get('nonce') != NONCE or p.get('realm') != REALM:
        return False
    expected = md5(f'{ha1(p.get("username",""), PW)}:{NONCE}:{md5(f"{method}:{uri}")}')
    return p.get('response') == expected

def basic_ok(header):
    if not header or not header.lower().startswith('basic'):
        return False
    import base64
    try:
        u, p = base64.b64decode(header.split()[-1]).decode().split(':', 1)
    except Exception:
        return False
    return u == USER and p == PW

SDP = ('v=0\r\ns=stub\r\nm=video 0 RTP/AVP 96\r\na=rtpmap:96 H265/90000\r\na=control:track1\r\n')

def rtsp_conn(c):
    c.settimeout(10)
    buf = b''
    authed = False
    try:
        while True:
            try:
                d = c.recv(4096)
            except socket.timeout:
                break
            if not d:
                break
            buf += d
            while b'\r\n\r\n' in buf:
                head, buf = buf.split(b'\r\n\r\n', 1)
                txt = head.decode(errors='replace')
                lines = txt.split('\r\n')
                method, uri = (lines[0].split() + ['', ''])[:2]
                auth = ''
                for l in lines[1:]:
                    if l.lower().startswith('authorization:'):
                        auth = l.split(':', 1)[1].strip()
                if MODE == 'mute':
                    continue                      # accept, never answer
                if MODE.startswith('enforced'):
                    good = digest_ok(auth, method, uri) or basic_ok(auth)
                    if MODE == 'enforced_bad':
                        good = False
                    if not good:
                        c.sendall(b'RTSP/1.0 401 Unauthorized\r\nCSeq: 1\r\n'
                                  b'WWW-Authenticate: Digest realm="cam", nonce="deadbeefcafe"\r\n\r\n')
                        continue
                    authed = True
                if method == 'DESCRIBE':
                    body = SDP.encode()
                    c.sendall(b'RTSP/1.0 200 OK\r\nCSeq: 1\r\nContent-Type: application/sdp\r\n'
                              + f'Content-Length: {len(body)}\r\n'.encode()
                              + f'Content-Base: rtsp://127.0.0.1:{RTSP}/live/ch00_1/\r\n\r\n'.encode()
                              + body)
                elif method == 'SETUP':
                    c.sendall(b'RTSP/1.0 200 OK\r\nCSeq: 3\r\nSession: 12345678\r\n'
                              b'Transport: RTP/AVP/TCP;unicast;interleaved=0-1\r\n\r\n')
                elif method == 'PLAY':
                    c.sendall(b'RTSP/1.0 200 OK\r\nCSeq: 4\r\nSession: 12345678\r\n\r\n')
                    # emit interleaved RTP so the packet counter sees real traffic
                    for i in range(12):
                        payload = b'\x80\x60' + b'\x00' * 10 + b'\x00' * 20
                        frame = b'\x24\x00' + len(payload).to_bytes(2, 'big') + payload
                        c.sendall(frame)
                        time.sleep(0.02)
                else:
                    c.sendall(b'RTSP/1.0 200 OK\r\nCSeq: 9\r\n\r\n')
    except OSError:
        pass
    finally:
        try: c.close()
        except Exception: pass

def rtsp_loop():
    s = socket.socket(); s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind(('127.0.0.1', RTSP)); s.listen(8)
    while True:
        try:
            c, _ = s.accept()
        except OSError:
            return
        threading.Thread(target=rtsp_conn, args=(c,), daemon=True).start()

RESP = ('<?xml version="1.0" encoding="UTF-8"?><s:Envelope '
        'xmlns:s="http://www.w3.org/2003/05/soap-envelope"><s:Body>'
        '<tds:GetDeviceInformationResponse xmlns:tds="http://www.onvif.org/ver10/device/wsdl">'
        '<tds:Manufacturer>stub</tds:Manufacturer></tds:GetDeviceInformationResponse>'
        '</s:Body></s:Envelope>')

def onvif_conn(c):
    c.settimeout(10)
    buf = b''
    try:
        while b'\r\n\r\n' not in buf:
            d = c.recv(4096)
            if not d:
                return
            buf += d
        head, _, rest = buf.partition(b'\r\n\r\n')
        txt = head.decode(errors='replace')
        m = re.search(r'(?i)content-length:\s*(\d+)', txt)
        need = int(m.group(1)) if m else 0
        while len(rest) < need:
            rest += c.recv(4096)
        body = rest.decode(errors='replace')
        auth = ''
        for l in txt.split('\r\n'):
            if l.lower().startswith('authorization:'):
                auth = l.split(':', 1)[1].strip()
        if MODE == 'mute':
            return
        if MODE.startswith('enforced'):
            good = digest_ok(auth, 'POST', '/onvif/device_service')
            if MODE == 'enforced_bad':
                good = False
            if not good:
                # a real device also accepts WS-Security; the stub only accepts HTTP Digest
                c.sendall(b'HTTP/1.1 401 Unauthorized\r\n'
                          b'WWW-Authenticate: Digest realm="cam", nonce="deadbeefcafe"\r\n'
                          b'Content-Length: 0\r\nConnection: close\r\n\r\n')
                return
        payload = RESP.encode()
        c.sendall(b'HTTP/1.1 200 OK\r\nContent-Type: application/soap+xml\r\n'
                  + f'Content-Length: {len(payload)}\r\nConnection: close\r\n\r\n'.encode() + payload)
    except OSError:
        pass
    finally:
        try: c.close()
        except Exception: pass

def onvif_loop():
    s = socket.socket(); s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind(('127.0.0.1', ONVIF)); s.listen(8)
    while True:
        try:
            c, _ = s.accept()
        except OSError:
            return
        threading.Thread(target=onvif_conn, args=(c,), daemon=True).start()

if MODE != 'closed':
    threading.Thread(target=rtsp_loop, daemon=True).start()
    threading.Thread(target=onvif_loop, daemon=True).start()
print('ready', flush=True)
while True:
    time.sleep(1)
PY

cat > "$WORK/pick_port.py" <<'PY'
import socket, sys
s = socket.socket(); s.bind(('127.0.0.1', 0)); print(s.getsockname()[1]); s.close()
PY

PASS=0; FAIL=0
run_case () {
    local name="$1" mode="$2" expect_exit="$3" expect_text="$4" use_creds="$5"
    local rport oport
    [ "$mode" = "closed" ] && { rport=1; oport=1; } || { rport="$(python3 "$WORK/pick_port.py")"; oport="$(python3 "$WORK/pick_port.py")"; }
    if [ "$mode" != "closed" ]; then
        python3 "$WORK/stub.py" "$mode" "$rport" "$oport" > "$WORK/stub.log" 2>&1 &
        echo $! > "$WORK/stub.pid"
        for _ in $(seq 20); do grep -q ready "$WORK/stub.log" && break; sleep 0.1; done
    fi
    local credargs=()
    if [ "$use_creds" = "yes" ]; then
        printf '{"username":"admin","password":"correct-horse"}' > "$WORK/cred.json"
        chmod 600 "$WORK/cred.json"
        credargs=(--creds "$WORK/cred.json")
    fi
    local out rc
    out="$(bash "$CHECK" 127.0.0.1 "$rport" "$oport" "${credargs[@]+"${credargs[@]}"}" 2>&1)"
    rc=$?
    local ok=1
    [ "$rc" = "$expect_exit" ] || ok=0
    printf '%s' "$out" | grep -q "$expect_text" || ok=0
    if [ "$ok" = "1" ]; then
        echo "  PASS  $name  (exit $rc, saw \"$expect_text\")"
        PASS=$((PASS+1))
    else
        echo "  FAIL  $name  (exit $rc, expected $expect_exit and \"$expect_text\")"
        printf '%s\n' "$out" | sed 's/^/        | /'
        FAIL=$((FAIL+1))
    fi
    [ -f "$WORK/stub.pid" ] && kill "$(cat "$WORK/stub.pid")" 2>/dev/null; rm -f "$WORK/stub.pid"
    sleep 0.3
}

echo "== v380_security_check self-test (local stubs only; no camera contacted) =="
echo
echo "-- no listener at all --"
run_case "DISABLED  (closed ports)"        closed        0  "DISABLED"  no
echo "-- listener answers without credentials --"
run_case "EXPOSED   (anonymous media)"     exposed       1  "EXPOSED"   no
echo "-- listener demands auth, correct credentials supplied --"
run_case "ENFORCED  + authorized CONFIRMED" enforced_good 0  "authorized access CONFIRMED" yes
echo "-- listener demands auth, credentials supplied but rejected --"
run_case "ENFORCED  + authorized NOT CONFIRMED" enforced_bad 3 "NOT CONFIRMED" yes
echo "-- listener demands auth, no credentials supplied --"
run_case "ENFORCED  + authorized NOT ATTEMPTED" enforced_good 0 "NOT ATTEMPTED" no
echo "-- listener accepts but never answers --"
run_case "INCONCLUSIVE (mute listener)"    mute          3  "INCONCLUSIVE" no
echo
echo "== result: $PASS passed, $FAIL failed =="
[ "$FAIL" = "0" ] || exit 1
exit 0
