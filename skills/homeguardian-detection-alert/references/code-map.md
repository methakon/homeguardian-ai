# Reference map: skill claim -> real C++ file:line

All anchors verified in this checkout at skill version 1.0.0. `scripts/skill_lint.py` re-checks them.

| what | where |
|---|---|
| detector status from source_type + state | `src/core/SecurityObservation.cpp:75-110` |
| motion states (`on/detected/active`, `off/clear/idle`) | `src/core/SecurityObservation.cpp:78-80` |
| availability states (`offline/unavailable/error/unknown` are the detections) | `src/core/SecurityObservation.cpp:95-98` |
| tamper states (`unexpected_disconnect` is a detection) | `src/core/SecurityObservation.cpp:100-104` |
| threshold states (`crossed/above/below`) | `src/core/SecurityObservation.cpp:106-108` |
| authorization gates the status | `src/core/SecurityObservation.cpp:127` |
| ingested detector id `sensor.<source_type>` | `src/core/SecurityObservation.cpp:124` |
| usable-vote partition; "inconclusive is not a vote"; stale excluded | `src/core/FusionEngine.cpp:23-38` |
| `min_usable = 2`, `min_confidence = 0.5` | `src/core/DetectionFusionRule.h:36-37` |
| default evidence allowlist `{activity-energy}` | `src/core/DetectionFusionRule.h:50` |
| empty allowlist means allow-all | `src/core/DetectionFusionRule.h:79-80` |
| episode evidence opt-in (default off) | `src/core/DetectionFusionRule.h:55` |
| correlation window 5000 ms | `src/core/Pipeline.h:15`, applied `src/core/Pipeline.cpp:28` |
| default observation_type allowlist | `src/net/ObservationIngest.h:51-53` |
| wire framing (4-byte BE length, <= 4096) | `src/net/DeviceProtocol.h:29,49` |
| alert policy (cooldown) + single-writer persistence | `src/main_ingress.cpp:271,296-297,304-305` |
| read-only MCP alert summary | `hg_mcp_server` (`get_alert_summary`) |
| tested positive/negative fixture | `scripts/verify_fusion_window.sh` |
| replay + revocation negative controls | `scripts/verify_alert_causality.sh` |
