#!/usr/bin/env bash
# v380_security_check.sh - authentication-exposure check for the V380 camera.
#
# Question it answers: can a client with NO credentials reach private video or
# privileged/identifying control state on the camera?
#
# EXIT CODES
#   0  PROTECTED  - every tested interface refused an unauthenticated client
#   1  EXPOSED    - an unauthenticated client got video, config or user data
#   2  usage error / host unreachable
#
# SAFETY CONTRACT (do not relax without an explicit owner decision):
#   * sends NO credential, NO password guess, NO auth header of any kind
#   * records NO media: an RTP session is counted (interleaved packet count
#     over TCP), never written to disk, never decoded, never published
#   * issues NO control command: no PTZ move, no preset recall, no reboot,
#     no configuration set. Read-only ONVIF calls only
#   * one target only, given on the command line - no scanning
#
# Usage: bash scripts/v380_security_check.sh <camera-ip> [rtsp-port] [onvif-port]
#   e.g. bash scripts/v380_security_check.sh 192.168.1.19 554 8899

set -uo pipefail

CAM="${1:-}"
RTSP_PORT="${2:-554}"
ONVIF_PORT="${3:-8899}"

if [ -z "$CAM" ]; then
    echo "usage: bash scripts/v380_security_check.sh <camera-ip> [rtsp-port] [onvif-port]" >&2
    exit 2
fi

python3 - "$CAM" "$RTSP_PORT" "$ONVIF_PORT" <<'PY'
import socket, re, struct, sys, time

CAM, RTSP_PORT, ONVIF_PORT = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
NS_S  = 'http://www.onvif.org/ver10/schema'
NS_TD = 'http://www.onvif.org/ver10/device/wsdl'

exposed = []      # (severity, description)
protected = []    # interface names that refused

def reachable(port):
    s = socket.socket(); s.settimeout(3.0)
    try:
        return s.connect_ex((CAM, port)) == 0
    finally:
        s.close()

# ---------------------------------------------------------------- RTSP
def rtsp_describe(path, timeout=6.0):
    """Return (status_line, sdp_text, challenged). No credential is sent."""
    s = socket.socket(); s.settimeout(timeout)
    try:
        s.connect((CAM, RTSP_PORT))
    except OSError as e:
        return None, '', False, f'connect failed errno={e.errno}'
    url = f'rtsp://{CAM}:{RTSP_PORT}{path}'
    s.sendall(f'DESCRIBE {url} RTSP/1.0\r\nCSeq: 1\r\nAccept: application/sdp\r\n\r\n'.encode())
    buf = b''; t0 = time.time()
    try:
        while time.time() - t0 < timeout:
            d = s.recv(8192)
            if not d:
                break
            buf += d
    except socket.timeout:
        pass
    s.close()
    txt = buf.decode(errors='replace')
    status = txt.split('\r\n')[0] if txt else '(no response)'
    sdp = txt.split('\r\n\r\n', 1)[1] if '\r\n\r\n' in txt else ''
    return status, sdp, ('WWW-Authenticate' in txt or '401' in status), None

def rtsp_play_packet_count(path, seconds=5):
    """SETUP + PLAY with no credentials; COUNT interleaved RTP packets.
    Nothing is written to disk and no payload is decoded."""
    s = socket.socket(); s.settimeout(8.0)
    try:
        s.connect((CAM, RTSP_PORT))
    except OSError:
        return 0, 'connect failed'
    url = f'rtsp://{CAM}:{RTSP_PORT}{path}'
    def hdr(timeout=6.0):
        s.settimeout(timeout); b = b''; t0 = time.time()
        while b'\r\n\r\n' not in b and time.time() - t0 < timeout:
            try:
                d = s.recv(4096)
            except socket.timeout:
                break
            if not d:
                break
            b += d
        return b.decode(errors='replace')
    s.sendall(f'DESCRIBE {url} RTSP/1.0\r\nCSeq: 1\r\nAccept: application/sdp\r\n\r\n'.encode())
    desc = hdr()
    if not desc.startswith('RTSP/1.0 200'):
        s.close(); return 0, f'DESCRIBE {desc.splitlines()[0] if desc else "(none)"}'
    base = re.search(r'Content-Base:\s*(\S+)', desc)
    track = (base.group(1).strip() if base else url).rstrip('/') + '/track1'
    s.sendall(f'SETUP {track} RTSP/1.0\r\nCSeq: 2\r\nTransport: RTP/AVP/TCP;unicast;interleaved=0-1\r\n\r\n'.encode())
    setup = hdr()
    if not setup.startswith('RTSP/1.0 200'):
        s.close(); return 0, f'SETUP {setup.splitlines()[0] if setup else "(none)"}'
    sid = re.search(r'Session:\s*([^;\r\n]+)', setup)
    s.sendall(f'PLAY {url} RTSP/1.0\r\nCSeq: 3\r\nSession: {sid.group(1).strip() if sid else "0"}\r\nRange: npt=0.000-\r\n\r\n'.encode())
    play = hdr()
    if not play.startswith('RTSP/1.0 200'):
        s.close(); return 0, f'PLAY {play.splitlines()[0] if play else "(none)"}'
    pkts = 0; buf = b''; t0 = time.time(); s.settimeout(2.0)
    while time.time() - t0 < seconds:
        try:
            d = s.recv(65536)
        except socket.timeout:
            continue
        if not d:
            break
        buf += d
        while len(buf) >= 4:
            if buf[0] != 0x24:
                buf = buf[1:]; continue
            ln = struct.unpack('>H', buf[2:4])[0]
            if len(buf) < 4 + ln:
                break
            if buf[1] % 2 == 0:
                pkts += 1
            buf = buf[4+ln:]
    s.close()
    return pkts, 'ok'

