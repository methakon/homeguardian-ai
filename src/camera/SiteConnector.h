#pragma once

// SiteConnector — the component that sits between a camera adapter and everything
// else: the registry/authorization layer, the consent rules, the existing event
// pipeline, and (later) the Alexa-facing controls (milestone HG-V380-09, task 1).
//
// ITS JOB, IN ONE SENTENCE
//   Make it impossible to consume a camera without an authorization check that is
//   current at the moment of use — and make every refusal carry its own name.
//
// THE THREE RULES IT ENFORCES
//   1. AUTHORIZATION BEFORE EVERY FRAME, not once per session. The check is repeated
//      immediately before each frame is accounted for, which is the
//      time-of-check/time-of-use closing point: a consent withdrawal, a revocation or
//      the operator's capture switch taking effect mid-stream must stop the stream,
//      not wait for the next session.
//   2. A MOCK CANNOT BE REPORTED AS REAL. The observation's `Provenance` is derived
//      from `adapter.adapter_class()`; there is no path by which a mock produces a
//      `real` observation.
//   3. NOTHING IS EMITTED FOR A DEVICE THAT MAY NOT BE USED. A denied device produces
//      no stream observation at all — not a "no activity" one — so an unauthorized
//      camera can never look like a quiet one.
//
// WHAT IT DOES NOT DO
//   It opens no socket and speaks no protocol. The only transport it knows is the
//   adapter's own interface, which is why the same connector will drive a real
//   RTSP/ONVIF adapter later without changing this file. It also never forwards
//   media: it accounts for frame METADATA and hands the count to the existing
//   pipeline as a bounded Observation, so video never travels through the event path.

#include "ICameraAdapter.h"
#include "IDeviceAuthorizer.h"

#include "ControlChannel.h"
#include "SiteProfile.h"

#include "core/Observation.h"

#include <cstdint>
#include <functional>
#include <string>

namespace homeguardian::camera {

struct ConnectorConfig {
    // Bounded pull: the connector accounts for at most this many frames per call and
    // then re-checks authorization before the next batch.
    uint32_t max_frames_per_pull = 4;

    // Reconnection is bounded and gives up rather than looping forever.
    uint32_t max_reconnect_attempts = 3;
    uint64_t backoff_base_ms = 250;   // advisory: reported in status for a caller that sleeps

    // How often an unchanged `unavailable` state may be re-announced. 1 = only on a
    // transition; higher values allow periodic refresh. Never unbounded: a device that
    // stays unreachable must not flood the pipeline.
    uint32_t reannounce_unavailable_every = 0;

    // How often a successful pull also sends a telemetry control message. 0 = never.
    // Bounded on purpose: the hub carries control, and control stays low-volume.
    uint32_t telemetry_every_n_pulls = 10;
};

struct ConnectorStatus {
    AdapterClass adapter_class = AdapterClass::mock;
    LogicalDeviceId id{};

    bool registered = false;            // the authorization check succeeded at registration
    bool streaming = false;
    bool session_active = false;        // a live session exists and may still be used
    bool out_of_site_scope = false;     // the camera failed the site reach allowlist

    uint64_t frames_accounted = 0;
    uint32_t pulls_allowed = 0;
    uint32_t pulls_denied = 0;

    uint32_t observations_emitted = 0;
    uint32_t observations_suppressed = 0;
    uint32_t observations_rejected = 0;   // validate() refused to build one

    uint32_t reconnects = 0;
    uint32_t reconnect_failures = 0;

    // Set when an authorization change forced a stop mid-stream. Distinguishes "the
    // device went away" from "we withdrew permission", which are different incidents.
    bool stopped_by_authorization = false;

    // Control-channel accounting. `control_refused` counts messages the channel itself
    // refused (oversized or malformed) - a signal worth surfacing rather than hiding.
    uint64_t control_sent = 0;
    uint64_t control_refused = 0;

