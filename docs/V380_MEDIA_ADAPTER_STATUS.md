# V380 proprietary media adapter — status, blocker, and the firmware-generation split

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

**Neither camera is reachable.** Re-measured, not carried forward: `192.168.1.19` and `192.168.1.20` give
**no ICMP reply**, every tested port (554/8554/8899/8800/9800/80/443) is **filtered — timeouts, not refusals**,
and the neighbour table shows both as `FAILED`. Camera 3 (`192.168.1.10`) is absent from the neighbour table
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
3. The device id per unit (camera 3's `103287516` is on record; camera 1 and 2 device ids are not yet).

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
