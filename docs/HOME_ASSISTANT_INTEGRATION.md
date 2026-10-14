# Home Assistant Integration (S-05)

**Requested skill:** `Home Assistant hub` - **DOES NOT EXIST** in this environment. No skill,
plugin or catalog entry carries that identifier.
**Inspected instead:** the installed Hermes plugin `homeassistant` (`~/.hermes/plugins/homeassistant/`,
`plugin.yaml` name `homeassistant`, kind `platform`, v2.0.1, MIT, NousResearch, catalog sha
`ba30cb0cf86c52bdb5cde98974bc062e97966529`), plus the `home-security-sensor-nodes` skill's
Home Assistant tooling section.
**Milestone:** D-02x, branch `feature/skills-detection-integration`.

---

## 1. What the available material actually supports

| Mechanism | Available? | Decision |
|---|---|---|
| REST API (`GET /api/states/<entity_id>`) | Yes - the plugin's tools use it | **ADOPTED** - implemented natively in C++ |
| Entity state changes | Yes | **ADOPTED** as observations, via polling |
| Motion / contact / occupancy / availability entities | Yes | **ADOPTED** with an explicit device_class mapping |
| Environmental sensors (temperature, humidity, pressure) | Yes | **ADOPTED** as environmental readings (no threshold is invented) |
| Camera entities | Yes | **ADOPTED** as availability only - the adapter never pulls a stream |
| WebSocket event subscription | Yes in the plugin (Python, `aiohttp`) | **DEFERRED** - requires a framed client; REST polling satisfies the milestone with a far smaller surface |
| MQTT | Not established as available here | **DEFERRED** - no messaging dependency added |
| Local automations | n/a (they run in Home Assistant, not here) | **NOT CONSUMED** - out of scope; HomeGuardian does not duplicate automation |
| Service calls / actuators (locks, alarms, sirens, cameras) | Yes in the plugin | **REJECTED** for now - read-only until separately approved |

The Python plugin itself is **not a dependency**: it is an agent-side gateway (agent tools +
notification delivery), whereas HomeGuardian is a C++20 hub in a container with no Python at
runtime. It was read to establish what the API supports.

## 2. Adopted implementation

`src/core/HomeAssistantAdapter.{h,cpp}` + `src/core/HttpClient.{h,cpp}` + the new bridge route
`/api/integrations`.

| Required behaviour | Where it lives | Test |
|---|---|---|
| 1. Configure the instance address | `HomeAssistantConfig::base_url` | `[ha][config]` |
| 2. Store credentials securely | `read_secret_file()`: regular file, <= 4096 B, **refuses group/other-readable**, trims trailing newline; the token is private to the transport | `[ha][secret]` |
| 3. Discover permitted entities | explicit `entity_allowlist`; nothing else is requested | `[ha][adapter]` (asserts every requested path) |
| 4. Allowlist what may be consumed | allowlist + entity-id shape validation; an unmapped entity is skipped with a reason | `[ha][mapping]` |
| 5. Convert authorized updates into validated observations | `map_entity()` + `SecurityObservation::validate()` + `to_observation()` | `[ha][mapping]`, `[ha][adapter]` |
| 6. Track online/offline | `DeviceAvailabilityTracker` fed on successful reads only | `[ha][adapter]`, `[gaps]` |
| 7. Reconnect without fabricating events | reconnect -> an availability observation (`first_seen=false`) and nothing else | `[ha][adapter]` |
| 8. Avoid duplicate ingestion | `SensorDeduplicator` + a deterministic observation id | `[ha][adapter]` |
| 9. Preserve real-vs-synthetic provenance | `Provenance::real` for entities; fixtures stay synthetic | `[ha][adapter]`, `[integration][skills]` |
| 10. Disabled by default until configured | `enabled=false`; refusal when disabled, misconfigured or unauthorized, with **zero requests issued** | `[ha][adapter]` (three sections), `[gaps]` |

### Security posture

- **Read-only.** The client has a GET path only; there is no POST/PUT/DELETE code to misuse, so
  the adapter cannot actuate anything.
- **No TLS in this build**, therefore an `https://` URL is **refused** with an explicit reason, and
  an `http://` endpoint additionally requires `allow_plaintext=true`. Silently downgrading while
  sending a bearer token would be worse than refusing to start.
