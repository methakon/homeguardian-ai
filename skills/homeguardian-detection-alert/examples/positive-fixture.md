# Bounded recipe: produce a fused alert, and prove the reader agrees

Prerequisites: the fixture script `scripts/verify_fusion_window.sh` @ `4bc964d` does all of this; this is the
recipe it follows, so you can port it rather than guess.

1. **Own scope.** Own loopback-only ports, own stores, own test CA, own containers. Verify the ports are free
   first and refuse to run if one is taken — a stale instance holding a port has produced a false result before.
2. **Two identities**, both registered: `paired`, `revoked:false`, `consent_granted`, `capture_enabled`.
   A revoked device belongs in the negative half, not here.
3. **Ingress config**: single writer; `evidence_detector_ids: ["sensor.motion","sensor.presence"]` (the ADR-014
   opt-in). No threshold is edited — not `min_usable`, not `min_confidence`, not the window, not the episode
   opt-in.
4. **Live timestamps**: `acquired_at_ms` = now. (A pinned 2025 goldens is a negative control: it must be refused.)
5. **Frames**: motion 12x `state:"on"`; presence 12x `state:"present"`; `confidence` present on every frame;
   `acquisition_mode`/`provenance_claim` both `"synthetic"`.
6. **One window**: send both streams back-to-back so the evidence shares a 5 s span (`Pipeline.h:15`).
7. **Assert the writer**: each frame ACKed `accepted` and correlated to its `observation_id`; then a row in the
   alert store — `rule_name=detection-fusion`, severity `warning`, `confidence=0.9` in the tested run.
8. **Assert the reader**: start the MCP server as a **separate process** against that store; its
   `get_alert_summary` must name **the same alert id** the writer committed. An alert only one process can see
   is not verified.
9. **The negative, in the same run**: availability-only (healthy) frames -> `alert_count=0`, which is the correct
   answer, not a failure. Print the unmet condition so the next reader does not "fix" it with a threshold.
10. **Audit trail**: frame -> ACK -> detector id/status/confidence -> verdict + confidence -> alert id -> MCP
    answer. Any missing hop means the claim is unverified.

Expected shape of the passing output is quoted in `SKILL.md` §9.
