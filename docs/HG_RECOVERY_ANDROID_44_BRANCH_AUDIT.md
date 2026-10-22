# Legacy/parallel branch recovery audit — Android lane

**HG-RECOVERY-ANDROID-44.** Bounded audit, docs only. No install, camera/mic, fastboot, credential,
Oracle, runtime restart or main push. **Nothing was changed outside this lane.**

Reference point: `main` @ **`79e21c216779fe3f9e3c3d3c33df2bc6d27e5a44`**

## 0. Method, and the trap the directive named

"Ahead" is measured as commits on the branch that main lacks (`git rev-list --left-right --count`).
**Ahead ≠ missing functionality**, and this audit found the case that proves it: `hg-cam-08d-evidence` is
2 commits ahead, yet **every Android file it "adds" is already in main**. Counting commits would have
produced a false "recover this" verdict. So each branch was checked by *content*, not by count.

Conversely, a branch with **zero** unique commits is a strict **ancestor** of main: its entire tree is
already in main and there is nothing to recover from it at all.

## 1. Divergence

| branch | ahead | behind | head | unique content |
|---|---|---|---|---|
| `experiment/mi-a3-edge-sec02` | **0** | 211 | `fff599f` | **none — ancestor of main** |
| `experiment/mi-a3-edge-service` | **0** | 179 | `c8a8a78` | **none — ancestor of main** |
| `experiment/mi-a3-edge-service-audit` | **0** | 177 | `2001585` | **none — ancestor of main** |
| `feature/h03-android-wifi-node` | **0** | 223 | `cad758f` | **none — ancestor of main** |
| `experiment/mi-a3-edge` | 2 | 214 | `cfb282a` | Mi A3 edge config/scripts/logs |
| `experiment/mi-a3-linux-runtime` | 33 | 179 | `e3f59c8` | native-Linux boot artifacts, fastboot probes |
| `experiment/mi-a3-linux-runtime-hashfix` | 33 | 179 | `a301ca6` | same + SHA256SUMS regeneration |
| `experiment/mi-a3-kaggle-remote` | 1 | 266 | `953ea3a` | Kaggle notebook + committed PNG outputs |
| `experiment/hg-cam-08d-evidence` | 2 | 223 | `23076b0` | **duplicate** Android files + docs + 7.7 KB log |
| `experiment/hg-panasonic-service-transport-06` | **21** | 195 | `bf776d0` | transport fixes, test suite, E2E runner, docs |

## 2. Per-feature recovery matrix

`ACCEPT` = safe to take now · `DEFER` = genuinely valuable but post-submission · `REJECT` = do not take.

| # | Feature | Source SHA / path | Equivalent in main | Tests / provenance | Portability · licensing · secrets | Deps | Verdict | Smallest safe recipe |
|---|---|---|---|---|---|---|---|---|
| 1 | Android serializer/mTLS/reconnect **fixes** | `bf776d0` `apk/src/.../ObservationTransport.java` (+360) | main has the file but **not** the rebase/counter/bounds fixes | 34 TLS + 59 cross-receiver + 19 live E2E | portable (pure Java); no secrets | android.jar only | **DEFER** | `git checkout -p bf776d0 -- apk/src/org/homeguardian/harness/ObservationTransport.java` then re-run the isolated suite |
| 2 | Detector producer (motion + presence) | `bf776d0` `apk/tests/DetectorProducerFixtureTest.java` | none | 9/9, drives a real committed alert | portable; no secrets | `scripts/run_real_ingress_e2e.sh` | **DEFER** | cherry-pick the test + its runner; needs the 2-identity registry |
| 3 | Real-ingress E2E runner | `bf776d0` `scripts/run_real_ingress_e2e.sh` | none | 19/19, 19/19, 19/19 | portable; test-only throwaway creds in scratch | docker image `homeguardian-ai:d032test` | **DEFER** | add the script + `apk/tests/RealIngressE2ETest.java` together |
| 4 | Wire vectors (pinned + live) | `bf776d0` `apk/tests/SyntheticVectorEmitTest.java` | none | 19/19 each mode | portable; no secrets | — | **DEFER** | single test file, self-contained |
| 5 | Test stubs (`org.json`, `android.util.Log`) | `bf776d0` `apk/tests/stubs/**` | main has `Log`/`Context`/`PowerManager` only | exercised by the whole suite | portable; test-only | — | **DEFER** | needed by #1–#4; take together |
| 6 | Pairing preflight + runbook, capture checklist, claims matrix | `bf776d0` `docs/HG_PAIRING_PREFLIGHT_AND_RUNBOOK.md`, `HG_TWO_DEVICE_CAPTURE_CHECKLIST.md`, `HG_ANDROID_PROVENANCE_CLAIMS_MATRIX.md` | none | documentation of verified behaviour | no secrets (checked) | none | **DEFER** | plain `git checkout bf776d0 -- <doc>` |
| 7 | Mi A3 edge engine config/scripts/logs | `cfb282a` `config/mi_a3_edge*.json`, `scripts/mi_a3_edge_*.sh`, `logs/mi_a3_*` | no | device measurements, not reproducible here | device-specific; LAN addresses in configs | Mi A3 handset | **REJECT** | — |
| 8 | Native Linux on Mi A3 (boot/kernel/initramfs) | `e3f59c8` / `a301ca6` `native-linux/**` | no | abandoned by its own decision record `e3f59c8` | **stale binaries: 15.7 MB `Image.gz-dtb`, 4.3 MB `initramfs.cpio.gz`, 2.7 MB `hg_mcp_server`** | fastboot, bootloader | **REJECT** | prohibited/unsafe bootloader path; superseded |
| 9 | Fastboot probes / bootloader diagnostics | `e3f59c8` `scripts/**fastboot**`, docs | no | read-only probes | no lasting value if the lane is abandoned | USB/fastboot | **REJECT** | — |
| 10 | Kaggle remote workspace | `953ea3a` `deploy/kaggle/**` | no | 1 notebook + committed results | **non-portable; committed PNG outputs ~500 KB each**; ties to a Kaggle account | Kaggle quota | **REJECT** | benchmark outputs are not an artifact to ship |
| 11 | Panasonic camera evidence (docs + log) | `23076b0` `docs/HG_CAM_08D_*.md`, `docs/evidence/*.log` (7.7 KB) | **Android code already in main** | the only unique part is prose + a small log | no secrets | none | **REJECT as recovery** | the "code" is a duplicate; docs can be re-read from the branch if ever needed |
| 12 | Four 0-ahead branches | `fff599f`, `c8a8a78`, `2001585`, `cad758f` | **fully contained in main** | — | — | — | **REJECT** | nothing to recover; ancestors of main |

