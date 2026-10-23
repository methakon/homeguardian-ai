# Physical-camera evidence audit and release-compatible handoff (G04 / G10)

**Option B integration handoff.** This document audits the physical-camera evidence on the experimental
branch, states what is really demonstrated, and delimits it from the hackathon release. It is the whole
handoff: nothing here asks the release to import experimental code.

## Immutable reference

| Field | Value |
|---|---|
| Branch | `experiment/hg-v380-09-secure-camera-integration` |
| Audited state | `ee58844e2ca6dde52e111dd899304127b358efa5` |
| Provenance set at that state | 88 artifacts, all present at their named revision (0 missing) |
| This handoff | the commit that adds this file, directly on top of the audited state |

Verify the audited state before trusting any number here:

```sh
git fetch origin experiment/hg-v380-09-secure-camera-integration
git rev-parse origin/experiment/hg-v380-09-secure-camera-integration   # ee58844e2ca6dde52e111dd899304127b358efa5
git cat-file -e ee58844e:docs/evidence/provenance-manifest.json        # the cited set exists at that revision
cd docs/evidence && sha256sum -c SUBMISSION_ARTIFACT_HASHES.txt        # artifact hashes
```

Earlier commits named in the audit request, for continuity: `c4d3e9dd7b91fd75ffd9865e81fa88fecde18812`
(2026-10-19, the 37-artifact provenance report) and `91194f21daa350a781de03ad12d0a84f9f7ac60a` (2026-10-20,
83 artifacts). 37 commits lie between `c4d3e9d` and the audited state. The count moved 37 → 83 → 88 as
implementation and evidence were added; the *classification* of each artifact did not change silently.

## 1. Scope: what this package is, and what it is not

**Is:** an audited, reproducible record of a bounded experiment with one authorized physical USB UVC camera,
plus the tests that show the sensor spine it fed. Usable by judges as optional experimental work, and by
release integration as an explanation of why the experimental camera system is not in the release.

**Is not:** part of PR #20. The physical camera driver (`V4l2CameraDevice`) and the V380 bridge
(`V380SourceAdapter`) are **not** referenced by the release and are **not** required to build or run it. The
release manifest is unchanged by this package; no release path points into these documents or sources.

## 2. Provenance matrix — every physical-camera claim against a real source

Taxonomy: **REAL** = authorized physical USB UVC camera input. **TRANSCRIBED** = reported live console
output without independently preserved raw output. **REPLAY** = processing of previously captured values.
**SYNTHETIC** = generated fixture input. **UNSUPPORTED** = not demonstrated.

| # | Claim | Class | Backing artifact (at the audited SHA) |
|---|---|---|---|
| 1 | A USB UVC camera was opened at a fixed geometry and produced frames without errors | REAL | `PHYSICAL_CAMERA_STILL_SCENE.log`, `PHYSICAL_CAMERA_AMBIENT_RUN{,_2,_3}.log` |
| 2 | Ambient (no deliberate subject) motion fraction and change-character were measured | REAL | `PHYSICAL_CAMERA_AMBIENT_RUN_3.log`; `scenario-ambient_baseline*.log` (3 runs) |
| 3 | A deliberate brightness step (light switched) was captured and classified | REAL | `PHYSICAL_CAMERA_BRIGHTNESS_STEP.log`, `PHYSICAL_CAMERA_BRIGHTNESS_STEP2.log` |
| 4 | The character descriptor separated that global illumination from localized motion | REAL | `PHYSICAL_CAMERA_CHARACTER_RUN.log`, `PHYSICAL_CAMERA_CHARACTER_RUN2.log`; `docs/CHANGE_CHARACTER_SEPARATION.md` |
| 5 | Initial capture produced a large change on the first frame pair | REAL | `PHYSICAL_CAMERA_CHARACTER_RUN.log` (0.3522) — see §3, re-interpreted as exposure settling |
| 6 | Live console narrative of the capture session | **TRANSCRIBED** | `docs/evidence/PHYSICAL_CAMERA_CAPTURE_LOG.md` — no raw console dump was preserved; treat as operator narrative, not transcript-grade evidence |
| 7 | Scenario records aggregated from physical runs | REAL (derived) | `scenario-records-20261019T154658Z.json` |
| 8 | Bounded media buffering under slow-consumer load, drop accounting, ceilings | SYNTHETIC | `tests/core/test_media_backpressure.cpp` (`[mediabp]`) |
| 9 | Frame pair → bounded scalar derivation identical to the engine's own descriptor | SYNTHETIC | `tests/core/test_local_media_observation.cpp` (`[localmedia]`) |
| 10 | Media → ingest → detectors → real engine → real MCP, with the media boundary asserted | SYNTHETIC frames, REAL engine | `tests/integration/test_local_media_to_mcp.cpp` (`[mediachain]`) |
| 11 | Corroborated evidence produces a genuine alert with accurate provenance | SYNTHETIC | `tests/integration/test_network_sensor_to_mcp.cpp` (`[e2e]`); runner prints the real `get_alert_summary` |
| 12 | aarch64 codegen for the hub sources | REAL (host output, **not** camera evidence) | `docs/evidence/ARM64_CROSS_COMPILE.log` — a *build* log; it is `real` in the manifest's sense (genuine host output) but demonstrates nothing about the camera |
| 13 | Species classification, disaster prediction, real camera-to-fused-alert operation, two-way V380 audio | **UNSUPPORTED** | no artifact; see §6 |

REPLAY: no artifact in the set is labeled replay. Where previously captured scalars were re-fed to a rule,
the run is class **REPLAY** and is marked as such in its own file name or log header; none is cited as
physical evidence.

