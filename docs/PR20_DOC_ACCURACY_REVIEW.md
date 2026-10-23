# PR #20 documentation accuracy review — claims matrix, redlines, camera-evidence segment

**Review scope:** physical-sensor workstream
**Artifact under review:** PR #20 `release/hackathon-2026-mcp-candidate` → `main`
**PR head reviewed:** `2be31239a8600936b6218b8a1b6148831f5c1b86`
**Method:** clean clone of the PR head, built locally, every count in this document **re-executed**.
Where a document's number differs from this review's result, the measured value below is what the command printed.
**Scope of edits:** none. This branch is isolated; nothing here touches the release branch.

---

## 1. Claims matrix

Measured on the PR head, `2026-10-19`, from a clean clone at `hg_build/` (see §5 for the exact
commands and their printed output).

| # | Claim (source) | Claimed | **Measured** | Verdict |
|---|---|---|---|---|
| 1 | C++ cases — `docs/PROOF_REGISTER.md` §3, `docs/DEVPOST_SUBMISSION.md:59`, `docs/SUBMISSION_VIDEO_SCRIPT.md:99,104`, `docs/DEVPOST_DEMONSTRATION_GUIDE.md:188`, `docs/SECURITY_READINESS_V8.md:278`, `docs/RELEASE_AUDIT_STATUS.md:73` | 526 | **527** | WRONG (stale by one case) |
| 2 | C++ assertions — same six places | 38,926 | **38,929** | WRONG (stale by three) |
| 3 | C++ cases — `docs/DEVPOST_SUBMISSION.md:4`, PR #20 description | 574 | **527** | WRONG (this is another branch's count) |
| 4 | MCP protocol checks — `README.md:201`, `docs/PROOF_REGISTER.md` §2 row 7, `docs/DEVPOST_DEMONSTRATION_GUIDE.md:192` | 71 | **73** | WRONG (`verify_mcp.sh` prints 73) |
| 5 | MCP protocol checks — `docs/DEVPOST_SUBMISSION.md:4`, PR #20 description | 73 | **73** | PASS |
| 6 | Dashboard / WebRTC — `README.md:203` | 158 | **161** | WRONG |
| 7 | Dashboard / WebRTC — `docs/DEVPOST_DEMONSTRATION_GUIDE.md:193` | 132 | **161** | WRONG |
| 8 | Dashboard / WebRTC — `docs/PROOF_REGISTER.md` §3, `docs/DEVPOST_SUBMISSION.md:59` | 71 | **161** | WRONG |
| 9 | Dashboard / WebRTC — `docs/DEVPOST_SUBMISSION.md:4`, PR #20 description | 161 | **161** | PASS |
| 10 | MCP client — `README.md:202`, Devpost, guide, register | 26 | **26** | PASS |
| 11 | Java E2E bridge — `README.md:204`, Devpost, PR description | 21 | **21** | PASS |
| 12 | Outbound bridge — `README.md:205`, Devpost, PR description | 46 | **46** | PASS |
| 13 | Independent security gates — `docs/PROOF_REGISTER.md`, Devpost, guide, video script | 15 | **15** | PASS |
| 14 | Alert retention — `docs/DEVPOST_DEMONSTRATION_GUIDE.md:195` | 8 | **8** | PASS |
| 15 | Build/deploy guards — `docs/DEVPOST_DEMONSTRATION_GUIDE.md:196` | 12 | **12** | PASS |
| 16 | Alexa skill — `docs/DEVPOST_DEMONSTRATION_GUIDE.md:194` | 77 | **77** | PASS — *only after `npm install` in `alexa_skill/`, see redline R9* |
| 17 | License is the unmodified Apache-2.0 text, sha256 verified — `README.md`, `docs/DEVPOST_SUBMISSION.md:10` | `cfc7749b…` | **`cfc7749b96f63bd31c3c42b5c471bf756814053e847c10f3eb003417bc523d30`** | PASS (independently confirmed) |
| 18 | "verified physical USB sensor evidence" — `docs/SUBMISSION_VIDEO_SCRIPT.md:4` | implied present in this build | **no artifact exists in this branch** | WRONG — see §3 |
| 19 | "no physical camera or microphone is ever activated" — `README.md:14-15, 84` | absolute | contradicted by `docs/DEVPOST_SUBMISSION.md:98` ("Android camera streaming integration has been verified in lab adb/Wi-Fi test harnesses") | CONTRADICTION — see §3 |
| 20 | V4L2 capture is "Planned (not implemented)" and "blocked pending explicit approval" — `README.md:89-90, 96-97` | not implemented | the V4L2 backend and the physical-capture tool exist and have been exercised under explicit consent (isolated branch) | WRONG direction (underclaim) |
| 21 | Species identification / animal identification from any signal | never claimed | no such claim exists in README, Devpost story or video script ✓; no classifier in the build ✓ | PASS (no claim) |
| 22 | Disaster / calamity / earthquake / flood prediction | disclaimed in `docs/DEVPOST_SUBMISSION.md:91` | not implemented and **unrepresentable** (fusion frame has no such element) ✓ | PASS (no claim) |
| 23 | "Achieved complete home monitoring" — `docs/DEVPOST_SUBMISSION.md:58` | complete | sensing is synthetic in the judge build; one consented USB capture exists outside this branch | OVERSTATED wording |

