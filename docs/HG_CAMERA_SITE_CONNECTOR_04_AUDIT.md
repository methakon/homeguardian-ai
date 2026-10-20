# HG-CAMERA-SITE-CONNECTOR-04 — camera integration readiness audit

**Worker:** Hermes Session 3 (camera integration readiness). **Scope:** audit, isolated tests, documentation and
bounded corrective patches only — no camera settings changed, no capture, no bypass, nothing merged.

## Deliverables

| Field | Result |
|---|---|
| **MISSION** | HG-CAMERA-SITE-CONNECTOR-04 |
| **BRANCH** | `experiment/hg-v380-09-secure-camera-integration` |
| **EXACT SHA** | audited state `6404b7ff59e74a4f799a17322589568a137b664e`; evidence handoff `9d252c46f6fca2008e5e0285107002470d451df4` (exists, verified) |
| **CAMERA INVENTORY** | **PARTIAL** — three units identified by device id + NIC fingerprint, all reachable, all interfaces characterised; per-unit ONVIF *firmware support* still unknown for two of them |
| **AUTHORIZED MEDIA PATH** | **BLOCKED** — no authenticated media listener exists on any unit; the only media-bearing interface is the proprietary protocol, closed on this generation |
| **SITE CONNECTOR** | **TEST-ONLY** — framework + mock-backed rehearsal + a local USB-UVC bridge; no deployable network connector binary |
| **IDENTITY / CONSENT** | **PASS** — logical identity independent of address; consent, revocation and per-frame authorization enforced and tested |
| **REAL UVC EVIDENCE** | **PASS** — 13 real physical-camera logs + preserved narrative, classified and hash-pinned |
| **V380 STREAMING** | **BLOCKED** |
| **ANIMAL DETECTION** | **NOT IMPLEMENTED** (motion/change characterisation only; no species claim anywhere) |
| **SECURITY GAPS** | see §7 |
| **TEST COMMANDS / RESULTS** | see §5 |
| **OWNER APPROVALS REQUIRED** | see §8 |
| **NEXT RECOMMENDED ACTION** | see §9 |

---

## 1. Phase A — evidence and inventory verification

**Branch and SHAs.** Working tree clean. `HEAD == origin == 6404b7ff…`; the handoff commit `9d252c46…`
resolves. The provenance set **at that commit** is 88 artifacts: real 14, transcribed 1, synthetic 21,
implementation 35, documentation 17; scope external 83 / release 5.

**Hash pins — one audit defect found and fixed.** `SUBMISSION_ARTIFACT_HASHES.txt` verified **19/20**; the
mismatch was `provenance-report.json`, which the validity checker *regenerates* on every run. A generated file
cannot hold a pin, and it is not evidence — so it has been removed from the pinned set rather than "re-pinned"
(which would fail again on the next run). Re-verified: **19/19 OK, 0 failed**. The pinned set now contains only
immutable artifacts: the physical logs, the transcribed narrative, the docs, the segment script.

**Classification separation** (unchanged this pass):

- **REAL** — 13 physical USB UVC capture logs + scenario records. Bounded scalars only; no frame, audio sample
  or media file was retained at any point.
- **TRANSCRIBED** — one document. It is the operator narrative of that session; **no raw console dump was
  preserved**, so it is not transcript-grade evidence and nothing is cited from it that a log does not carry.
- **SYNTHETIC** — 21 suites (media backpressure, local-media derivation, media chain, network ingest,
  end-to-end), plus implementation and documentation artifacts.
- **REPLAY** — no artifact is labelled replay; nothing is cited as physical evidence.

**Identity consistency with owner-approved private records.** Three units, keyed on device id + NIC
fingerprint, both of which were *stable while their DHCP addresses moved* (two units moved during the work).
Real identifier/address/fingerprint values live only in the private evidence directory outside every
repository, mode 600; the repository holds placeholders. Camera identity was corrected once by the owner and
that correction is preserved, not revisited.

**Experimental alert gate.** `require_motion_character` defaults to `false`
(`src/core/ActivityEpisode.h:77`) and nothing in the release path enables it.

**Absence-as-success: none, and the opposite is asserted.** The boundary suite explicitly checks that a missing
capability is announced as `unavailable`, never as "no activity" (`tests/camera/test_camera_boundary.cpp:270-275`),
and the camera suites contain no assertion that treats an absent stream as a successful integration.

## 2. Phase B — camera interface feasibility (measured, exposure-only)

Measured per unit with one TCP connect per port — no credentials sent, no login attempted, no banner read,
nothing extracted.