## 3. Correction: the first large change was the camera settling, not household movement

An earlier interpretation read the large change on the opening frame pair of the physical character run as
substantial movement across the scene. That reading was **wrong, and it is withdrawn**. The artifact shows a
**spread, unipolar** change on the *warm-up* pair — the signature of the camera settling its exposure between
the first two frames, not of a subject crossing the room. The corrected wording is what
`docs/CHANGE_CHARACTER_SEPARATION.md` carries today:

> the single large change is a **spread, unipolar** change on the **warm-up** frame pair: the camera settling
> its exposure between the first two frames

Consequences that are already implemented, not just documented: a warm-up observation is `inconclusive`, so
it casts no vote in the episode path (asserted by test). Distinguishing "something walked past" from "the
exposure settled" is exactly what the region-spread and bipolar-balance descriptors exist to do, and the
distinction is asserted by test rather than assumed.

No claim anywhere in this package attributes initial exposure change, ambient drift, or camera gain artifacts
to household movement or animal activity.

## 4. `require_motion_character` remains OFF, and why it is not a production detector

- The default is `false` (`src/core/ActivityEpisode.h:77`) and nothing in the release path enables it.
- It is a **gate**, and a gate that rejects evidence cannot create a detection: with it on, an episode whose
  members lack a motion-character result is suppressed. That is a safety property, not a sensitivity one.
- The thresholds it depends on were measured on **one camera, one exposure, one lighting condition**. The
  ≈1% ambient margin documented in `CHANGE_CHARACTER_SEPARATION.md` is a property of that sensor in that
  room and must be re-measured before it is trusted anywhere else.
- Therefore it is not accepted as a calibrated production detector, and the experimental camera system is
  not part of the release. Turning it on in the candidate would be a claim about calibration that has not
  been earned.

## 5. Evidence segment (optional, 15–20 s)

**Preferred, zero capture, judge-usable immediately: replay of committed real output.**
`scripts/evidence_segment_15s.sh` prints the key *committed* lines of the physical runs with ~18 s of
pacing. It starts no camera and changes no setting; it shows recorded output, and it says so on screen. A
recording of this script is REPLAY-class by construction.

```sh
scripts/evidence_segment_15s.sh
```

**Alternative, only with the owner present and consenting: a live 15–20 s segment.** Frame it so that
nothing private and nothing unaudited enters the recording:

1. Camera aimed at a wall or floor in the designated non-private area — **no faces, no household members,
   no identifying objects, no documents, no screens**.
2. Microphone **not** captured; the segment is picture-only.
3. One deliberate brightness step, then the ambient/quiet scene. 15–20 s total, one take.
4. Label it on screen or in the caption as experimental and not part of the submitted release.
5. **Do not** record a person as "movement" evidence — a consented participant makes the result
   non-anonymous, and the private-identity rule in §7 then applies.

Sanitization: no device credentials, no IP addresses, no household identifiers, no vendor app screens, no
metadata that names the location. Retain nothing beyond the segment; the retained files stay under the
shared media policy in `tests/media/` (`real/` and `quarantine/` are mode 0700, never publicly served, never
committed). Raw frames are not retained for this package at all — the evidence is scalars and logs.

## 6. Claim language

**Permitted** (each backed by the matrix above):

- "We opened an authorized USB UVC camera locally and measured bounded motion/change scalars from it in a
  non-private test area; raw frames stayed on the machine."
- "A deliberate light-switch event was captured, and a region-spread / bipolar-balance descriptor
  distinguished that global illumination change from localized change in our tests."
- "The sensor path — authenticated observation → fusion engine → read-only MCP status and alerts — is
  covered by 9558 assertions in 721 test cases, with the media boundary asserted."
- "The experimental camera integration is under review and is not part of the submitted release."

**Unsupported — must not appear in any submission text:**

- animal-species classification, animal-activity conclusions about real animals;
- disaster or calamity prediction;
- "real camera-to-fused-alert operation" — the alert path is proven with synthetic corroborating evidence,
  not from live camera input end to end;
- two-way V380 audio, V380 audio extraction, or any V380 media acquisition;
- RTSP/ONVIF availability on the V380 (measured absent in the default state; the vendor app switches were
  documented, not verified by this package);
- any deployment claim: no Mi A3 install, no always-on host, no Oracle media relay;
- "calibrated detector" for `require_motion_character`.

## 7. Camera 3 private identity (preserved — not re-opened)

The Camera 3 identity correction stands as committed and is **not** revisited by this audit; the OCR/
identity reasoning and its correction history are preserved at the audited revision. Nothing in this package
names a camera by an address, and no credential or household identifier is included.

## 8. Backlog, explicitly not deployable

`SiteConnector` and the media-adapter architecture remain **documented backlog**. A functional source
framework does **not** establish a deployable connector, an authenticated V380 media stream, or an always-on
host. No connector is deployed, none is installed on the Mi A3, and no media path through Oracle exists.

## 9. Reproduction

```sh
# the full verification, in one command
scripts/linux_hub_integration_check.sh          # => VERDICT: PASS, prints the real get_alert_summary

cmake -S . -B build && cmake --build build -j8 --target homeguardian_tests
./build/homeguardian_tests                      # 9558 assertions / 721 cases
ctest --test-dir build                          # 3/3
python3 scripts/verify_evidence_provenance.py   # 88 artifacts, 0 failures
```

Physical-camera artifacts were produced with the authorized USB UVC camera on this host; reproducing them
requires the same hardware present, which is why they are preserved as logs rather than re-run for the
submission. The synthetic suites require no hardware and are the parts a judge can re-run unaided.
