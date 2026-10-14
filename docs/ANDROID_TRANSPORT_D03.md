# Android Transport (D-03) — what exists, and what has actually been executed

This document exists because "the Android side is implemented" can mean three different things, and
only one of them is true today.

| Layer | State | Evidence |
|---|---|---|
| Android acquisition (camera/mic, consent/permission gating, per-payload accounting) | **Implemented in D-02, compile-verified** | `apk/` builds; `scripts/verify_android_node.sh` 18/18 |
| Android transport (bounded queue + framed mutual TLS) | **Implemented in D-03, compile-verified only** | `apk/src/org/homeguardian/harness/ObservationTransport.java`; APK builds |
| Wire protocol conformance | **Executed** | The C++ mock client (`src/main_device_client.cpp`) speaks the same frames; `scripts/verify_device_transport.sh` runs it against the real ingress |
| Physical device → hub delivery | **NOT PROVEN** | No phone attached. `adb devices` is empty |

## What the Android transport does

`ObservationTransport` is a bounded, self-contained sender:

- **Bounded queue (128 observations).** When it is full the OLDEST entry is dropped and a counter
  records it. A phone that loses the network must not grow without limit, and for telemetry the
  stale end of the queue is the right thing to lose.
- **One worker thread**, explicit reconnects with a bounded backoff (3 s), and a status line
  (`online/offline queued= sent= dropped= refused=`) for the node UI.
- **Mutual TLS** via `SSLContext` built from a client PKCS#12 and a trust store containing the
  private CA. Protocols are pinned to TLS 1.2/1.3, and verification is never disabled.
- **Framing identical to the hub's**: 4-byte big-endian length + JSON body, capped at 4096 bytes.
- **Compact observations only.** The class cannot carry raw audio, raw video, a frame buffer, a
  transcript or a biometric template: it serializes a handful of doubles and short strings, and it
  never starts acquisition itself.
- **No credential in an observation or a log.** The PKCS#12 password is read from a separate
  app-private file into a `char[]` used only to unlock the keystore; exception messages are logged
  as the exception CLASS only, so a keystore error cannot print material.
- **Provenance is honest by construction**: the message carries `acquisition_mode` and a provenance
  claim, and the hub derives the recorded provenance itself. A claim it cannot substantiate is
  recorded as synthetic and flagged (see `docs/DISTRIBUTED_DEVICE_TRANSPORT_D03.md` §2.4).

## How an operator would provision a phone

1. On the hub host: `bash tools/provision_ca.sh issue <device-id> deploy/docker/certs` and register
   the printed `credential_id` in `devices.json` (the tool prints the exact entry).
2. Convert the issued certificate + key into a PKCS#12 and push it into the app's private directory
   (`filesDir`), with the password in a separate `0600` file:
   `openssl pkcs12 -export -out <id>.p12 -inkey <id>.key -in <id>.crt -certfile ca/ca.crt`
   then `adb push` into the app's private files dir and `chmod 600`.
3. Point the app at the hub (`host`, `port`, and the trust store built from `ca/ca.crt`).
4. Grant consent in the app, press **START camera capture** (the OS permission prompt follows), and
   watch the counters. Capture still requires the operator action: pairing alone activates nothing.

**Not yet hardened:** the PKCS#12 sits in app-private storage. The stronger option is importing the
key into the Android Keystore and using a `KeyChain`-based manager, which is the recommended
follow-up before this is used on a device that is not the operator's own. It is recorded here rather
than presented as done.

## Why the delivered end-to-end evidence is the C++ client

The hub accepts one protocol. What must be proven is that the BOUNDARY enforces authentication,
consent, bounds, replay protection and provenance — and that is proven by driving the real ingress
with a real client over real TLS, including adversarial modes a cooperative Android app would never
produce (replay, future timestamps, unlisted source types, oversized frames, missing handshake).
`hg_device_client` is that client. When a phone is available, the same script's steps are repeated
from the device, and D-03's physical acceptance stops being BLOCKED at that point.

## D-03.1 Android transport changes

The transport now enforces the acknowledgment contract rather than trusting it:

- An acknowledgment is matched to the exact observation id, sequence and protocol major
  version; a mismatch is treated as a violation and never counted as a delivery.
- Refusals are classified into suspending / permanent / retryable, with a bounded retry
  count (4) and a documented drop for each class. An unrecognised reason is treated as
  permanent: retrying a refusal whose meaning is unknown is how a node loops.
- Consent withdrawal, capture disable and revocation suspend transmission and flush the
  queued observations, because sending work collected under a permission that no longer
  holds would be the wrong answer.
- `stop()` bumps a generation, closes the active socket and joins with a bounded wait, so
  shutdown cannot leave a worker running and a restart cannot leave two.
- The sequence counter adopts the hub's `last_sequence` from the welcome, which is what
  makes an application restart legitimate instead of a replay.
- Features are validated (finite, in range) before queueing; the password file read is
  bounded to 512 bytes and the keystore password buffer is wiped after use.
- Nothing logs credentials or raw media.

Verification status: compile-verified (signed APK builds, dex contains
`ObservationTransport`). The behaviours above are exercised by the C++ transport contract
tests and the native client; they are NOT yet exercised on a physical device.