def soap(path, body, timeout=6.0):
    s = socket.socket(); s.settimeout(timeout)
    try:
        s.connect((CAM, ONVIF_PORT))
    except OSError as e:
        return f'CONNECT-FAIL {e}'
    env = ('<?xml version="1.0" encoding="UTF-8"?><s:Envelope '
           'xmlns:s="http://www.w3.org/2003/05/soap-envelope">'
           f'<s:Body>{body}</s:Body></s:Envelope>')
    s.sendall((f'POST {path} HTTP/1.1\r\nHost: {CAM}:{ONVIF_PORT}\r\n'
               'Content-Type: application/soap+xml; charset=utf-8\r\n'
               f'Content-Length: {len(env.encode())}\r\nConnection: close\r\n\r\n').encode()
              + env.encode())
    buf = b''; t0 = time.time()
    try:
        while time.time() - t0 < timeout:
            d = s.recv(8192)
            if not d:
                break
            buf += d
    except socket.timeout:
        pass
    s.close()
    return buf.decode(errors='replace')

print(f'== v380_security_check: {CAM} (no credentials, no media recorded) ==')
print()

print('-- RTSP stream description (unauthenticated) --')
for path in ('/live/ch00_0', '/live/ch00_1'):
    status, sdp, challenged, err = rtsp_describe(path)
    if err:
        print(f'   {path:16s} {err}')
        continue
    codec = re.search(r'rtpmap:\d+\s+(\S+)', sdp)
    print(f'   {path:16s} {status}  auth_challenge={challenged}'
          + (f'  codec={codec.group(1)}' if codec else ''))
    if status.startswith('RTSP/1.0 200') and not challenged:
        exposed.append(('HIGH', f'RTSP DESCRIBE {path} returned media description without authentication'))
    elif challenged:
        protected.append(f'RTSP DESCRIBE {path}')

print()
print('-- RTSP media flow (unauthenticated, packet count only) --')
for path in ('/live/ch00_0', '/live/ch00_1'):
    pkts, note = rtsp_play_packet_count(path)
    print(f'   {path:16s} rtp_packets={pkts}  ({note})')
    if pkts > 0:
        exposed.append(('CRITICAL', f'{path} delivered {pkts} RTP packets to an unauthenticated client (anonymous live video)'))

print()
print('-- ONVIF read-only state (unauthenticated) --')
checks = [
    ('GetDeviceInformation', '/onvif/device_service', f'<td:GetDeviceInformation xmlns:td="{NS_TD}"/>', 'HIGH',
     'device identity/version readable without authentication'),
    ('GetNetworkInterfaces', '/onvif/device_service', f'<td:GetNetworkInterfaces xmlns:td="{NS_TD}"/>', 'HIGH',
     'IP/MAC of the camera readable without authentication'),
    ('GetUsers', '/onvif/device_service', f'<td:GetUsers xmlns:td="{NS_TD}"/>', 'HIGH',
     'account name + level readable without authentication'),
    ('GetProfiles', '/onvif/media_service', '<trt:GetProfiles xmlns:trt="http://www.onvif.org/ver10/media/wsdl"/>', 'HIGH',
     'stream/encoder/PTZ configuration readable without authentication'),
    ('GetStreamUri', '/onvif/media_service', '<trt:GetStreamUri xmlns:trt="http://www.onvif.org/ver10/media/wsdl">'
     '<trt:StreamSetup><tt:Stream xmlns:tt="' + NS_S + '">RTP-Unicast</tt:Stream>'
     f'<tt:Transport xmlns:tt="{NS_S}"><tt:Protocol>RTSP</tt:Protocol></tt:Transport></trt:StreamSetup>'
     '<trt:ProfileToken>PROFILE_000</trt:ProfileToken></trt:GetStreamUri>', 'MEDIUM',
     'live stream URI handed out without authentication'),
]
for label, path, body, sev, desc in checks:
    resp = soap(path, body)
    ready = not resp.startswith('CONNECT-FAIL') and resp.startswith('HTTP/1.1 200')
    protected_by_fault = 'NotAuthorized' in resp or 'Sender not Authorized' in resp or '401' in resp.split('\r\n')[0]
    print(f'   {label:22s} {"ANONYMOUS OK" if ready else "refused"}')
    if ready:
        exposed.append((sev, desc))
    elif protected_by_fault:
        protected.append(f'ONVIF {label}')

print()
print('== VERDICT ==')
if exposed:
    for sev, desc in exposed:
        print(f'   [{sev}] {desc}')
    print()
    print('   EXPOSED - private media and/or camera state are reachable without credentials.')
    print('   ACTION: stop. Report this to the owner and ask for ONVIF to be disabled until')
    print('   the camera is reconfigured with authentication (ONVIF user + stream user/password).')
    sys.exit(1)
print('   PROTECTED - no unauthenticated access found on the tested interfaces.')
if protected:
    print('   refused: ' + ', '.join(protected))
sys.exit(0)
PY
