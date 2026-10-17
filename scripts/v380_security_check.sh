#!/usr/bin/env bash
# v380_security_check.sh - access-control classification for a V380 camera.
#
# WHY THIS EXISTS
#   An earlier revision exited 0 whenever no anonymous access was found. That
#   conflated two very different worlds:
#       (1) the interface is not running at all        -> DISABLED
#       (2) the interface is running and demands auth  -> AUTHENTICATION ENFORCED
#   Only (2) is a secured integration. This revision classifies each interface
#   separately and never reports "protected" as if it were a positive result.
#
# CLASSIFICATION (per interface)
#   DISABLED                no listener: connection refused/reset
#   AUTHENTICATION ENFORCED the listener rejects the unauthenticated request with
#                           an explicit challenge (RTSP 401 + WWW-Authenticate,
#                           ONVIF HTTP 401 or a SOAP NotAuthorized fault)
#   EXPOSED                 the listener answers an unauthenticated request with
#                           data (media description, media flow, device/account state)
#   INCONCLUSIVE            a listener exists but neither challenged nor disclosed
#                           (no answer, empty close, malformed reply, transport error)
#
# AUTHENTICATED CONFIRMATION (optional, --creds FILE)
#   "A secured integration requires both a negative unauthenticated test AND a
#    successful authorized connection." With a credential file the script ALSO
#    performs an AUTHORIZED request (RTSP Digest with Basic fallback, ONVIF
#    WS-Security PasswordDigest, HTTP Digest fallback) and reports whether it
#    succeeded. Without credentials the positive half is reported as NOT
#    ATTEMPTED - never as passed.
#
# EXIT CODES (fail-closed)
#   0  SECURE         no EXPOSED and no INCONCLUSIVE interface; every live
#                     interface is AUTHENTICATION ENFORCED (and, when credentials
#                     were supplied, authorized access was CONFIRMED).
#                     NOTE: an all-DISABLED camera also exits 0 but is labelled
#                     DISABLED, which is NOT the same claim.
#   1  EXPOSED        at least one interface served an unauthenticated client
#   2  usage error / refused credential file
#   3  INCONCLUSIVE   no exposure proven, but something could not be classified
#                     (or the authorized half did not confirm) - NOT secure
#
# SAFETY CONTRACT (do not relax without an explicit owner decision)
#   * one target given on the command line - no scanning, no host discovery
#   * records NO media: an RTP session is COUNTED (interleaved packets over TCP),
#     never written to disk, never decoded, never published
#   * issues NO control command: no PTZ move, no preset recall, no reboot, no
#     configuration set. Read-only calls only
#   * sends NO credential unless --creds names a file the operator supplied;
#     never guesses, never brute-forces, never tries a vendor default password
#   * the credential file is refused if group/other readable; its contents are
#     never printed, logged or echoed
#
# Usage:
#   bash scripts/v380_security_check.sh <camera-ip> [rtsp-port] [onvif-port]
#   bash scripts/v380_security_check.sh <camera-ip> 554 8899 --creds /path/cred.json
#
# Credential file format (JSON): {"username": "admin", "password": "..."}
#
# The previous revision of this script (exit 0 == "PROTECTED", no classification)
# is preserved at commit 2784d77 for the original HG-V380-04 evidence.

set -uo pipefail

CAM="${1:-}"
RTSP_PORT="${2:-554}"
ONVIF_PORT="${3:-8899}"

if [ -z "$CAM" ]; then
    echo "usage: bash scripts/v380_security_check.sh <camera-ip> [rtsp-port] [onvif-port] [--creds FILE]" >&2
    exit 2
fi

CREDS=""
shift 3 2>/dev/null || shift $# 
while [ "$#" -gt 0 ]; do
    case "$1" in
        --creds) CREDS="${2:-}"; shift 2 ;;
        *) echo "unknown argument: $1" >&2; exit 2 ;;
    esac
done
if [ -n "$CREDS" ] && [ ! -f "$CREDS" ]; then
    echo "credential file not found: $CREDS" >&2
    exit 2
fi

python3 - "$CAM" "$RTSP_PORT" "$ONVIF_PORT" "$CREDS" <<'PY'
import base64, hashlib, json, os, re, socket, struct, sys, time

