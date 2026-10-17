#pragma once

// ICameraAdapter — the camera-SPECIFIC seam (milestone HG-V380-09, task 1).
//
// This is the ONLY interface that knows anything about a particular camera family.
// Everything above it (registry, consent, processing, Alexa-facing controls) talks
// to this seam and never to a vendor protocol.
//
// WHY IT IS SHAPED THIS WAY
//   The two V380 units failed in two different ways, and both failures are
//   representable here rather than hidden:
//     * camera 1 (Hw_HsAkQQVL_WF_QQ_20240412): the standard interface existed and
//       enforced nothing -> an adapter for it must be able to report
//       `refused_unsafe_interface` and be left stopped.
//     * camera 2 (Hw_AWT3610E_XHR_E_V1.0_WF_20230807): no standard interface exists
//       -> an adapter may declare NO video capability at all, and must then fail a
//       stream request with `capability_absent` rather than substituting something.
//
// CONTRACT NOTES
//   * `media_available()` and `read_frame()` hand back METADATA and a bounded payload
//     COUNT. An adapter must never buffer or expose pixel data through this seam; the
//     connector only needs a bounded activity signal.
//   * No method may block indefinitely: every operation is expected to respect the
//     caller's budget. Implementations that cannot honour a budget must return a
//     classified failure instead of hanging.
//   * `adapter_class()` is authoritative: the connector derives an Observation's
//     provenance from it, so a mock is structurally incapable of being reported as a
//     real camera.

#include "CameraTypes.h"

#include <cstdint>
#include <string>

namespace homeguardian::camera {

class ICameraAdapter {
public:
    virtual ~ICameraAdapter() = default;

    // ---- identity (stable, logical, never address-derived) ----
    virtual const LogicalDeviceId& id() const = 0;

    // ---- what this adapter IS ----
    virtual AdapterClass adapter_class() const = 0;

    // Informational only: model/firmware/LAN address as OBSERVED. The connector must
    // never use this for an authorization decision, and must not treat it as identity.
    virtual std::string attachment_summary() const = 0;

    // The address this adapter actually reached, as an OBSERVATION. It is used for exactly
    // one thing: checking the camera against the site profile's REACH allowlist (multi-site
    // isolation). It is never an identity and never an authorization input. An adapter that
    // cannot observe an address returns empty, and a connector with a site profile then
    // refuses rather than assuming.
    virtual std::string observed_ip() const { return {}; }

    // ---- what the adapter CLAIMS the device can do ----
    // A declaration is not a grant. The connector still consults the authorizer before
    // any frame or control command.
    virtual CapabilitySet declared_capabilities() const = 0;

    // ---- lifecycle ----
    // Probe reachability and interface availability without consuming media.
    virtual ConnectOutcome probe() = 0;

    // Begin a bounded stream. Requires Capability::video to be declared AND the caller
    // to hold authorization; an adapter must not itself decide consent.
    virtual ConnectOutcome start_stream(const StreamRequest& request) = 0;

    // Stop and release. Idempotent, never throws.
    virtual void stop_stream() noexcept = 0;

    // True while the adapter believes a stream is running.
    virtual bool streaming() const = 0;

    // Pull up to `max` frames' worth of METADATA from the active stream.
    // Returns the number of frames accounted for (0 when none are ready, when nothing
    // is streaming, or when the transport closed — check `last_outcome()`).
    virtual size_t read_frames(FrameMeta* out, size_t max) = 0;

    // Send a closed-vocabulary control command. Requires the matching declared
    // capability; otherwise `capability_absent`.
    virtual ConnectOutcome control(ControlCommand command) = 0;

    // The outcome of the most recent operation, so a caller can classify a refusal
    // without the adapter having to throw.
    virtual ConnectOutcome last_outcome() const = 0;

    // Counts of frames accounted for since construction. Useful as a bounded activity
    // signal in tests and status output.
    virtual uint64_t frames_accounted() const = 0;
};

}  // namespace homeguardian::camera