**Nothing in the three documents under review claims a camera-triggered alert, an animal
identification, or a disaster prediction.** The defects are: stale/incorrect counts (1-3, 4, 6-8),
a dangling physical-sensor claim (18), an internal contradiction (19-20) and two overstated phrases
(21 is clean; 23 and the WebRTC sentence in §4 R5 are wording).

---

## 2. Documentation redlines — exact replacements

Every `SEARCH` string below was verified to occur **exactly once** in its file at the PR head, so the
pairs can be applied mechanically.

### R1 — `README.md` hackathon note: scope the synthetic statement (matrix 19)

`SEARCH`
```
> an Alexa+ runtime and is not connected to Amazon in any way. All household
> observations remain **synthetic**; no physical camera
> or microphone is ever activated. See
```
`REPLACE`
```
> an Alexa+ runtime and is not connected to Amazon in any way. **In the demo and
> judge paths every household observation is synthetic, and no sensor is
> activated**: capture is disabled by default and requires an explicit operator
> flag. A separate, isolated physical-capture path exists on an experimental
> branch and was exercised once under the owner's explicit consent (see
> docs/SUBMISSION_PHYSICAL_SENSOR_INPUTS.md); it is not part of this build. See
```

### R2 — `README.md` "Simulated" bullet (matrix 19)

`SEARCH`
```
- All household observations are **synthetic**; no physical sensor is activated.
```
`REPLACE`
```
- All household observations **used by the demo and the test suites** are synthetic; no sensor is
  activated by the demo. A separate operator-gated physical-capture path exists on an experimental
  branch and is not part of this build (see `docs/SUBMISSION_PHYSICAL_SENSOR_INPUTS.md`).
```

### R3 — `README.md` hardware status (matrix 20)

`SEARCH`
```
  Hardware capture backend (`V4L2`/PipeWire) is disabled by default (`media_capture_enabled: false`)
  and blocked pending physical authorization.
```
`REPLACE`
```
  Media capture is disabled by default (`media_capture_enabled: false`). A **V4L2** capture backend
  (Linux UVC, no external library) and an operator-gated capture tool exist on the experimental
  branch `experiment/hg-v380-09-secure-camera-integration`; they reduce frames to bounded scalars,
  store nothing, and run only with an explicit `--confirm` flag. ALSA/PipeWire microphone capture is
  not implemented. Neither path is enabled in this build.
```

### R4 — `README.md` "Planned (not implemented)" list (matrix 20)

`SEARCH`
```
- Real camera/microphone capture backends (V4L2/ALSA/PipeWire) — **blocked
  pending explicit approval**.
```
`REPLACE`
```
- Real camera/microphone capture backends: **V4L2 video capture is implemented on the experimental
  branch** (consent-gated, scalars only, nothing stored); **ALSA/PipeWire audio capture is not
  implemented**; neither is enabled in this build.
```

### R5 — `README.md` test counts (matrix 4, 6)

`SEARCH`
```
bash scripts/verify_mcp.sh              # native protocol suite: 71 checks, 0 failures
```
`REPLACE`
```
bash scripts/verify_mcp.sh              # native protocol suite: 73 checks, 0 failures
```

`SEARCH`
```
node web_mcp/test_devices_dashboard.js   # dashboard/WebRTC/secrets: 158 checks, 0 failures
```
`REPLACE`
```
node web_mcp/test_devices_dashboard.js   # dashboard/WebRTC/secrets: 161 checks, 0 failures
```

