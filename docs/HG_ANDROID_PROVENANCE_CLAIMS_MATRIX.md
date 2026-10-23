# Android provenance / claims matrix (Devpost)

Deadline override — reporting artefact, no new coding.
**Android lane SHA: `b311f790a261c0918853e22be90eb8ccba9725b0`**
Merged judge demo under evaluation: PR #20 → `main` @ `366f4a950e0ae7e8416a95e3a10019694184a6fd`

## 1. Is any Android capability required for the judge demo? **No.**

Read from `docs/DEVPOST_DEMONSTRATION_GUIDE.md` on the merged commit, not assumed:

```
Host prerequisites (guide §Missing Prerequisites):
  curl, jq, cmake >= 3.20, make, g++ (C++20), node >= 18
  -> no APK, no ADB, no handset, no Android SDK

Network prerequisites: "No Cloud or Network Required" - all checks over 127.0.0.1 loopback
Runner:                bash scripts/verify_synthetic_mcp_demo.sh [--serve]
```

The demo is the C++ analyzer plus the read-only MCP server, driven by the **synthetic test provider**
(`camera-syn-001`). Android is a *producer* of observations and is not in the demo's execution path.
The guide already states this in its own words:

> Android node transport is therefore evaluated cleanly using **synthetic test observations and isolated
> loopback fixtures**, holding the unstable physical handset fail-closed (`physical_camera_unverified`)
> so hardware glitches never interrupt the judge demonstration.

**Therefore: the Android lane is safe to freeze. Nothing in this lane needs to ship for the demo.**

## 2. Provenance matrix

| Claim | Status | Evidence |
|---|---|---|
| Android **serializer** produces protocol-correct frames | **VERIFIED** | `apk/src/.../ObservationTransport.java`; frames asserted on the bytes actually emitted |
| Those frames are accepted by the **real C++ D-03 ingress** | **VERIFIED** | `scripts/run_real_ingress_e2e.sh` → 19/19; real mTLS to `hg_device_ingress` |
| Typed **detector-capable** streams (motion + presence) | **VERIFIED** | `apk/tests/DetectorProducerFixtureTest.java` → 9/9 |
| The fixture **produces a committed alert** | **VERIFIED** | hub's own `audit.db`: `detection-fusion`, `warning`, `confidence=0.9` |
| Availability-only input produces **0 alerts** | **VERIFIED** (negative control) | same fixture, `evidence_detector_ids=[]` → accepted, 0 alerts |
| Replay / restart / clock-skew / stale-timestamp handling | **VERIFIED** | suite + live ingress cases A–E |
| **Physical phone used in any test** | **NO** | no device in any run; loopback only |
| **Physical camera or microphone activated** | **NO** | never; every frame is synthetic |
| **Raw video media stream from Android** | **NO** | Android side sends **observations only** — numeric features + state, no media |
| **End-to-end Android → alert → MCP in one demo** | **NOT CLAIMED BY THIS LANE** | my runs stop at the committed alert; the MCP read-back is Hermes 3's `4bc964d` proof (see §4) |
| **On-device APK evidence** | **NONE** | nothing installed; the harness APK is a pre-existing lab artefact |

## 3. The honest one-liner

> Android contribution is **a synthetic, serializer-driven fixture proven against the real C++ ingress,
> including a fixture that drives a real committed alert**. It is not physical sensing, and the judge
> demo does not depend on it.

## 4. What this lane does NOT claim

- **No physical camera or voice integration proof.** No sensor was activated in any run.
- **No raw video media.** Android emits observations (features + state), never media; the demo's
  synthetic video test pattern comes from the simulator (`camera-syn-001`), not from Android.
- **No Android → MCP round trip demonstrated by this lane.** I verify up to the committed alert in the
  hub's store. The independent read-only MCP read-back was demonstrated separately by Hermes 3 at
  `4bc964d` with the same alert shape; I am not claiming it as my own evidence.
- **No on-device execution.** All Android-side results are JVM + real-socket, not on-handset.

## 5. Evidence links

```
Lane SHA          b311f790a261c0918853e22be90eb8ccba9725b0
Command           bash scripts/run_real_ingress_e2e.sh
Results           real ingress 19/19 | pinned vector 19/19 | live vector 19/19 | detector fixture 9/9
Isolated suite    45 + 33 + 27 + 59 + 34 + 19, 0 failed, exit 0
Fixture manifest  hg_e2e_s2/detector-fixture/manifest.json  (per-stream sha256 + required allowlist)
Issues            #24 (coordination), #25, #26 (E2E owner)
```

## 6. 12-second video script (optional, truthful as written)

| t | On screen | Voice |
|---|---|---|
| 0:00–0:04 | Terminal: `bash scripts/run_real_ingress_e2e.sh` scrolling to `PASS: all 19` | "The Android serializer talks to the real C++ ingress over mutual TLS." |
| 0:04–0:08 | Cut to `detector-fixture/manifest.json` — the two typed streams with sha256 | "Two synthetic streams — motion and presence — typed so the engine counts them as evidence." |
| 0:08–0:12 | Cut to the alert row: `detection-fusion · warning · confidence 0.9` | "Synthetic observations, a real fused alert. No camera, no microphone, no phone." |

If a 10-second cut is needed, drop the middle row and keep 0:00–0:04 and the alert row.
