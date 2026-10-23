# V380 proprietary media adapter — status, blocker, and the firmware-generation split

> **Privacy notice:** this document's subject is host identity, so real LAN addresses, device
> identifiers and fingerprints appear as placeholders (`<redacted-...>`). Measurements, evidence and
> conclusions are unchanged; only the values are withheld, and the real values live in the private
> evidence directory outside every repository.

**Authorization.** The owner has explicitly authorized reverse-engineering and use of the V380 proprietary
media protocol on ports 8800/9800 for Cameras 1, 2 and 3, with the standing condition that the cameras are
never exposed to the public internet. This supersedes the earlier milestone rule that held media BLOCKED and
forbade a login handshake. It does **not** lift anything else: no firmware modification, no `ceshi.ini`, no
password guessing, no anonymous RTSP/ONVIF, no connector deployed, nothing installed on the Mi A3, no media
through Oracle.

## 1. What was built, and why it is safe to have built it blind

The wire protocol is documented by public open re-implementations of the V380/Xiongmai family. No code was
copied from any of them (their licensing was not verified, so nothing is vendored): the layouts below are
implemented independently from the interface they describe.

`src/camera/v380_protocol.{h,cpp}` — the codec:

| Element | Fact |
|---|---|
| Login | command `1167`, variant `1022/2/1`, device id at offset 13, username at 49, password field at 81 |
| Password field | 32 bytes = 16-byte clear random key + one AES block, encrypted twice (vendor static key, then the random key) |
| Login result | `1168` + 1001 ok; 1011 invalid username; 1012 invalid password; 1018 invalid device id |
| Stream login | command `301` (device id, fps, auth ticket, `4096+1`, resolution) → response `401` with `v21` |
| Stream start | command `303` carrying `v21` |
| Media framing | 12-byte chunk headers, magic `0x7f`, type `0x00` I-frame / `0x01` P-frame / `0x16` audio, `total`/`cur`/`len` fields, `len <= 500`, `0x6f` keepalive |
| Audio | carries a 20-byte prefix the media layer strips |

Named refusals rather than booleans, everywhere: the device returns **no text** on a bad exchange, so a
boolean would discard the only diagnostic the protocol provides. `unsupported_firmware_generation` (below)
and `invalid_password` are distinct outcomes, and a non-zero password field longer than the 32-byte layout
allows is refused **before** it is sent — a reference implementation memcpy's it into the field unchecked,
which is how a truncated password surfaces as a mysterious device-side rejection.

`src/core/Aes128Ecb.{h,cpp}` — AES-128-ECB in-repo, because OpenSSL development headers are **not present**
on the workstation that builds this project, and the intended hub target is a minimal ARM64 rootfs. One
self-contained primitive removes a dependency instead of adding one. Verified three ways: the FIPS-197
Appendix B and C.1 vectors in both directions, canonical S-box generation (0 mismatches), and **12/12 random
keys cross-checked against the `openssl` CLI**. `[aes]` 23 assertions / 4 cases; `[v380proto]` 232
assertions / 9 cases; full suite 9810 assertions / 734 cases.

## 2. THE BLOCKER, and the question the protocol cannot answer from here

**Neither camera is reachable.** Re-measured, not carried forward: `<redacted-lan-host-cam1>` and `<redacted-lan-host-cam2>` give
**no ICMP reply**, every tested port (554/8554/8899/8800/9800/80/443) is **filtered — timeouts, not refusals**,
and the neighbour table shows both as `FAILED`. Camera 3 (`<redacted-lan-host-cam3>`) is absent from the neighbour table
entirely. A closed port would RST instantly; a timeout across every port means **no path**, so nothing is
learned about which camera ports exist. Live ingestion cannot be attempted until the devices are powered and
on the network.

**Two generations exist, and this codec speaks only one:**

- **Legacy** (what this codec implements): after stream login the device emits the 12-byte chunk framing and
  the payload is the codec bitstream essentially as-is. The public reference tool works on the platform line
  `Hw_AWT3610E_XHR_E_...` — which is **camera 2's platform** (`..._WF_20230807`, one month later). That is
  encouraging, not proof.