### R6 — `docs/DEVPOST_SUBMISSION.md:4` certified note (matrix 3)

`SEARCH`
```
independent regression test suites (574 C++ tests, 161 UI/dashboard tests, 73 MCP protocol tests, 21 Java E2E bridge tests, 46 outbound bridge tests)
```
`REPLACE`
```
independent regression test suites (527 C++ test cases / 38,929 assertions, 161 dashboard/WebRTC checks, 73 native MCP protocol checks, 26 MCP client checks, 21 Java E2E bridge checks, 46 outbound bridge checks, 15 HTTP security gates)
```
Also in the same line: `@ commit \`1e3e778\`` is not the PR head — update it to the final release SHA.

### R7 — `docs/DEVPOST_SUBMISSION.md:59` accomplishments (matrix 1, 2, 8, 4)

`SEARCH`
```
The system is validated by **526 native C++ test cases with 38,926 assertions**, 71 WebRTC and dashboard tests, 26 MCP client tests, 71 native MCP protocol tests, and 15 independent adversarial HTTP security gates — 100% passing with zero failures.
```
`REPLACE`
```
The system is validated by **527 native C++ test cases with 38,929 assertions**, 161 dashboard/WebRTC checks, 26 MCP client checks, 73 native MCP protocol checks, and 15 independent adversarial HTTP security gates — 100% passing with zero failures.
```

### R8 — `docs/DEVPOST_SUBMISSION.md:32` WebRTC sentence (matrix 23)

`SEARCH`
```
A built-in WebRTC streaming engine enables real-time, low-latency browser monitoring.
```
`REPLACE`
```
A built-in WebRTC streaming engine delivers real-time, low-latency browser viewing of synthetic
test streams; every physical camera view request is refused at the gateway (HTTP 400
`physical_camera_unverified`).
```

### R8b — `docs/DEVPOST_SUBMISSION.md:58` "complete home monitoring"

`SEARCH`
```
- **Zero Cloud Exfiltration:** Achieved complete home monitoring and anomaly detection without sending a single byte of telemetry or media to external cloud infrastructure.
```
`REPLACE`
```
- **Zero Cloud Exfiltration:** No telemetry and no media leave the host — the judge demo's sensing is synthetic, anomaly detection runs locally, and nothing is transmitted to external cloud infrastructure.
```

### R9 — `docs/SUBMISSION_VIDEO_SCRIPT.md:4` physical-sensor claim (matrix 18) — **the sensor overclaim**

`SEARCH`
```
has passed all C++ core, MCP protocol, and outbound bridge test suites. All claims reflect implemented capabilities and verified physical USB sensor evidence.
```
`REPLACE` (choose one)
```
has passed all C++ core, MCP protocol, and outbound bridge test suites. Every claim in this script refers to what this build contains; the physical USB-camera work is documented separately on an unmerged experimental branch and is not presented here.
```
…or, if the camera artifacts are merged into the release (see §3 option A):
```
has passed all C++ core, MCP protocol, and outbound bridge test suites. All claims reflect implemented capabilities; the only physical-sensor evidence is the operator-consented USB-camera capture described in docs/SUBMISSION_PHYSICAL_SENSOR_INPUTS.md (bounded scalars, no frames retained, no camera-triggered alert demonstrated).
```

### R10 — `docs/SUBMISSION_VIDEO_SCRIPT.md:99,104` counts (matrix 1, 2)

`SEARCH`
```
  - Fast montage: C++ test suite finishing 526 tests / 38,926 assertions; 15 independent security gates green; GitHub repository and Apache 2.0 license.
```
`REPLACE`
```
  - Fast montage: C++ test suite finishing 527 tests / 38,929 assertions; 15 independent security gates green; GitHub repository and Apache 2.0 license.
```

`SEARCH`
```
  > "HomeGuardian AI is backed by 526 test cases, over 38,000 assertions, and 15 independent security verification gates — all green.
```
`REPLACE`
```
  > "HomeGuardian AI is backed by 527 test cases, 38,929 assertions, and 15 independent security verification gates — all green.
```

### R11 — `docs/DEVPOST_DEMONSTRATION_GUIDE.md:188,192,193,194` (matrix 1, 2, 4, 7, 16)

