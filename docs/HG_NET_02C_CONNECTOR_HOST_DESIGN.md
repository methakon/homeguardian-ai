# HG-NET-02C — Site-connector host design and rehearsal evidence

**Date:** 2026-10-18
**Repository:** `methakon/homeguardian-ai`
**Branch:** `experiment/hg-v380-09-secure-camera-integration`
**Builds on:** `HG-NET-02C` site-connector framework, `HG-CAM-10` camera inventory and identity
evidence (see §12 of `HG_CAM_10_ADDITIONAL_CAMERA_ONBOARDING.md` for the prerequisite analysis this
document turns into a design)

**Isolation:** everything here is a design plus a mock-only executable. It is deliberately not
linked into `hg_mcp_server`, the web demo or the bridge, and it touches no other workstream's code.

## 1. What exists, and what the host has to supply

| Component | State | Role in the host |
|---|---|---|
| `CameraInventory` | exists, tested | durable fingerprint → logical camera identity; churn vs new-device resolution; health |
| `SiteProfile` | exists, tested | the connector's OWN identity, its credential id and its reach allowlist (CIDR) |
| `SiteConnector` | exists, tested | per-frame authorization, consent enforcement, session termination, bounded telemetry |
| `ControlChannel` | exists, tested | the closed control vocabulary with a hard 1 KiB ceiling — media is unrepresentable |
| `DirectMediaPlanner` | exists, tested | direct-preferred media path; a relay is an approval-gated outcome, never automatic |
| `IDeviceAuthorizer` / `RegistryAuthorizer` | exists, tested | authorization against the existing device registry |
| `ICameraAdapter` | interface exists; **no real adapter** | the only piece that would speak to hardware |
| **A host process** | **this document + `hg_site_connector_rehearsal`** | what the design below specifies |

## 2. Minimal deployable host design

**Process model.** One process per site. It is **outbound-only**: it dials the coordination endpoint
and never listens on a port. There is no inbound surface to attack, no port forwarding, and no need
for the site's network to be reachable from anywhere. Device-local reach (cameras) is used only from
inside the site, by the adapter.

**Lifecycle.**

1. Load its `SiteProfile` (its own connector identity, its credential id, its reach allowlist).
   A profile that is invalid — no credential, no allowlist, or an address used as an identity —
   reaches **nothing** rather than reaching everything.
2. Load the inventory (fingerprint bindings + addresses as attachments) and start the control
   channel to the coordinator.
3. Per camera, on a schedule: observe (fingerprint from the neighbour table, then documented ports
   only) → resolve identity (known / moved / new candidate) → register → authorize → account for a
   bounded frame batch → send bounded telemetry.
4. Obey the control plane: consent changes, revocations and terminations take effect on the next
   check, because authorization is re-evaluated **per frame**, not once per session.

**The adapter seam.** The host knows `ICameraAdapter` and nothing about protocols. Adding a camera
family means adding an adapter — no change to identity, authorization, consent, control or the
media planner. A deployment with no authorized interface for a camera simply reports that camera as
registered/control-only, which is the honest state rather than a failure to paper over.

**Configuration surface.** Site id · connector logical identity · credential id · reach allowlist ·
coordination endpoint · per-camera owner-provided identity + credential · offline timeout ·
telemetry interval. Nothing else. No address appears in an identity field, and the planner refuses
to treat one as identity if someone tries.

**Failure modes and the designed response.**

| Failure | Response |
|---|---|
| Camera reachable but silent | health → offline after the timeout; the record is **kept**, never deleted; recovery restores the same identity |
| Camera's DHCP lease changes | `known_moved`; identity and authorization unchanged; the old address becomes history |
| A genuinely new device appears | **candidate only** — no identity, no consent, no collection until the owner authorizes it |
| An observation is inconsistent (one NIC at two addresses, one address claimed by two NICs) | the pass is refused and **nothing is applied** |
| Consent withdrawn mid-session | the session terminates; the control plane is told it was a **consent change**; no observation is emitted |
| Device revoked | the session terminates; reported as a **revocation** (a different fact from a consent change) |
| Coordinator unreachable | the host buffers nothing media-shaped (it has none) and retries with bounded backoff; camera health is unaffected |
| Reach violation | `out_of_site_scope`; the camera is not even asked about |

