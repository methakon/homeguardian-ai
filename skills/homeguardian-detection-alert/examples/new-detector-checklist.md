# New detector checklist

Copy this into the PR/issue text and fill it in. "N/A" needs a reason.

- [ ] **source allowlist** — `source_type` is a real `SecuritySourceType` value; `observation_type` is in
      `ObservationIngest.h:51-53` (or explicitly added by the operator, additive, defaults untouched).
- [ ] **detection state** — the states that mean "detected" are named in `security_status_for`
      (`SecurityObservation.cpp:75-110`). No default-to-detected. Near-miss states must land in
      `inconclusive`, and a unit test must assert that.
- [ ] **evidence id + opt-in** — the detector id (`sensor.<source_type>` for ingested evidence) is documented,
      and the operator opt-in (`evidence_detector_ids`) is stated. Adding an id must not change any gate.
- [ ] **registry** — registration, pairing, consent, capture-enabled; revocation exercised.
- [ ] **synthetic provenance** — fixtures declare synthetic acquisition and provenance.
- [ ] **privacy** — what is observed, retention, who can read the MCP projection, consent subject/purpose.
- [ ] **security review** — identity cross-check, replay/sequence, timestamp bounds, loopback-only binds.
- [ ] **tests** — unit; **mutation** (a near-miss input must NOT alert); real mTLS cross-process; MCP read-back
      equal to the writer's committed alert.
- [ ] **negative control** — the liveness/quiet case that must produce 0 alerts, asserted, not assumed.
- [ ] **no threshold drift** — diff shows no change to `min_usable`, `min_confidence`, agreement, the
      correlation window, or the episode opt-in.