## 3. Secrets / licensing scan

Scanned every branch tree for private keys, `password=`, `api_key`, `secret=`, `Bearer <long>`:

```
experiment/mi-a3-edge                    matches: oracle_preflight_readonly.sh, verify_synthetic_mcp_demo.sh,
                                         NativeHttp.h, hg_alexa_bridge.cpp, hg_demo_server.js
experiment/mi-a3-linux-runtime           same set + logs/device-artifacts/*.xml
experiment/mi-a3-kaggle-remote           clean
experiment/hg-cam-08d-evidence           same set
experiment/hg-panasonic-service-transport-06  same set
```

**All of those files are already in main** (`git cat-file -e 79e21c2:<path>` — all present), so they are
**not branch additions**. The match itself is a **redaction rule**, not a credential:

```
scripts/oracle_preflight_readonly.sh:32:
  -e 's/((token|secret|password|passwd|key|apikey|api_key|credential|auth)[A-Za-z0-9_-]*[[:space:]]*[:=][[:space:]]*)[^[:space:]]+/\1<redacted>/Ig'
```

That is a `sed` expression whose job is to redact secrets. **No credential was found in any branch
addition.** Licensing: every candidate in the DEFER set is first-party code under the repo's existing
licence (Apache-2.0 per the repo); no third-party vendoring is introduced by any ACCEPT/DEFER item.

## 4. Cross-check against the judge release

Verified independently for the previous task: `docs/DEVPOST_DEMONSTRATION_GUIDE.md` @ `366f4a95` lists
host prerequisites `curl, jq, cmake, make, g++, node` — **no APK, no ADB, no handset, no Android SDK** —
and runs loopback-only. **The judge release needs no Android.** Therefore:

- **Nothing above is blocking for the hackathon.** Every useful item is `DEFER`.
- The `REJECT`s are not merely unnecessary, several are actively unsuitable to ship: multi-megabyte
  boot binaries, a prohibited bootloader path, and committed Kaggle benchmark images.

## 5. Post-submission ranking

1. **`ObservationTransport` fixes (#1)** — real defects with regression tests (permanently-refused
   observation counted as delivered; restarted node losing its first observation). Highest value.
2. **Detector producer (#2)** — the only fixture that drives a *detector-capable* alert end to end.
3. **Real-ingress E2E runner (#3)** — the harness that makes #1 and #2 checkable rather than asserted.
4. **Test stubs + vectors (#4, #5)** — small, and #1–#3 depend on them.
5. **Docs (#6)** — cheap, and they are the honest record of what was and was not proven.
6. Everything else: **do not recover.** Revisit only if a physical-hardware lane is ever re-authorised.

## 6. Bounded-audit statement

Read-only throughout. No branch was modified, no cherry-pick was performed, nothing was pushed to
`main`, no device or credential was touched. Only this document was added, in this lane.