**What the host must never do** (enforced by the layering, not by good intentions): carry media on
the control channel; route a stream through the coordination hub; collect from a camera outside its
reach allowlist; treat an address, model string or SSID as identity; emit any observation for a
refused device; or infer firmware/media readiness without a verified read-out.

**Deployment envelope.** Needs: a small always-on host inside the camera LAN, outbound network
access, a filesystem location for the inventory file, and no privileges beyond that — no root, no
inbound port, no camera credentials beyond the adapter's own. It does not need a GPU, a database,
TLS termination, or any media capability in the binary.

## 3. The rehearsal, and what it measured

`hg_site_connector_rehearsal` runs the whole chain in one executable with **mock adapters only**:
it opens no socket, speaks no camera protocol, and carries no media. It is a self-checking
transcript (non-zero exit on any failed expectation), not a demo that always "passes".

Run: `./build/hg_site_connector_rehearsal [cycles]`

**Result: 54 checks, 0 failures.** It exercises site-profile validity, three distinct camera
identities, registry authorization (including refusing a credential that belongs to another
camera), registration, start, one accounted frame per camera with **synthetic provenance asserted**,
reach isolation (an out-of-scope camera pulls **zero** frames), a DHCP address change resolved as
`known_moved` with the identity unchanged, consent withdrawal terminating a session with a
**consent-change** message and no observation, revocation reported as a **revocation**, an unrelated
camera keeping its session, and the media-path decision selecting a site-owned path with
`uses_control_hub == false`.

**Resource envelope — a fixture measurement, not a 24/7 claim:**

| Measurement | Single run | 20,000-cycle soak |
|---|---|---|
| Peak RSS (self-reported) | 3.69 MiB | 4.29 MiB |
| Peak RSS (`/usr/bin/time -v`) | 4.16 MiB | 4.76 MiB |
| Resident growth over the run | 64 KiB | **640 KiB** over 20,000 cycles |
| Wall time | ~1 ms | 254 ms |
| CPU | — | 0.16 s user + 0.09 s system |

Two findings worth keeping:

1. **The footprint is bounded, but only if the consumer forwards rather than hoards.** The first
   soak run grew to **48 MiB**: the cause was the rehearsal's own sink retaining every observation
   (an `Event` carries a JSON payload, ~2.2 KiB each). The connector was not leaking; a host that
   keeps every observation in memory is. The host must forward to the pipeline/persistence and keep
   at most a bounded tail. This is now asserted (growth < 2 MiB) and documented, because "it ran for
   a week" would not have caught it.
2. **Recovery from a consent withdrawal takes two explicit acts.** The existing registry latches the
   operator's capture switch off when consent is withdrawn, so re-consenting alone does not resume
   collection. That is the right default — a withdrawn camera must not silently return — and the
   rehearsal now demonstrates it rather than tripping over it.

**What this does not prove.** No camera, no media, no socket, no always-on duty, no thermal or
battery behaviour, no cross-ISP path, and nothing about the Mi A3. The numbers describe one process
on one workstation, which this project has already measured as not-always-on.

## 4. Remaining prerequisites before this host could run for real

1. An always-on host inside the camera LAN (owner: procurement/placement).
2. A credential issued for that connector identity, and site enrolment (local binding is in code;
   distribution is operational).
3. An authorized camera interface before any media claim — otherwise the host runs
   **control-only** and says so.
4. A media path exercised end to end (peer-to-peer or site-uplink) — designed and refused-to-hub by
   construction, but not demonstrated.

## 5. Priority

This work is deliberately below the mandatory MCP demonstration and the independent security
release gate. Nothing here changes the demo surface, and nothing here is required for submission.