| Service | Unit A | Unit B | Unit C |
|---|---|---|---|
| 23 telnet | refused | refused | refused |
| 554 RTSP | refused | refused | refused |
| 8899 ONVIF | refused | refused | refused |
| 8800 / 9800 vendor | open | open | open |
| Proprietary login | refused `-100` | refused `-100` | refused `-100` |
| Stream login | `401` / `v21 = -11` | `401` / `v21 = -11` | `401` / `v21 = -11` |

1. **Observed capabilities:** vendor protocol only, on the **encrypted generation** (`v21 = -11`), which public
   reverse-engineering attributes to a key reachable only by unpacking a packed vendor app at runtime. No
   standard listener on any unit.
2. **Documented authenticated media interface supported? No** — none exists to authenticate to at measurement
   time. This is an *absent interface*, not a hardened one.
3. **Unknown device-side:** whether unit C's firmware implements ONVIF at all (its app toggle produced nothing
   observable on 554/8899 or to a WS-Discovery probe); whether any unit would *enforce* authentication on RTSP
   if enabled (unit A's earlier enablement produced **anonymous** RTSP — the reason it stays disabled); the true
   codec (vendor documentation says H.264, a measured SDP said H.265).
4. **Standard vs proprietary, separated:** standard ONVIF/RTSP = absent today and contingent on an owner-side
   toggle per unit; proprietary = present but closed on this generation.
5. **Could a trusted connector reach a camera without weakening security? Not today.** A connector may only
   attach to a listener that exists *and* enforces authentication. Nothing satisfies both, so this is
   **NOT IMPLEMENTED** rather than approximated.
6. **NOT IMPLEMENTED:** video, microphone, PTZ, two-way audio — on all three units.

No anonymous ONVIF/RTSP was enabled, no proprietary bypass attempted, no credential extracted, no camera
rebooted or reconfigured.

## 3. Phase C — C++ SiteConnector readiness

1. **Executable deployment: no — TEST-ONLY.** `hg_site_connector_rehearsal` is documented in its own CMake
   block as opening *no socket and speaking no camera protocol*: every adapter is `MockCameraAdapter`. The other
   relevant binary, `hg_camera_bridge`, is the **local USB UVC** path (V4L2, operator-gated with `--confirm`),
   not a network connector. Both are deliberately kept out of the MCP server and web demo. A deployable
   network-camera connector **does not exist in this tree**.
2. **Trusted always-on connector host verified? No.** The intended hub is another workstream's device and no
   connector host has been provisioned or verified here.
3. **Identity independent of camera IP: yes.** Logical identity is the device id + fingerprint; the address is
   an attachment resolved at runtime. Tested: resolution, DHCP move (`resolved_moved`), fingerprint mismatch
   refusal, both conflict shapes, cache expiry, invalidation, unregistered id (44 assertions).
4. **Authorization / consent / revocation / reconnection:** one read-only authorizer answers a single question
   by reading revoked → paired → credential binding → consent → capture **in that order**, so the reported
   reason is the fact actually blocking it; an empty credential is refused rather than treated as "no check
   needed"; authorization is re-checked **before every frame**, not once per session.
5. **Authenticated, provenance-labelled observations: yes on the network path** — producer + authenticated
   ingest + detectors; provenance is *derived* (a mock adapter can only produce synthetic provenance, with no
   override).
6. **Can it run without the development workstation? Not verified** — no host chosen, so this stays open.
7. **Mi A3 CPU/RAM/network: not measured, and not claimed.** The only figures held are from this workstation's
   bounded media path (2.7–4.0 µs/frame, 8.1 MiB RSS) and they must not be re-labelled as Mi A3 results.
8. **Media direct, not via Oracle:** the design is direct/local; no relay is implemented and none would carry
   media through the VPS.

## 4. Phase D — isolated adapter tests (the ten required scenarios)

| # | Scenario | Coverage | Result |
|---|---|---|---|
| 1 | Registry authorization | `[camera]` boundary + authorizer cases | PASS |
| 2 | Stable logical identities | `[camera]` inventory + `[resolve]` | PASS |
| 3 | Changing device IP addresses | `[resolve]` move/mismatch/conflict | PASS |
| 4 | Unauthorized connector rejection | `[camera]` boundary | PASS |
| 5 | Revoked consent | `[camera]` + `[v380]` | PASS |
| 6 | Stale / replayed observations | `[e2e]` + `[reliability]` | PASS |
| 7 | Disconnect / reconnect | `[camera]` boundary (`reconnect` returns named outcomes) | PASS |
| 8 | Source provenance | `[camera]` + `[producer]` | PASS |
| 9 | Unavailable media cannot create a detection | `[camera]` boundary (`unavailable`, never "no activity") | PASS |
| 10 | A failed camera connection does not crash the core | `[camera]` (`refused_unsafe_interface`, `capability_absent`) | PASS |

## 5. Test commands and results

```sh
HG_SOURCE_DIR=$PWD ./build/homeguardian_tests '[camera]'      #   646 assertions /  39 cases  PASS
HG_SOURCE_DIR=$PWD ./build/homeguardian_tests '[v380]'        #   195 /  22                  PASS
HG_SOURCE_DIR=$PWD ./build/homeguardian_tests '[v380proto]'   #   237 /   9                  PASS
HG_SOURCE_DIR=$PWD ./build/homeguardian_tests '[resolve]'     #    44 /   8                  PASS
HG_SOURCE_DIR=$PWD ./build/homeguardian_tests '[mediachain]'  #    32 /   3                  PASS
HG_SOURCE_DIR=$PWD ./build/homeguardian_tests '[net]'         #   723 /  24                  PASS
HG_SOURCE_DIR=$PWD ./build/homeguardian_tests '[producer]'    #    90 /   7                  PASS
HG_SOURCE_DIR=$PWD ./build/homeguardian_tests '[reliability]' #    89 /   5                  PASS
HG_SOURCE_DIR=$PWD ./build/homeguardian_tests '[e2e]'         #   197 /  15                  PASS
HG_SOURCE_DIR=$PWD ./build/homeguardian_tests                 #  9862 / 742                 PASS
ctest --test-dir build                                        # 2/3 — see §7.4 (not this workstream)
```

## 6. Phase E — physical observation boundary (wording rules preserved)

The submission wording must keep these apart, and the handoff document does:

- **Real** physical USB UVC frames reduced to bounded scene-change scalars (13 logs).
- **Synthetic/replayed** detector inputs (21 suites) — never presented as camera results.
- **Experimental** character-separation work — the descriptor's own labels, not detections.
- **Real alert generation** (from corroborated synthetic evidence, via the real rule and dispatcher) versus any
  hypothetical/replayed alert.
- **Animal/species recognition: NOT VERIFIED** — no claim in any tense.
- **Disaster prediction: NOT ESTABLISHED** — no claim in any tense.

No new physical media capture was performed or required for this audit, and no experimental alert rule was
enabled to manufacture a positive demonstration.

## 7. Security gaps

1. **Unit A's standard-media interface was measured as anonymous when enabled** (all paths returning content
   without a credential). It stays disabled, and re-enabling it requires the security check to pass first.
