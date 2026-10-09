# HomeGuardian AI — Local Skill Inventory

Skills discovered in the local Hermes installation at `~/.hermes/skills/`.
Only skills relevant to HomeGuardian are listed. Each entry includes the
actual file path, key guidance, intended use, and limitations.

---

## C++ Development

### `cpp/cpp-acquisition-security-contract`
- **Path:** `~/.hermes/skills/cpp/cpp-acquisition-security-contract/SKILL.md`
- **Guidance:** C++ adapter security contract review; install libcurl after review.
- **Use:** Security review of C++ network adapters (Tier 2 HTTP client).
- **Limitations:** Focused on acquisition/transport security, not full architecture.

### `cpp/cpp-transport-implementation`
- **Path:** `~/.hermes/skills/devops/cpp-transport-implementation/SKILL.md`
- **Guidance:** C++ transport with security controls and SHA-256.
- **Use:** Secure HTTP transport for Tier 2 server and Tier 3 hybrid forwarding.
- **Limitations:** Transport layer only; no application logic.

### `cpp-service-foundation-build-gate`
- **Path:** `~/.hermes/skills/software-development/cpp-service-foundation-build-gate/SKILL.md`
- **Guidance:** Build gate for C++ service foundation.
- **Use:** Phase B build verification and acceptance criteria.
- **Limitations:** Generic gate; needs HomeGuardian-specific checks.

### `cpp-service-foundation-security-contract-review`
- **Path:** `~/.hermes/skills/cpp-service-foundation-security-contract-review/SKILL.md`
- **Guidance:** Review adapter gaps; fix; mark PASS after gates.
- **Use:** Security contract review for C++ service adapters.
- **Limitations:** Review framework only; no implementation.

### `offline-first-credential-restricted-service`
- **Path:** `~/.hermes/skills/software-development/offline-first-credential-restricted-service/SKILL.md`
- **Guidance:** Scaffolding a no-credentials C++ data service.
- **Use:** Phase B/C++ core design — local-first, no cloud credentials.
- **Limitations:** Scaffolding guide; not a full architecture.

### `deterministic-analysis-components`
- **Path:** `~/.hermes/skills/software-development/deterministic-analysis-components/SKILL.md`
- **Guidance:** Building pure analysis components with replay.
- **Use:** Tier 2 inference pipeline — deterministic, testable analysis.
- **Limitations:** Analysis components only; no I/O or networking.

---

## Testing and Code Quality

### `test-driven-development`
- **Path:** `~/.hermes/skills/software-development/test-driven-development/SKILL.md`
- **Guidance:** TDD: enforce RED-GREEN-REFACTOR, tests before code.
- **Use:** Phase B and beyond — all modules developed with TDD.
- **Limitations:** Methodology only; no HomeGuardian-specific tests.

### `systematic-debugging`
- **Path:** `~/.hermes/skills/software-development/systematic-debugging/SKILL.md`
- **Guidance:** 4-phase root cause debugging: understand bugs before fixing.
- **Use:** Debugging pipeline and inference failures.
- **Limitations:** Debugging process only; no prevention.

### `requesting-code-review`
- **Path:** `~/.hermes/skills/software-development/requesting-code-review/SKILL.md`
- **Guidance:** Pre-commit review: security scan, quality gates, auto-fix.
- **Use:** Hermes code review before every commit.
- **Limitations:** Review checklist only; no automated enforcement.

### `verification-code-integrity`
- **Path:** `~/.hermes/skills/software-development/verification-code-integrity/SKILL.md`
- **Guidance:** Audit verification checks for warnings that skip the result.
- **Use:** Ensuring tests actually verify behavior, not just pass.
- **Limitations:** Audit tool only; no fix guidance.

### `deliverable-review`
- **Path:** `~/.hermes/skills/software-development/deliverable-review/SKILL.md`
- **Guidance:** Independently verify deliverable documents; no fabricated citations.
- **Use:** Documentation verification and evidence quality.
- **Limitations:** Document review only; no code review.

---

## Android and Hardware

### `android-mtk-clone-phones`
- **Path:** `~/.hermes/skills/hardware/android-mtk-clone-phones/SKILL.md`
- **Guidance:** Android phone via adb, mtkclient, Bluetooth, WiFi.
- **Use:** Tier 1 Android acquisition client development and debugging.
- **Limitations:** Focused on MTK clone phones; not general Android NDK.

