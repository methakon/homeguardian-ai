# Alert interface for the R2 authorized action adapter

**For:** the R2 action-orchestration work. **Nothing in its worktree was touched and no
action-orchestration change is made here** — this is the interface, from the alert producer's side.

## The exact fields an alert carries

`Alert` (`src/core/Alert.h`), as serialized by `Alert::to_json()`:

| field | type | notes |
|---|---|---|
| `alert_id` | string | **never empty** (the constructor throws), stable for the alert's lifetime, and what `alert_ids` in the MCP summary carries |
| `severity` | enum | `info` < `warning` < `critical`; derived by the rule, never set by a producer |
| `message` | string | bounded, human-readable, no payloads or credentials |
| `contributing_event_ids` | array of strings | **the stable event ids** that produced the alert, in the order they contributed — use these for provenance, not timestamps |
| `detected_at` | integer (epoch ms) | processing time: when the hub decided, not when the sensor observed |
| `confidence` | number [0,1] | the fused confidence, validated on construction |
| `rule_name` | string | `detection-fusion` for the sensor path |
| `details` | object | the rule's own evidence summary |

## Duplicate alert semantics (what the adapter must expect)

`AlertPolicy` deduplicates with the key **`rule_name` (+ severity)** and a default **60 s cooldown**,
re-arming after it elapses; retained dedup state is capped at 256 keys. Practical consequences:

- One sustained transition usually yields **one** delivered alert, but a transition with a distinct rise can
  yield **more than one** (measured: two, ~20 ms apart, same rule name). **Do not treat alert count as an
  event count.**
- The same alert id is never re-delivered, but the *same condition* can produce a *new* alert after the
  cooldown — deduplicate on `(rule_name, condition window)`, not on alert_id alone, if the adapter must act
  once per incident.
- Alerts that survive the policy are the only ones persisted and the only ones the MCP summary reports.

## Revocation behaviour

- A source revoked in the registry is refused **per observation**, so an adapter can see a source go from
  contributing to refused between two alerts.
- Consent withdrawal is refused as `consent_withdrawn` and stops the stream; the engine does not emit a
  "condition ended" alert, so an adapter should treat the absence of further evidence as *unknown*, never as
  *resolved*.
- A disconnected source yields exactly one `device_availability` / `"offline"` transition and **zero** sensed
  observations. An adapter must not read silence as "nothing happening".

## What the adapter must not assume

- That an alert exists for every observation (it does not: the fused confidence gate is real, and a single
  detector class cannot clear it).
- That severity escalates with the number of observations (it does not: it follows the rule's bands).
- That the hub can be asked to act by a producer (it cannot: the MCP tools are read-only, and no producer
  path can enable capture, grant consent or trigger an action).