| SEARCH | REPLACE |
|---|---|
| `**526 / 526** (38,926 assertions)` | `**527 / 527** (38,929 assertions)` |
| `slowloris read deadlines, overload drain \| **71 / 71** \|` | `slowloris read deadlines, overload drain \| **73 / 73** \|` |
| `sequence replay protection \| **132 / 132** \|` | `sequence replay protection \| **161 / 161** \|` |
| `` `cd alexa_skill && npm test` `` | `` `cd alexa_skill && npm install && npm test` `` |

**Build-directory reproducibility (measured, and it bit me):** the `alexa_skill` suite spawns
`hg_build/hg_alexa_bridge`. Built into any other directory it reports **25 pass / 52 fail** with
`spawn .../hg_build/hg_alexa_bridge ENOENT`; built into `hg_build/` it reports **77 / 77**. The guide's
C++ and retention rows use `./build/…` while `README.md` says `mkdir -p hg_build`. Pick one directory
and use it everywhere, or set `HG_BRIDGE_PATH` explicitly (the retention row already does).

### R12 — `docs/PROOF_REGISTER.md` §3 aggregate (matrix 1, 2, 8, 4) and header

| SEARCH | REPLACE |
|---|---|
| `- **Total C++ Test Cases:** 526` | `- **Total C++ Test Cases:** 527` |
| `- **Total C++ Assertions:** 38,926` | `- **Total C++ Assertions:** 38,929` |
| `- **Dashboard / WebRTC Integration Tests:** 71` | `- **Dashboard / WebRTC Integration Tests:** 161` |
| `- **Native MCP Protocol Conformance Tests:** 71` | `- **Native MCP Protocol Conformance Tests:** 73` |
| ``**Candidate SHA:** `11ad94548f65bfe22a0db4297a6e92218448d1f4` (`11ad945`)`` | the final release SHA (currently `2be31239a8600936b6218b8a1b6148831f5c1b86`) |

Also `docs/PROOF_REGISTER.md` §2 row 7 says "71/71 protocol conformance checks" — same correction to 73.

### R13 — two remaining stale counts

| File | SEARCH | REPLACE |
|---|---|---|
| `docs/SECURITY_READINESS_V8.md:278` | `complete C++ suite: 526 test cases, 38926 assertions` | `complete C++ suite: 527 test cases, 38929 assertions` |
| `docs/RELEASE_AUDIT_STATUS.md:73` | `(38,926 assertions in 526 test cases)` | `(38,929 assertions in 527 test cases)` |

### R14 — licensing (three inconsistencies)

**a. Missing license declaration in both npm manifests** (npm treats an absent field as unspecified,
which contradicts "all original code is Apache-2.0"):

| File | SEARCH | REPLACE |
|---|---|---|
| `package.json` | `  "version": "1.0.0",` | `  "version": "1.0.0",\n  "license": "Apache-2.0",\n  "private": true,` |
| `alexa_skill/package.json` | `  "version": "0.1.0",` | `  "version": "0.1.0",\n  "license": "Apache-2.0",\n  "private": true,` |

**b. `NOTICE` omits two runtime dependencies of the shipped gateway.** `web_mcp/` requires
`werift` and `ws` (both MIT: `werift@0.25.0`, `ws@8.22.0` per `package-lock.json`), and the README
states the gateway/dashboard need them. Every other npm dependency is attributed; these two are not.
Add under the npm section:

```
Installed separately via npm for the local web gateway (web_mcp/; optional, not part of the
native demo):

  - werift — MIT License. https://github.com/shinyoshiaki/werift-webrtc
  - ws     — MIT License. https://github.com/websockets/ws
```

**c. The relicense is invisible where a judge looks.** `LICENSE` on `main` is the **MIT** license
(`bde998306b6ccabb3c6f752a9ffb4f26ed881c22af38f4dfcb1d05d120463241`), and this PR replaces it with
Apache-2.0. `docs/AMAZONAPPDEV2026_FINAL_AUDIT.md` records the owner's approval, but `NOTICE` and the
README licence section do not, so a judge diffing history sees an unexplained change. Add to `NOTICE`:

```
Early development of this repository was released under the MIT License; the project was
relicensed to the Apache License 2.0 by the copyright holder for this release (see
docs/AMAZONAPPDEV2026_FINAL_AUDIT.md, "P0-2 RESOLVED").
```

The licence *itself* is correct: `LICENSE` is byte-identical to the canonical Apache-2.0 text
(sha256 `cfc7749b…`, independently compared against `/usr/share/common-licenses/Apache-2.0`).