### `android-gradle-build-troubleshooting`
- **Path:** `~/.hermes/skills/software-development/android-gradle-build-troubleshooting/SKILL.md`
- **Guidance:** Android Gradle build failures and missing tools.
- **Use:** Tier 1 Android build issues.
- **Limitations:** Gradle-specific; not NDK C++ build.

### `android-mtk-flash-root`
- **Path:** `~/.hermes/skills/software-development/android-mtk-flash-root/SKILL.md`
- **Guidance:** Rooting or flashing MTK Android phone over USB.
- **Use:** Tier 1 device preparation (if root needed for sensors).
- **Limitations:** Flashing/root only; not app development.

---

## Computer Vision and Face Analysis

### `face-reading`
- **Path:** `~/.hermes/skills/face-reading/SKILL.md`
- **Guidance:** Face detection, recognition, age, emotion, physiognomy, posture. Combines scientific (FER) and cultural (physiognomy) practices.
- **Use:** Tier 2 face analysis module — detection, recognition, emotion estimation.
- **Limitations:** Physiognomy is cultural, not scientific. Emotion detection 70-85% accuracy. Not for medical diagnosis.

### `facial-expression-recognition`
- **Path:** `~/.hermes/skills/facial-expression-recognition/SKILL.md`
- **Guidance:** FER: 7 basic emotions, valence-arousal, micro-expressions, Action Units (FACS). Models: AU-ViT, LibreFace 2.0, DeepFace 3.0, MAE-DFER.
- **Use:** Tier 2 FER pipeline — AU-based emotion classification.
- **Limitations:** Accuracy varies with lighting/pose/occlusion. Best with frontal face, good lighting.

### `lip-reading`
- **Path:** `~/.hermes/skills/lip-reading/SKILL.md`
- **Guidance:** Lip reading via face mesh mouth ROI and sequence models.
- **Use:** Tier 2 lip reading for silent speech recognition.
- **Limitations:** Requires clear mouth visibility; sequence models need temporal context.

### `remote-vital-signs`
- **Path:** `~/.hermes/skills/remote-vital-signs/SKILL.md`
- **Guidance:** Measure HR, HRV, SpO2 from face via camera (rPPG).
- **Use:** Tier 2 rPPG pipeline — heart rate from face video.
- **Limitations:** HRV and SpO2 experimental on phones. Requires still subject, good lighting.

### `thermal-emotion-detection`
- **Path:** `~/.hermes/skills/thermal-emotion-detection/SKILL.md`
- **Guidance:** Detect stress/emotion from facial thermal patterns.
- **Use:** Tier 2 thermal analysis (requires thermal camera hardware).
- **Limitations:** Requires FLIR One or similar thermal camera. RGB camera cannot measure temperature.

---

## Audio and Voice Analysis

### `voice-analysis`
- **Path:** `~/.hermes/skills/voice-analysis/SKILL.md`
- **Guidance:** Voice emotion, health, stress, deception cues. Tools: openSMILE, Wav2Vec2, DigiVoice. Datasets: TESS, SAVEE, CREMA-D, IEMOCAP, RAVDESS.
- **Use:** Tier 2 voice analysis — emotion, health biomarkers, stress.
- **Limitations:** Deception detection unreliable (50-60%). Health screening not clinical-grade. Emotion 60-75% natural accuracy.

### `animal-language-recognition`
- **Path:** `~/.hermes/skills/multimodal-affective-computing/animal-language-recognition/SKILL.md`
- **Guidance:** Classify animal sounds by species, emotion, traits.
- **Use:** Tier 2 optional animal sound classification.
- **Limitations:** Research-grade; not production-ready.

### `sign-language-recognition`
- **Path:** `~/.hermes/skills/sign-language-recognition/SKILL.md`
- **Guidance:** Sign language recognition via MediaPipe and graph models.
- **Use:** Tier 2 accessibility feature (optional).
- **Limitations:** Requires specific models; not core HomeGuardian functionality.

---

## Posture and Behavior

### `body-posture-recognition`
- **Path:** `~/.hermes/skills/body-posture-recognition/SKILL.md`
- **Guidance:** Detect emotions from body posture and gestures via camera.
- **Use:** Tier 2 posture analysis for fall detection and activity recognition.
- **Limitations:** Requires full-body view; accuracy limited by camera angle.