- **Newer**: the payload is AES-encrypted and framed with a 30-byte `V380 Pro` header, and the stream login is
  rejected with `-11`/`-12` — exactly the code this codec maps to `unsupported_firmware_generation`. Public
  work reports the key for that generation as recoverable only from a packed (Qihoo 360 Jiagu) application
  image at runtime, with the old public keys tested and failing. **Nothing in this repository can obtain that
  key, and no protocol work can**, so that generation is a named dead end rather than a half-finished task.

**What is still needed from the owner:**
1. Power and network the cameras (both were offline at the time of this measurement).
2. The **device password** for each camera — the app's *Settings → Password → Change Device Password*, which
   is separate from the cloud account login. None exists in the private evidence directory. It must be at most
   **16 characters** for the legacy field; a longer password cannot be sent at all, which is a finding in
   itself and the reason the codec refuses rather than truncates.
3. The device id per unit (camera 3's `<redacted-cam3-device-id>` is on record; camera 1 and 2 device ids are not yet).

## 3. The next measurable step

A single bounded handshake decides everything: build the login request for one camera, send it on 8800, and
read the result. That is one connection, one request, no media, no retry loop — and its outcome is one of
three named values: `ok` (proceed), `invalid_password` (credential problem), or
`unsupported_firmware_generation` (this generation is out of reach by protocol work). Everything else in this
project is downstream of that answer, so it is deliberately the first thing to do and the only thing worth
doing before the owner supplies credentials.

## 4. Security posture (unchanged by the authorization)

- **Never public.** No port forwarding, no listener on the workstation, no cloud or VPS relay, no media
  transcoding, no public stream.
- **The vendor static key is a parameter, never a constant in this repository.** The repository is public and
  a vendor decryption key does not belong in it. It is loaded at runtime from the private evidence directory
  under the same permission rule as any credential; the tests use a synthetic key.
- **Credentials** live outside every repository, in a file refused when group/other-readable, never logged and
  never placed in a status payload.
- Authorization before every frame (the existing `ConsentGuardedDevice` / `RegistryAuthorizer` path), consent
  revocable, and an unauthorized device emits nothing at all.
- If any codec built here were ever to serve a stream of its own, authentication is enabled **unconditionally**
  — the public tools in this space default to no authentication, which is not acceptable.

## 5. Not claimed

No live camera stream, no decoded frame, no camera-to-fused-alert operation, no audio, no PTZ, no species or
disaster inference, no deployment. The codec is a wire implementation verified against synthetic bytes, and
the `[v380proto]` suite passed with **no camera connected** — which is precisely why no media claim may be
written in any tense until the handshake above produces `ok` and a frame is actually reassembled.

---

# MEASURED RESULTS — the bounded handshake, run once per camera

Tool: `tools/v380_handshake.cpp` (`hg_v380_handshake`). One connection, one login, and only if the login is
answered, exactly one stream login. It **stops before the stream start (303)**, which is the command that
begins media: no stream was opened, no frame was reassembled, no video byte was written anywhere.

## Camera 3 — `<redacted-lan-host-cam3>`, device id <redacted-cam3-device-id>, empty password (QR-set-up device)

```
connect ok  ip=<redacted-lan-host-cam3> port=8800
login response: 412 bytes, command=-100 result_code=0 -> unexpected_command
first bytes: 9c ff ff ff 00 00 00 ...
stream login response: 32 bytes, command=401 v21=-11 -> unsupported_firmware_generation
RESULT unsupported_firmware_generation
```

**Classification: `unsupported_firmware_generation`** — not `ok`, and **not** `invalid_password`.

- The device **answered**, so the empty password was never rejected as a credential: it refused the exchange
  before reaching a credential verdict (`-100` = `0xFFFFFF9C`, whose low byte `0x9c` is the signature the
  public write-up attributes to the newer framing).
- The stream login then returned `401` with **`v21 = -11`**, the documented rejection of the newer generation.
- That generation encrypts its media behind a 30-byte `V380 Pro`-framed protocol, and public reverse
  engineering reports the key as recoverable only by unpacking a Qihoo-360-Jiagu-packed application at
  runtime, with the previously public keys tested and failing. **No protocol work reaches it**, so this is a
  named dead end rather than a half-finished task. Nothing in this repository attempts to invent that key.
- The login response contains **no `V380 Pro` literal**, which is recorded because it is what was measured —
  the framing signature appears on the media connection, not on the login exchange.

## Cameras 1 and 2 — `<redacted-lan-host-cam1>`, `<redacted-lan-host-cam2>`

```
RESULT connect_failed  detail=connect failed: No route to host
```

**Classification: `connect_failed` — no path.** Not a credential verdict and not a protocol verdict: nothing
about these units was learned, because nothing could be sent.

- `No route to host` is stronger than a timeout: the local stack has no route/ARP entry for either address.
- A broadcast discovery query (`NVDEVSEARCH^100` to `255.255.255.255:10008`, the vendor discovery protocol the
  public tool uses) produced **0 responders** on this subnet.
- The owner reports both cameras powered and networked; the measurement says they are **not reachable from
  this workstation**, and that is reported as "not observable from here" rather than as either party being
  wrong. The specific data that closes the gap: the address the vendor app currently shows for each camera,
  or the router's DHCP client list.
- **No device id was fabricated.** The login request carries a *numeric* device id at a fixed offset; an IP
  address cannot fill it, and substituting one would manufacture an identifier and make the device's answer a
  lie about which camera was contacted. The tool's `missing_device_id` path (exit 2) exists exactly for this
  and was not needed here only because the connection fails first.

## What this means for the goal

**No unit can be streamed by the proprietary-protocol route today**, and that is now a measured statement
rather than a guess: one camera is on the wrong side of a generation boundary this codec cannot cross, and two
are not reachable from here at all.

The cheap alternative for camera 3 is **not** the proprietary protocol: its firmware refused 554/8899, which
is the as-shipped state for this family, and the vendor app exposes a documented *ONVIF setting* on models
that support it. Earlier work on camera 1 proved that toggling it yields **local RTSP with live H.265 RTP** —
but it also proved that interface demanded **no authentication**, which is why the security check runs BEFORE
any media claim and why authentication must be verified, not assumed. If camera 3's app shows the toggle, that
is the shortest path to an actual live stream, entirely local. Reaching the newer generation's encrypted
stream, by contrast, would require runtime-unpacking a protected vendor application — a different and much
more invasive act than a protocol handshake, and **not authorized**; it would need its own explicit decision.

---

# STATUS — proprietary path CLOSED on camera 3; pivot to the standard path (owner decision, 2026-10-21 00:01 IST)

## 1. Camera 3 — proprietary 8800/9800: **CLOSED**

**Closed as `unsupported_firmware_generation`.** The bounded handshake produced `401` with `v21 = -11` after a
login the device refused with `-100`, and public reverse engineering attributes that generation's encrypted
media to a Qihoo-360-Jiagu-packed application whose key is reachable only by unpacking it at runtime.

Per the owner's instruction, **no attempt will be made to unpack or memory-dump the vendor application**, and
none is planned. The closure is a decision recorded against evidence, not a task left half-done: the codec
stays in the tree because it is the reason the verdict could be measured at all, and its `[v380proto]` suite
documents what the legacy path would require. Nothing further will be spent on this route for this unit.

## 2. Camera 3 — pivot to standard RTSP/ONVIF: **AWAITING OWNER TOGGLE**

Awaiting the owner enabling ONVIF in the V380 Pro app (*Settings → Advanced settings → ONVIF setting*). The
as-shipped and current state was measured, not assumed:

```
== v380_security_check v2: <redacted-lan-host-cam3>  rtsp=554 onvif=8899 ==
   RTSP/554       DISABLED    no RTSP listener (connection refused)
   ONVIF/8899     DISABLED    no ONVIF listener (connection refused)
== VERDICT == DISABLED
exit=0
```

**`DISABLED` is not `AUTHENTICATION ENFORCED`** — nothing is protected, because nothing is running. No
security claim may be read from this result in either direction; it is a baseline, and a secured integration
still cannot be claimed from it alone.

### The exact sequence once the toggle is on

1. Re-run the checker — **same command, no credentials, before touching any stream**:
   `bash scripts/v380_security_check.sh <redacted-lan-host-cam3> 554 8899`
2. Read the classification, and act on it rather than around it:
   - `EXPOSED` → the interface answers **without authentication**. **STOP**, document the security impact, do
     not wire it into anything, and report to the owner. This is exactly what camera 1's toggle produced
     (anonymous RTSP with live H.265 RTP), so it is the *expected* outcome, not a surprise.
   - `AUTHENTICATION ENFORCED` → only then proceed, and only with an approved credential passed via
     `--creds FILE` (never inline, never in shell history), which is also the only way the positive half of
     the check can be completed. Without it the positive half is `NOT ATTEMPTED`, never "passed".
   - `INCONCLUSIVE` → treat as non-secure (non-zero exit) and report; do not retry until it is understood.
3. Only after a green security result: probe the stream locally (`ffprobe`), record the REAL working URL,
   codec, resolution and FPS as measured values, and keep it on the local network — no port forwarding, no
   public stream, no relay.
4. Media claims stay unwritten until step 3 has an actual result. A viable *interface* is not a viable
   *stream*, and neither is a capability claim.

## 3. Cameras 1 and 2 — **STANDING DOWN**

Standing down until the DHCP leases / addresses are confirmed from the router. Both gave `No route to host`
and neither answered the vendor discovery broadcast, so nothing about them is known from here, and no device
id will be guessed in the meantime. Nothing is scheduled for these units.

---

# Dynamic addresses: how camera identity survives a DHCP change

The cameras take DHCP leases, so any design that stores an address breaks quietly the moment the lease moves -
and one that treats the address as IDENTITY breaks dangerously, because a recycled address would hand a
camera's consent to whatever device takes it next. Both failure modes are already real here: two of the three
units changed address while nothing about the devices changed.

**The rule: an address is an attachment attribute, never an identity.** Identity is the owner-confirmed device
id plus the NIC fingerprint (normalized MAC), both of which were stable across the moves. Nothing
authorization-related may read an address - consent, pairing and capture flags are stored against the logical
device.

**And it is not "scanning".** One UDP broadcast (`NVDEVSEARCH^100`) reaches every camera on the segment; each
answers with its id, fingerprint, address, mask and gateway in a single exchange. There is no sweep, no
per-host probing, and no traffic to anything that is not already listening for that query. A periodic subnet
sweep would be slower, noisier, and would enumerate devices nobody asked about.

`src/camera/V380Discovery.{h,cpp}` parses those responses (validated against a live device whose device id was
independently confirmed - the field it reads as the device id reproduced that value exactly).
`src/camera/CameraEndpointResolver.{h,cpp}` turns a device id into a current address under five rules:

1. **Resolve on demand, cache briefly.** A resolved address is reused within a short TTL; a failed connection
   invalidates it immediately, which is how a move is discovered without waiting for anything.
2. **The fingerprint decides.** A responder claiming a known device id with a DIFFERENT MAC is refused
   (`fingerprint_mismatch`) and no address is returned. This is the security-critical rule: without it, another
   device could claim a camera's id and inherit its consent.
3. **Contradictions apply nothing.** Two responders claiming one device id, or one address claimed by two
   responders, means the pass is not evidence: report `conflict` and resolve nothing.
4. **A move is not a new device.** Same fingerprint at a new address is `resolved_moved`: identity and consent
   survive, only the attachment is updated. Reporting a move as a stranger is how a registry fills with
   phantoms.
5. **Fail closed.** Every refusal and every absence returns no address, so a caller can never accidentally
   connect to whatever happens to answer. An id nobody registered is `not_found` - onboarding by accident is
   not possible.

## Demonstrated on the live cameras

A census of the segment, then resolution **by device id alone** with the units' *previous* leases planted as
the remembered (now stale) addresses:

```
pass 1: 3 responder(s)
  device_id=<redacted-camid-1>  fingerprint=<redacted-mac-a>  address=<redacted-lan-host-a>  media_port=8800
  device_id=<redacted-camid-2>  fingerprint=<redacted-mac-b>  address=<redacted-lan-host-b>  media_port=8800
  device_id=<redacted-camid-3>  fingerprint=<redacted-mac-c>  address=<redacted-lan-host-c>  media_port=8800

RESOLVE resolved_moved  device_id=<redacted-camid-1>  address=<redacted-lan-host-a>
RESOLVE resolved_moved  device_id=<redacted-camid-2>  address=<redacted-lan-host-b>
RESOLVE resolved        device_id=<redacted-camid-3>  address=<redacted-lan-host-c>
```

Two units the memory had at their old addresses were correctly reported as **moved** (same fingerprint, new
address); the one that had not moved was not falsely reported as moved. In the same run, a device presenting a
known device id with the wrong fingerprint was **refused** with no address returned.

Tests: `[resolve]` 44 assertions / 8 cases with a stubbed discovery source, so the rules are verified without a
network and without cameras - including the move, the mismatch, both conflict shapes, cache expiry,
invalidation, and the refusal to resolve an unregistered id.

**Owner-side note:** a DHCP reservation would make the addresses stable, but nothing here depends on it, and it
needs router access. Resolution is the mechanism; a reservation would only be an optimisation.

---

# Exposure measurement — CVE-2025-7503 (telnet) and CVE-2026-12527 (RTSP bypass), after the ONVIF toggle

Owner-directed, exposure only. **No credentials were sent, no login attempted, no banner read, no file
requested, nothing extracted, no config or firmware touched.** Port 23 was connect-classified only: an OPEN
result is the finding, and logging in would be the exploit rather than the assessment.

## Per-unit surface (one TCP connect per port; errno semantics are the evidence)

| Unit | 23 (telnet) | 554 (RTSP) | 8899 (ONVIF) | 8800 / 9800 (vendor) |
|---|---|---|---|---|
| camera 1 | CLOSED (refused) | CLOSED (refused) | CLOSED (refused) | OPEN |
| camera 2 | CLOSED (refused) | CLOSED (refused) | CLOSED (refused) | OPEN |
| camera 3 | CLOSED (refused) | CLOSED (refused) | CLOSED (refused) | OPEN |

`CLOSED (refused)` is a live host answering with a refusal (errno 111) — a different fact from a filtered
timeout, and it means no listener exists on that port.

## CVE-2025-7503 (telnet root backdoor, hard-coded credentials)

**Not present on any of the three units.** Port 23 refuses on all three, so the documented backdoor service is
absent from our hardware as measured. Note this is a *per-unit measurement*, not a general statement about the
V380 family: the advisory names the `AppFHE1_V1.0.6.0` firmware line, while these units report
`AppEV2W_VA3` / `Hw_AWT3610E_XHR_E_V1.0_WF_20230807`, and one unit's earlier full 1-65535 sweep likewise found
only 8800/9800. Re-measure rather than inherit this verdict if a unit's firmware changes.

## CVE-2026-12527 (RTSP media-pipeline auth bypass)

**Not testable, and not applicable as measured — there is no RTSP listener to bypass.** Port 554 refuses on all
three units, and after the owner enabled the ONVIF setting on camera 3 the interface state is **unchanged from
the pre-toggle baseline**: 554 refused, 8899 refused, no ONVIF WS-Discovery responder.

That state is the correct result to report, and it must not be read as "secured": per the checker's own rule,
`DISABLED` is not `AUTHENTICATION ENFORCED` — nothing is protected because nothing is running.

### The sensitive check, and what it returned

A single WS-Discovery `Probe` multicast to `239.255.255.250:3702` (one query, not a sweep) produced **no ONVIF
reply**. One datagram came back from a different host (`<redacted-lan-host-other>`) carrying our own Probe body
rather than a `ProbeMatches` element with `XAddrs` — that is a reflection of our own packet, **not** an ONVIF
responder, and it is recorded as such rather than counted as a device answering.

### The network cannot settle why, and should not pretend to

Three states look identical from here: the toggle did not take effect; the firmware ignores it (as on camera 2,
where the option is absent from the app entirely); or the listeners only bind after a reboot. **Only the device's
own UI decides which.** The decisive evidence is the app screen for this unit — whether an ONVIF switch exists
at all and whether it reads ON — and, if it does, whether the vendor app requires a restart for it to bind.

## What this means for the goal

The only open surface on all three units is the vendor protocol (8800/9800), whose media path this project has
already measured as closed to us (`v21 = -11`, the encrypted generation). So **no unit currently offers a route
to live video**, and that statement is now measured from both directions: the proprietary path refuses, and the
standard path has no listener.

## Recommended remediation, unchanged and not dependent on any of the above

The cameras should sit on their own segment reachable only by one trusted broker, with default-DROP on the
camera-facing interface and **no port forwarding** to any camera. Isolation bounds who can reach a camera; it
does not make an unauthenticated service secure, and it is not claimed to. Verifying that no forwarding rule
exists needs the router's own admin page and therefore the owner's password.