2. **No authenticated media path exists on any unit today** — the single open surface is the proprietary
   protocol on the encrypted generation, closed to protocol work. Correctly reported as BLOCKED, not "in
   progress".
3. **Isolation is the compensating control** and is not yet verified: no confirmation that the router forwards
   no port to a camera. Reading that needs the owner's router credential (single attempt; the gateway locks
   after three failures).
4. **`ctest` provenance validator fails on a release-scope entry owned by another workstream** (a declared
   marker missing from a document on the release branch). It reproduces with this work stashed, so it is not
   caused here; both the entry and the file belong to another lane and were left untouched.
5. **One generated file had been hash-pinned** — found and fixed in this pass (§1).

## 8. Owner approvals required

1. **Per-unit ONVIF switch confirmation** — whether an ONVIF setting exists at all on units B and C (a single
   app screenshot decides it; the network cannot).
2. **Enabling a standard interface on a unit** if it exists — with the security check run *before* any stream
   is used, and unit A kept disabled until its interface is shown to enforce authentication.
3. **Router forward-list check** (needs the gateway credential) to prove no camera port is exposed.
4. **Any renewed household media capture** — not required for this audit and not requested.
5. **Hub/host decisions** (Mi A3 deployment, process supervision) remain another workstream's.

## 9. Next recommended action

**Recommend the audit stand as the answer for now: the V380 media path is BLOCKED, and the MCP submission must
not depend on it.** Concretely, in priority order:

1. Obtain the two app screenshots that settle whether an ONVIF switch exists on units B and C. If none exists,
   those units are permanently control-only and the question closes.
2. For any unit with a switch: enable it, then run `scripts/v380_security_check.sh <host> 554 8899` **before**
   anything touches a stream. On `EXPOSED`, keep it disabled and rely on isolation; only on `AUTHENTICATION
   ENFORCED` proceed to a connector.
3. Only after a passing security result: wire RTSP behind the existing site-connector seam and re-run the ten
   scenarios plus the media-chain suite for a *real* (not mock) provenance path.
4. Meanwhile, verify the isolation claim (no camera port forwarded) and keep the submission's camera claims at
   "experimental, not in the submitted release".

**Explicitly BLOCKED rather than insecure:** no anonymous listener was enabled, no proprietary bypass attempted,
no credential extracted, no camera reconfigured, no media relayed, nothing merged.