### R15 — animal / calamity disclosure (matrix 21, 22)

No animal-identification, species, calamity or disaster claim exists in the README, the Devpost story
or the video script, and `docs/DEVPOST_SUBMISSION.md:91` already disclaims disaster forecasting.
However the release ships `docs/ANIMAL_ACTIVITY_DETECTION.md`, whose result vocabulary includes the
label `possible_animal_activity`. A judge who reads that document should not have to work out whether
the submission claims animal recognition. Add a sixth bullet to the Devpost disclosure list:

```
6. **Environmental / activity layer (disclosed, not claimed):** the repository documents a bounded
   precursor-activity layer. Its only animal-related output is the label `possible_animal_activity`,
   a hypothesis about a multi-sensor activity pattern that is explicitly **not** a species
   identification, and it can never emit an earthquake, flood, storm or other calamity prediction
   (the fusion frame contains no such element). No classifier and no model runtime exist in the build.
```

---

## 3. The media gap: a claim whose artifacts are not in this branch

`docs/SUBMISSION_VIDEO_SCRIPT.md:4` claims "verified physical USB sensor evidence", and this PR also
ships `docs/SUBMISSION_PHYSICAL_SENSOR_INPUTS.md`. **None of the artifacts those documents cite exist
in the release branch.** Verified file by file at the PR head:

| Referenced path | In the release branch |
|---|---|
| `docs/JUDGE_EVIDENCE_PHYSICAL_SENSOR.md` | ABSENT |
| `docs/PHYSICAL_CAMERA_PATH.md` | ABSENT |
| `docs/evidence/PHYSICAL_CAMERA_CAPTURE_LOG.md` | ABSENT (no `docs/evidence/` directory) |
| `docs/evidence/PHYSICAL_CAMERA_STILL_SCENE.log` | ABSENT |
| `src/backend/linux/V4l2CameraDevice.{h,cpp}` | ABSENT |
| `src/core/CameraActivitySession.{h,cpp}` | ABSENT |
| `src/main_camera_bridge.cpp` | ABSENT |
| `tests/core/test_camera_activity.cpp` | ABSENT |
| `tests/core/test_camera_calibration.cpp` | ABSENT |
| CMake target (`camera_bridge`, any `v4l2` reference) | ABSENT |

The release's camera-tagged C++ tests are simulator and D-02 tests only
(`[camera][config][sim]`, `[camera][framerate][sim]`, `[camera][e2e][failure]`, `[camera][d02]`) —
the physical suite is not among them. `docs/SUBMISSION_PHYSICAL_SENSOR_INPUTS.md` is present and
byte-identical to the copy on the isolated branch, so its §5 "Evidence paths" table is, in this
branch, a list of dangling references.

Two honest resolutions; either is fine, the current state is not:

- **Option A — include the artifacts.** Add from `experiment/hg-v380-09-secure-camera-integration`
  (all committed): the 7 code/test files listed above, the CMake wiring for `hg_camera_bridge`, the
  four documentation/evidence files, and the `*.log` evidence file (it needs `git add -f`: the
  `.gitignore` `*.log` rule silently excludes it — that is how it was nearly lost once already).
  Then the claim at `SUBMISSION_VIDEO_SCRIPT.md:4` is true as written and the segment in §4 runs on
  the release branch.
- **Option B — remove the claim.** Delete the physical-sensor sentence (redline R9), drop
  `docs/SUBMISSION_PHYSICAL_SENSOR_INPUTS.md` from this PR, and add to the Devpost disclosures:
  "No physical USB-camera capture is part of the judge build; that work lives on an unmerged
  experimental branch and is not relied on by any claim in this submission."

Option A is the stronger submission — the evidence exists, is reproducible, and is genuinely physical —
but it is the submission lead's call, and it must not delay the P0 MCP gate.

---

## 4. Reproducible 20-second camera-evidence segment (existing artifacts only)

**No capture, no device access, no filming.** Every command below was executed today and prints the
output shown. Prerequisite (once): check out the isolated branch and build —

```bash
git fetch origin experiment/hg-v380-09-secure-camera-integration
git checkout FETCH_HEAD                     # or: git checkout -b camera-evidence FETCH_HEAD
mkdir -p hg_build && cd hg_build && cmake .. && make -j4 homeguardian_tests hg_camera_bridge
```