CAM, RTSP_PORT, ONVIF_PORT, CREDS_PATH = sys.argv[1], int(sys.argv[2]), int(sys.argv[3]), sys.argv[4]
NS_S  = 'http://www.onvif.org/ver10/schema'
NS_TD = 'http://www.onvif.org/ver10/device/wsdl'

DISABLED, ENFORCED, EXPOSED, INCONCLUSIVE = (
    'DISABLED', 'AUTHENTICATION ENFORCED', 'EXPOSED', 'INCONCLUSIVE')
results = []          # (interface, classification, note)
findings = []         # (severity, description)
auth_confirmed = {}   # {'RTSP': bool, 'ONVIF': bool}
creds = None

# ---------------------------------------------------------------- credentials
if CREDS_PATH:
    mode = os.stat(CREDS_PATH).st_mode & 0o777
    if mode & 0o077:
        print(f'  REFUSING credential file: mode {oct(mode)} is group/other accessible.')
        print('  chmod 600 it and retry. Nothing was sent.')
        sys.exit(2)
    creds = json.load(open(CREDS_PATH))
    if not creds.get('username') or 'password' not in creds:
        print('  credential file must contain "username" and "password". Nothing was sent.')
        sys.exit(2)
    print(f'  authorized tests ENABLED (user={creds["username"]!r}; password not shown)')

def md5(x):  return hashlib.md5(x.encode()).hexdigest()

def parse_challenge(v):
    """Digest challenge -> dict of params. {} means Basic."""
    if not v or not v.lower().startswith('digest'):
        return {}
    d = {}
    for m in re.finditer(r'(\w+)\s*=\s*(?:"([^"]*)"|([^,\s]+))', v[len('Digest'):].strip()):
        d[m.group(1).lower()] = m.group(2) if m.group(2) is not None else m.group(3)
    return d

def digest_header(user, pw, spec, method, uri):
    ha1 = md5(f'{user}:{spec.get("realm","")}:{pw}')
    ha2 = md5(f'{method}:{uri}')
    parts = [f'username="{user}"', f'realm="{spec.get("realm","")}"',
             f'nonce="{spec.get("nonce","")}"', f'uri="{uri}"']
    if 'auth' in spec.get('qop', ''):
        nc, cnonce = '00000001', hashlib.sha1(str(time.time()).encode()).hexdigest()[:16]
        parts.append(f'response="{md5(f"{ha1}:{spec.get("nonce","")}:{nc}:{cnonce}:auth:{ha2}")}"')
        parts += [f'qop=auth', f'nc={nc}', f'cnonce="{cnonce}"']
    else:
        parts.append(f'response="{md5(f"{ha1}:{spec.get("nonce","")}:{ha2}")}"')
    if spec.get('opaque'):
        parts.append(f'opaque="{spec["opaque"]}"')
    return 'Digest ' + ', '.join(parts)

def auth_for(method, uri, challenge_value):
    """Authorization header for a challenge, or '' when none can be built."""
    if not creds or not challenge_value:
        return ''
    spec = parse_challenge(challenge_value)
    if not spec:
        tok = base64.b64encode(f'{creds["username"]}:{creds["password"]}'.encode()).decode()
        return 'Basic ' + tok
    return digest_header(creds['username'], creds['password'], spec, method, uri)

def chall_of(txt):
    m = re.search(r'WWW-Authenticate:\s*([^\r\n]+)', txt or '')
    return m.group(1) if m else ''

def is_refused(err):
    """A refused/reset connection means no listener: errno 111 = ECONNREFUSED,
    104 = ECONNRESET. Anything else (timeout, transport error) is not conclusive."""
    if not err:
        return False
    return any(tok in err for tok in ('refused', 'reset', 'errno=111', 'errno=104',
                                     'ECONNREFUSED', 'ECONNRESET'))

def challenged(txt):
    """Did the device explicitly DEMAND authentication?"""
    if not txt:
        return False
    if '401' in txt.split('\r\n')[0]:
        return True
    return bool(re.search(r'NotAuthorized|Sender not Authorized|Unauthorized', txt))

# ---------------------------------------------------------------- RTSP
def rtsp_sock(timeout=6.0):
    s = socket.socket(); s.settimeout(timeout)
    s.connect((CAM, RTSP_PORT))
    return s