    AuthorizationOutcome last_denial = AuthorizationOutcome::allowed;
    ConnectOutcome last_connect = ConnectOutcome::ok;
    std::string session_termination_reason{};
};

class SiteConnector {
public:
    // The sink is the existing event path: typically
    // `[&](const homeguardian::Event& e) { pipeline.process(e); }`.
    using EventSink = std::function<void(const homeguardian::Event&)>;

    // `profile` and `control` are BORROWED and optional; both may be null, in which case
    // reach checking and hub messaging are simply not performed (a development setup).
    SiteConnector(ICameraAdapter& adapter,
                  const IDeviceAuthorizer& authorizer,
                  std::string credential_id,
                  EventSink sink,
                  ConnectorConfig config = {},
                  const SiteProfile* profile = nullptr,
                  ControlChannel* control = nullptr);

    const LogicalDeviceId& id() const { return adapter_.id(); }

    // Ask the authorizer. Exposed so a caller can check before acting, and so a test
    // can assert the exact refusal reason.
    AuthorizationOutcome authorize(Capability capability) const;

    // Check the camera's OBSERVED address against the bound site profile's reach
    // allowlist. `ok` when no profile is bound; `out_of_site_scope` when one is bound and
    // the address is outside it, or cannot be evaluated at all - an unevaluable reach rule
    // must not become permission.
    ConnectOutcome scope_check() const;

    // Register/report this device. Requires authorization for `video`; records the
    // outcome either way and never throws.
    AuthorizationOutcome register_device();

    // Start a bounded stream. Order of checks: authorization, then the adapter's
    // DECLARED capability (a declaration is not a grant, but a missing declaration is
    // a hard stop), then the adapter's own start.
    ConnectOutcome start();

    // Account for up to `max_frames_per_pull` frames, RE-AUTHORIZING BEFORE EACH ONE.
    // Returns the number of frames accounted for. On a denial mid-pull it stops the
    // stream, records the reason and emits nothing.
    size_t pull_once(int64_t now_ms);

    // Simulate connectivity loss and reconnect. Bounded by max_reconnect_attempts.
    // A device revoked (or un-consented, or capture-disabled) while offline must NOT
    // resume: authorization is re-evaluated before the adapter is restarted.
    ConnectOutcome reconnect(int64_t now_ms);

    // End the session deliberately (operator action, or a termination received from the
    // control plane). Idempotent.
    void terminate_session(const std::string& reason, int64_t now_ms);

    void stop() noexcept;

    bool session_active() const { return status_.session_active; }
    bool has_site_profile() const { return profile_ != nullptr; }
    const ConnectorStatus& status() const { return status_; }

private:
    // Build and submit one bounded Observation. Returns false when validate() refused
    // it (counted, never silently dropped).
    bool emit(DetectorStatus detector_status, const std::string& note,
              uint64_t frames, uint32_t bytes, int64_t now_ms);

    // Send one control message over the attached channel, if any. Counts what the channel
    // refused. Never throws, never blocks. Media has no representation here.
    void send_control(ControlKind kind, const std::string& detail, int64_t now_ms);

    // Stop the adapter and mark the session dead, without emitting anything.
    void end_session(const std::string& reason);

    Provenance provenance_for_adapter() const;

    ICameraAdapter& adapter_;
    const IDeviceAuthorizer& authorizer_;
    std::string credential_;
    EventSink sink_;
    ConnectorConfig cfg_;
    const SiteProfile* profile_ = nullptr;
    ControlChannel* control_ = nullptr;
    ConnectorStatus status_{};

    bool has_emitted_ = false;
    DetectorStatus last_emitted_status_ = DetectorStatus::unavailable;
    uint64_t observation_seq_ = 0;
    uint32_t consecutive_unavailable_ = 0;
    uint64_t pull_seq_ = 0;
    uint64_t control_seq_ = 0;
};

}  // namespace homeguardian::camera