### `behavior-change-detection`
- **Path:** `~/.hermes/skills/behavior-change-detection/SKILL.md`
- **Guidance:** Detect routine changes and behavioral anomalies via sensors.
- **Use:** Tier 2 behavior anomaly detection — baseline learning and deviation alerts.
- **Limitations:** Requires 1-2 weeks baseline data; false positives during routine changes.

### `intent-recognition`
- **Path:** `~/.hermes/skills/intent-recognition/SKILL.md`
- **Guidance:** Detect intent and motive from multimodal signals.
- **Use:** Tier 2 intent inference (hypothesis engine).
- **Limitations:** Motive detection is suggestive, never definitive. Ethical guardrails required.

---

## Multimodal Fusion

### `multimodal-affective-computing`
- **Path:** `~/.hermes/skills/multimodal-affective-computing/SKILL.md`
- **Guidance:** Detect emotions and behavior from face, body, voice, and vital signs. Master guide for multimodal fusion.
- **Use:** Tier 2/3 fusion engine — combining face, voice, posture, vital signs.
- **Limitations:** Fusion accuracy depends on individual modality accuracy. Missing data handling required.

---

## ML Inference

### `mlops/inference/llama-cpp`
- **Path:** `~/.hermes/skills/mlops/inference/llama-cpp/SKILL.md`
- **Guidance:** llama.cpp local GGUF inference + HF Hub model discovery.
- **Use:** Optional local LLM inference for natural language understanding.
- **Limitations:** GGUF models only; not ONNX. Resource-intensive.

### `mlops/inference/serving-llms-vllm`
- **Path:** `~/.hermes/skills/mlops/inference/serving-llms-vllm/SKILL.md`
- **Guidance:** vLLM high-throughput LLM serving, OpenAI API, quantization.
- **Use:** Optional high-throughput LLM serving (if needed for concierge).
- **Limitations:** Requires GPU; overkill for Phase B.

### `mlops/huggingface-hub`
- **Path:** `~/.hermes/skills/mlops/huggingface-hub/SKILL.md`
- **Guidance:** HuggingFace hf CLI: search/download/upload models, datasets.
- **Use:** Model discovery and download for ONNX inference.
- **Limitations:** Download only; no inference.

### `mlops/evaluation/evaluating-llms-harness`
- **Path:** `~/.hermes/skills/mlops/evaluation/evaluating-llms-harness/SKILL.md`
- **Guidance:** lm-eval-harness: benchmark LLMs (MMLU, GSM8K, etc.).
- **Use:** Optional LLM evaluation (if concierge uses local LLM).
- **Limitations:** LLM-specific; not for vision/audio models.

---

## Resource Management

### `hermes-resource-utilization`
- **Path:** `~/.hermes/skills/hermes-resource-utilization/SKILL.md`
- **Guidance:** CPU/RAM constraints. Check load before parallel work.
- **Use:** Phase B resource monitoring — verify 30% CPU budget compliance.
- **Limitations:** Monitoring only; no enforcement.

### `workstation-resource-safety`
- **Path:** `~/.hermes/skills/workstation-resource-safety/SKILL.md`
- **Guidance:** Heavy builds or tests may freeze the desktop.
- **Use:** Phase B build safety — bounded parallelism, memory monitoring.
- **Limitations:** Safety guidelines only; no automated limits.

### `safe-local-process-and-io-management`
- **Path:** `~/.hermes/skills/sysadmin/safe-local-process-and-io-management/SKILL.md`
- **Guidance:** Starting heavy local work or stopping a process.
- **Use:** Phase B process management — build and test process control.
- **Limitations:** Process management only; no resource quotas.

---

## Security and Privacy

### `ethical-hacking`
- **Path:** `~/.hermes/skills/security/ethical-hacking/SKILL.md`
- **Guidance:** Ethical hacking/security testing — legal & defensive only.
- **Use:** Security review of HomeGuardian server and API.
- **Limitations:** Testing only; no secure coding guidelines.

### `env-secrets-deployment`
- **Path:** `~/.hermes/skills/devops/env-secrets-deployment/SKILL.md`
- **Guidance:** .env must leave git but stay on deploy hosts.
- **Use:** Phase B configuration secrets management.
- **Limitations:** Deployment-focused; not application-level security.