- **The token cannot leak by construction**: it is read into the transport privately, never
  logged, never placed in an observation or feature payload, never returned by `describe()` (which
  reports only the host/port and "read-only"), never in an image layer. Refused entirely if the
  file is group- or other-readable.
- **Header injection is refused, not sanitised**: a CR/LF in any header name or value aborts the
  request.
- **Bounded I/O**: a 64 KiB response cap with socket timeouts; a truncated or oversized response is
  reported as such rather than buffered without limit.
- **No fabricated telemetry**: a failed or 401 read produces an error count and no observation, and
  does not mark the entity alive.

### Mapping rules (deliberately conservative)

`binary_sensor` needs a mapped `device_class` (motion/moving -> motion; occupancy/presence ->
presence; door/window/opening/garage_door -> contact; tamper -> tamper). An unlabelled or unmapped
`binary_sensor` is **skipped with a reason** rather than guessed - including `sound`, which has no
honest source type in this vocabulary yet. `camera.*` maps to camera availability only.
`sensor.*` maps to an environmental reading only for temperature/humidity/pressure with a numeric
state, and the state is recorded as `within` because **no threshold is invented**: a crossing must
be configured explicitly.

## 3. Rejected

| Rejected | Reason |
|---|---|
| The Python `homeassistant` plugin as a hub dependency | Wrong layer (agent gateway) and wrong runtime (Python); HomeGuardian's hub is C++20 with no Python |
| Actuator control (locks, alarms, sirens, cameras) | Explicitly out of scope until separate approval; a read-only client makes misuse impossible |
| Opening Home Assistant to the public internet | Never required: the adapter is an outbound LAN client, and the hub's own listener stays loopback-only |
| Inventing a threshold crossing | The adapter reports the reading; what counts as a crossing is configuration, not a guess |
| `https://` support without TLS | Refused rather than silently downgraded |

## 4. Tests and measured behaviour

| Suite | Command | Result |
|---|---|---|
| Config fail-closed matrix, secret-file handling, entity mapping, adapter contracts, HTTP client | `homeguardian_tests "[ha]"` | 6 cases / 179 assertions |
| Authentication failure, consent withdrawal, multi-device, bounds | `homeguardian_tests "[gaps]"` | 4 cases / 84 assertions |
| Dashboard/status surface | `bash scripts/verify_integration_dashboard.sh` | 31 API assertions + 5 shell checks, 0 failed |

The `[ha]` suite includes a **real loopback exchange**: a one-shot HTTP server bound to 127.0.0.1
on an ephemeral port inside the test process proves the request head, the status line, the body
read, the size cap and the truncation path against a genuine socket peer - no external service is
involved. Everything else is driven through the injected `IHomeAssistantTransport`, so the
contracts are deterministic and offline.

Observed behaviour worth stating precisely: a 401 yields `errors == 2` and **zero observations**;
an unchanged second poll is fully de-duplicated; three entities going quiet produce exactly three
offline transitions; a reconnect produces an availability observation with `first_seen == false`;
and the payload never contains `Bearer`, `token` or a secret.

## 5. Measured performance (resource)

The adapter is not in the hub's hot path: it makes no request unless enabled, configured and
authorized, and the status route performs no network I/O at all (it reports the configuration's own
disposition and an empty registry). The HTTP client allocates one bounded response buffer; there is
no thread, no background loop and no timer. Footprint figures for the targets that link it:
`docs/SKILL_VALIDATION_REPORT.md` section 10.

## 6. Missing pieces and remaining validation requirements

| Item | Status |
|---|---|
| A real Home Assistant instance | **Absent on this host.** The adapter has never spoken to a live instance; the socket path is proven against a loopback server instead |
| A long-lived token | Not present; the secret-file path is tested with a temporary 0600 fixture |
| WebSocket event subscription | Deferred (would replace polling once a real instance is available) |
| TLS | Not linked; required before any non-LAN use, and `https://` stays refused until then |
| Threshold configuration for environmental crossings | Not implemented - by design, nothing is invented |
| Actuator control | Requires separate explicit approval |

**Remaining validation requirement:** point the adapter at a real instance on a trusted LAN, set
`allow_plaintext=true` knowingly, authorize it, and confirm the mapped observations match the
entity states - then decide whether polling or a WebSocket subscription is the right long-term
shape. Until that happens the dashboard reports Home Assistant as **disabled** with the reason
taken from the adapter's own validation, and 0 requests issued.