def rtsp_read(s, timeout=6.0):
    s.settimeout(timeout); buf = b''; t0 = time.time()
    while b'\r\n\r\n' not in buf and time.time() - t0 < timeout:
        try:
            d = s.recv(8192)
        except socket.timeout:
            break
        if not d:
            break
        buf += d
    return buf.decode(errors='replace')

def rtsp_probe(path, with_creds=False):
    """Unauthenticated DESCRIBE. Returns (text, error)."""
    try:
        s = rtsp_sock()
    except OSError as e:
        return None, f'connect failed errno={e.errno}'
    url = f'rtsp://{CAM}:{RTSP_PORT}{path}'
    s.sendall(f'DESCRIBE {url} RTSP/1.0\r\nCSeq: 1\r\nAccept: application/sdp\r\n\r\n'.encode())
    txt = rtsp_read(s)
    s.close()
    return txt, None

def rtsp_play_count(path, seconds=5, with_creds=False):
    """SETUP + PLAY; COUNT interleaved RTP packets. Nothing written or decoded."""
    try:
        s = rtsp_sock(8.0)
    except OSError:
        return 0, 'connect failed'
    url = f'rtsp://{CAM}:{RTSP_PORT}{path}'
    def hdr(timeout=6.0): return rtsp_read(s, timeout)
    s.sendall(f'DESCRIBE {url} RTSP/1.0\r\nCSeq: 1\r\nAccept: application/sdp\r\n\r\n'.encode())
    desc = hdr()
    # keep the challenge from the FIRST reply: a successful reply carries no
    # WWW-Authenticate header, so re-deriving it from `desc` would lose it and
    # SETUP/PLAY would then be sent unauthenticated (found by the self-test).
    challenge = chall_of(desc)
    if desc.startswith('RTSP/1.0 401') and with_creds and auth_for('DESCRIBE', url, challenge):
        s.sendall((f'DESCRIBE {url} RTSP/1.0\r\nCSeq: 2\r\nAccept: application/sdp\r\n'
                   f'Authorization: {auth_for("DESCRIBE", url, challenge)}\r\n\r\n').encode())
        desc = hdr()
        challenge = challenge or chall_of(desc)
    if not desc.startswith('RTSP/1.0 200'):
        s.close(); return 0, f'DESCRIBE {desc.splitlines()[0] if desc else "(no response)"}'
    base = re.search(r'Content-Base:\s*(\S+)', desc)
    track = (base.group(1).strip() if base else url).rstrip('/') + '/track1'
    ah = auth_for('SETUP', track, challenge) if with_creds else ''
    s.sendall((f'SETUP {track} RTSP/1.0\r\nCSeq: 3\r\nTransport: RTP/AVP/TCP;unicast;interleaved=0-1\r\n'
               + (f'Authorization: {ah}\r\n' if ah else '') + '\r\n').encode())
    setup = hdr()
    if not setup.startswith('RTSP/1.0 200'):
        s.close(); return 0, f'SETUP {setup.splitlines()[0] if setup else "(no response)"}'
    sid = re.search(r'Session:\s*([^;\r\n]+)', setup)
    ph = auth_for('PLAY', url, challenge) if with_creds else ''
    s.sendall((f'PLAY {url} RTSP/1.0\r\nCSeq: 4\r\nSession: {sid.group(1).strip() if sid else "0"}\r\n'
               + (f'Authorization: {ph}\r\n' if ph else '')
               + 'Range: npt=0.000-\r\n\r\n').encode())
    play = hdr()
    if not play.startswith('RTSP/1.0 200'):
        s.close(); return 0, f'PLAY {play.splitlines()[0] if play else "(no response)"}'
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