---

## Networking

### `x402-paid-endpoints`
- **Path:** `~/.hermes/skills/blockchain/x402-paid-endpoints/SKILL.md`
- **Guidance:** Building or verifying an x402 paid HTTP endpoint.
- **Use:** Not directly applicable to HomeGuardian (no payment processing).
- **Limitations:** Not relevant to HomeGuardian use case.

### `cloudflare-tunnel`
- **Path:** `~/.hermes/skills/devops/cloudflare-tunnel/SKILL.md`
- **Guidance:** Cloudflare Tunnel for HTTPS callbacks to localhost.
- **Use:** Optional remote access to HomeGuardian dashboard (if needed).
- **Limitations:** Cloud dependency; contradicts local-first principle.

### `blocked-page-recovery`
- **Path:** `~/.hermes/skills/web/blocked-page-recovery/SKILL.md`
- **Guidance:** Fetch fails: 403/429, paywall, WAF, bot wall.
- **Use:** Not directly applicable (HomeGuardian is local, not web scraping).
- **Limitations:** Not relevant to HomeGuardian.

---

## Communication

### `communication-style`
- **Path:** `~/.hermes/skills/software-development/communication-style/SKILL.md`
- **Guidance:** User wants English, concise, no filler — answer first.
- **Use:** All HomeGuardian documentation and reports.
- **Limitations:** Style guide only.

---

## Skills NOT Found (Gaps)

The following topics have **no dedicated skill** in the local installation:

| Topic | Impact | Mitigation |
|-------|--------|------------|
| Android NDK C++ development | Tier 1 acquisition client | Use official Android NDK docs; `android-mtk-clone-phones` for device access |
| SensorManager / sensor acquisition | Tier 1 sensor reading | Use Android NDK `ASensorManager` docs |
| Camera NDK / camera lifecycle | Tier 1 camera capture | Use Android NDK Camera2 API docs |
| Audio capture / VAD | Tier 1 audio | Use Android NDK AAudio docs |
| OpenCV C++ | Tier 2 vision processing | Use OpenCV official docs; no local skill |
| ONNX Runtime C++ | Tier 2 ML inference | Use ONNX Runtime official docs; no local skill |
| Model quantization | Tier 2 model optimization | Use ONNX Runtime / OpenVINO docs |
| Pose estimation (MediaPipe/OpenPose) | Tier 2 posture | Use MediaPipe C++ docs; no local skill |
| Audio feature extraction (openSMILE) | Tier 2 voice analysis | Use openSMILE docs; no local skill |
| Time-series anomaly detection | Tier 2 behavior | Implement custom; `behavior-change-detection` for concepts |
| HTTP server (Boost.Beast/Drogon) | Tier 2 server | Use library docs; no local skill |
| WebSockets | Tier 2/3 streaming | Use library docs; no local skill |
| SQLite C++ | Tier 2 persistence | Use SQLite C API docs; no local skill |
| spdlog / structured logging | Phase B logging | Use spdlog docs; no local skill |
| nlohmann/json | Phase B config | Use nlohmann/json docs; no local skill |
| Catch2 / unit testing | Phase B testing | Use Catch2 docs; no local skill |
| MCP Streamable HTTP | Phase F Alexa+ integration | Use MCP specification docs; no local skill |
| Alexa+ integration | Phase F voice interface | Use Alexa+ docs; no local skill |
| Ring API integration | Phase G doorbell events | Use Ring API docs; no local skill |
| Consent / privacy engineering | Core design | Implement from first principles; no local skill |
| CMake advanced patterns | Phase B build | Use CMake docs; `cpp-service-foundation-build-gate` for gate concepts |

---

## Summary

- **Total skills in local installation:** 208
- **Relevant to HomeGuardian:** 24
- **Gaps (no skill):** 20 topics

The local skills provide strong guidance for:
- C++ development patterns and security
- TDD and code review processes
- Computer vision and face analysis concepts
- Voice analysis and emotion detection
- Multimodal fusion concepts
- Resource management and build safety

The gaps are primarily in:
- Android NDK specific development
- Specific library usage (OpenCV, ONNX Runtime, Boost.Beast, etc.)
- MCP and Alexa+ integration
- Privacy/consent engineering

For gaps, rely on official library documentation and implement from first principles.