| # | Time | On screen (type or paste) | What the audience reads |
|---|---|---|---|
| 1 | 0:00–0:05 | `head -12 docs/evidence/PHYSICAL_CAMERA_STILL_SCENE.log` | `Device: /dev/video0 (uvcvideo, V4L2, YUYV 640x480 …)` and `capture transcript -> …` — a committed raw artifact, from a real UVC device |
| 2 | 0:05–0:10 | `grep -E 'MOTION-VERDICT\|sustained_increase=\|detection-fusion rule' docs/evidence/PHYSICAL_CAMERA_STILL_SCENE.log` | `sustained_increase=0` · `MOTION-VERDICT: NO sustained increase observed` · `detection-fusion rule: no alert` — the no-false-positive side |
| 3 | 0:10–0:15 | `sed -n '/^## 3\. Consented capture/,/RESULT:/p' docs/evidence/PHYSICAL_CAMERA_CAPTURE_LOG.md \| grep -E 'frames captured\|peak motion\|peak confidence\|windows by kind\|MOTION-VERDICT'` | `600 of 600` · `0.1252 / 2.70` · `0.849` · `sustained_increase=6` · `a sustained increase WAS observed` |
| 4 | 0:15–0:20 | `HG_SOURCE_DIR=$PWD ./hg_build/homeguardian_tests '[camera]'` | `All tests passed (217 assertions in 22 test cases)` — the closed vocabulary, bounds and gate asserted |

Exact outputs verified for all four shots (see §5). Captions must stay inside the vocabulary: "a
sustained increase in motion in view", "still scene refused", "bounded scalars, no frames retained".
No person, animal, species or alert claim may appear on screen — the tool has no such model, and the
camera-triggered-alert case is unmeasured.

**If the lead chooses Option B** (no camera artifacts in the release), the 20-second segment that is
reproducible from PR #20 as it stands is: `./hg_build/homeguardian_tests` (527 cases / 38,929
assertions) → `bash scripts/verify_mcp.sh` (73) → `node web_mcp/test_devices_dashboard.js` (161, and
the on-screen refusal `{"error":"physical camera unverified"}` / HTTP 400) →
`bash scripts/test_hg_sec_01_independent.sh` (15 gates). That segment shows the judge build's real
strengths and claims nothing about a camera.

---

## 5. Verification log (all run on the PR head, in a clean clone)

| Command | Printed result |
|---|---|
| `HG_SOURCE_DIR=$PWD ./b/homeguardian_tests` | `All tests passed (38929 assertions in 527 test cases)` |
| `bash scripts/verify_mcp.sh` | `== SUMMARY: 73 passed, 0 failed` |
| `node web_mcp/test_client.js` | `SUMMARY: 26 PASSED, 0 FAILED` |
| `node web_mcp/test_devices_dashboard.js` | `HG-UI-03 & RC2 RESULTS: 161 PASSED, 0 FAILED` |
| `node tests/test_outbound_mcp_bridge.js` | `SUMMARY: 46 PASSED, 0 FAILED` |
| `node tests/test_java_ws_bridge_cxx_e2e.js` | `End-to-End Test Results: 21 PASSED, 0 FAILED` |
| `bash scripts/test_hg_sec_01_independent.sh` | `SECURITY GATE RESULTS: 15 PASSED, 0 FAILED` |
| `HG_BRIDGE_PATH=build/hg_alexa_bridge bash scripts/verify_retention.sh` | `retention regression: 8 passed, 0 failed` |
| `bash scripts/verify_deploy_guards.sh` | `deployment guards: 12 passed, 0 failed, 0 skipped` |
| `cd alexa_skill && npm install && npm test` | `tests 77 · pass 77 · fail 0` |
| `sha256sum LICENSE` vs `/usr/share/common-licenses/Apache-2.0` | identical: `cfc7749b96f63bd31c3c42b5c471bf756814053e847c10f3eb003417bc523d30` |
| `HG_SOURCE_DIR=$PWD ./build/homeguardian_tests '[camera]'` (isolated branch) | `All tests passed (217 assertions in 22 test cases)` |

Environment: Linux, g++ 13, CMake + vendored Catch2/SQLite, CMake FetchContent sources supplied
locally for spdlog/nlohmann, Node.js v26, `npm ci` at the repo root (43 packages) and `npm install`
in `alexa_skill` (3 packages).

---

*No threshold was changed, no test was modified, no camera was activated, no microphone was opened,
and no branch was merged. The release branch was read only.*