# ---------------------------------------------------------------- ONVIF
def soap(body, with_creds=False):
    try:
        s = socket.socket(); s.settimeout(8.0); s.connect((CAM, ONVIF_PORT))
    except OSError as e:
        return None, f'connect failed errno={e.errno}'
    env = ('<?xml version="1.0" encoding="UTF-8"?><s:Envelope '
           'xmlns:s="http://www.w3.org/2003/05/soap-envelope">'
           f'<s:Body>{body}</s:Body></s:Envelope>')
    if with_creds:
        created = time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime())
        nonce = os.urandom(16)
        dgst = base64.b64encode(hashlib.sha1(nonce + created.encode() + creds['password'].encode()).digest()).decode()
        wsse = ('<s:Header><wsse:Security xmlns:wsse="http://docs.oasis-open.org/wss/2004/01/'
                'oasis-200401-wss-wssecurity-secext-1.0.xsd"><wsse:UsernameToken>'
                f'<wsse:Username>{creds["username"]}</wsse:Username>'
                '<wsse:Password Type="http://docs.oasis-open.org/wss/2004/01/'
                'oasis-200401-wss-username-token-profile-1.0#PasswordDigest">'
                f'{dgst}</wsse:Password>'
                '<wsse:Nonce EncodingType="http://docs.oasis-open.org/wss/2004/01/'
                'oasis-200401-wss-soap-message-security-1.0#Base64Binary">'
                f'{base64.b64encode(nonce).decode()}</wsse:Nonce>'
                '<wsu:Created xmlns:wsu="http://docs.oasis-open.org/wss/2004/01/'
                f'oasis-200401-wss-wssecurity-utility-1.0.xsd">{created}</wsu:Created>'
                '</wsse:UsernameToken></wsse:Security></s:Header>')
        env = env.replace('<s:Body>', wsse + '<s:Body>', 1)
    def post(auth):
        req = (f'POST /onvif/device_service HTTP/1.1\r\nHost: {CAM}:{ONVIF_PORT}\r\n'
               'Content-Type: application/soap+xml; charset=utf-8\r\n'
               + (f'Authorization: {auth}\r\n' if auth else '')
               + f'Content-Length: {len(env.encode())}\r\nConnection: close\r\n\r\n')
        # A FRESH connection per attempt: the device closes after each reply, so
        # reusing one socket makes the authenticated retry fail silently (caught
        # by scripts/v380_security_check_selftest.sh).
        c = socket.socket(); c.settimeout(8.0); c.connect((CAM, ONVIF_PORT))
        try:
            c.sendall(req.encode() + env.encode())
            buf = b''
            try:
                while True:
                    d = c.recv(8192)
                    if not d:
                        break
                    buf += d
            except socket.timeout:
                pass
        finally:
            c.close()
        return buf.decode(errors='replace')
    txt = post('')
    if with_creds and '401' in txt.split('\r\n')[0]:
        a = auth_for('POST', '/onvif/device_service', chall_of(txt))
        if a:
            txt = post(a)
    return txt, None

# ================================================================ run
print(f'== v380_security_check v2: {CAM}  rtsp={RTSP_PORT} onvif={ONVIF_PORT} ==')
print('   classification: DISABLED | AUTHENTICATION ENFORCED | EXPOSED | INCONCLUSIVE')
print('   (no media recorded, no control command sent)')
print()

# ---- RTSP
rtsp_path = '/live/ch00_1'
txt, err = rtsp_probe(rtsp_path)
if err:
    cls = DISABLED if is_refused(err) else INCONCLUSIVE
    note = 'no RTSP listener (connection refused)' if cls == DISABLED else err
    results.append((f'RTSP/{RTSP_PORT}', cls, note))
elif challenged(txt):
    results.append((f'RTSP/{RTSP_PORT}', ENFORCED,
                    'unauthenticated DESCRIBE challenged (401 + WWW-Authenticate)'))
elif txt.startswith('RTSP/1.0 200'):
    pkts, pnote = rtsp_play_count(rtsp_path)
    note = 'unauthenticated DESCRIBE returned a media description'
    if pkts > 0:
        note += f'; {pkts} RTP packets flowed with no credentials'
        findings.append(('CRITICAL', f'RTSP delivered {pkts} RTP packets to an unauthenticated client'))
    else:
        note += f' (media flow: {pnote})'
    findings.append(('HIGH', 'RTSP returned a media description with no credentials'))
    results.append((f'RTSP/{RTSP_PORT}', EXPOSED, note))
else:
    results.append((f'RTSP/{RTSP_PORT}', INCONCLUSIVE,
                    'listener present but the reply was neither a challenge nor data'))

# ---- ONVIF
DEVBODY = f'<td:GetDeviceInformation xmlns:td="{NS_TD}"/>'
txt, err = soap(DEVBODY)
if err:
    cls = DISABLED if is_refused(err) else INCONCLUSIVE
    note = 'no ONVIF listener (connection refused)' if cls == DISABLED else err
    results.append((f'ONVIF/{ONVIF_PORT}', cls, note))
elif challenged(txt):
    results.append((f'ONVIF/{ONVIF_PORT}', ENFORCED,
                    'unauthenticated GetDeviceInformation challenged (401 / NotAuthorized)'))
elif txt.startswith('HTTP/1.1 200'):
    results.append((f'ONVIF/{ONVIF_PORT}', EXPOSED,
                    'unauthenticated GetDeviceInformation returned device state'))
    findings.append(('HIGH', 'ONVIF disclosed device state with no credentials'))
else:
    results.append((f'ONVIF/{ONVIF_PORT}', INCONCLUSIVE,
                    'listener present but the reply was neither a challenge nor data'))

# ---- authorized confirmation
attempted = {}
if creds:
    rtsp_ok = onvif_ok = False
    if [r for r in results if r[0].startswith('RTSP') and r[1] == ENFORCED]:
        attempted['RTSP'] = True
        pkts, note = rtsp_play_count(rtsp_path, seconds=4, with_creds=True)
        rtsp_ok = pkts > 0
        print(f'   authorized RTSP test: {"CONFIRMED" if rtsp_ok else "NOT CONFIRMED"} '
              f'({pkts} RTP packets; {note})')
    if [r for r in results if r[0].startswith('ONVIF') and r[1] == ENFORCED]:
        attempted['ONVIF'] = True
        t, e2 = soap(DEVBODY, with_creds=True)
        onvif_ok = bool(t and t.startswith('HTTP/1.1 200') and 'GetDeviceInformationResponse' in t)
        print(f'   authorized ONVIF test: {"CONFIRMED" if onvif_ok else "NOT CONFIRMED"}'
              + (f' ({e2})' if e2 else ''))
    auth_confirmed = {'RTSP': rtsp_ok, 'ONVIF': onvif_ok}
    print()

print('-- classification --')
for iface, cls, note in results:
    print(f'   {iface:14s} {cls:24s} {note}')
print()

live   = [r for r in results if r[1] != DISABLED]
inconc = [r for r in results if r[1] == INCONCLUSIVE]
enf    = [r for r in results if r[1] == ENFORCED]

print('== VERDICT ==')
if findings:
    for sev, desc in findings:
        print(f'   [{sev}] {desc}')
    print()
    print('   EXPOSED - an unauthenticated client reached media or camera state.')
    print('   ACTION: disable the interface (or its ONVIF/RTSP setting) until authentication')
    print('   is configured, then re-run.')
    sys.exit(1)

if not live:
    print('   DISABLED - no RTSP or ONVIF listener is present.')
    print('   This is NOT "authentication enforced": nothing is protected because nothing is')
    print('   running. A secured integration cannot be claimed from this result alone.')
    sys.exit(0)

if inconc:
    print('   INCONCLUSIVE - an interface is running but could not be classified:')
    for iface, _, note in inconc:
        print(f'      {iface}: {note}')
    print('   NOT secure. Fail-closed: investigate before integrating.')
    sys.exit(3)

print('   AUTHENTICATION ENFORCED on: ' + ', '.join(r[0] for r in enf))
if not creds:
    print('   authorized access NOT ATTEMPTED (no --creds file supplied).')
    print('   The positive half is UNVERIFIED - this is not yet a secured-integration result.')
    print('   Re-run with --creds <file> once an approved credential exists.')
elif not attempted:
    print('   authorized access was not attempted on any tested interface.')
else:
    bad = [k for k, v in auth_confirmed.items() if not v]
    if bad:
        print(f'   BUT authorized access was NOT CONFIRMED for: {", ".join(bad)}')
        print('   A secured integration requires BOTH halves. Fail-closed: INCONCLUSIVE.')
        sys.exit(3)
    print('   authorized access CONFIRMED with the supplied credentials.')
    print('   SECURE - negative unauthenticated test AND positive authorized test both pass.')
sys.exit(0)
PY
