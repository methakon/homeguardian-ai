#pragma once

// Home Assistant integration adapter (read-only, disabled by default).
//
// SCOPE: this complements the central HomeGuardian C++ hub; it does NOT replace
// its engine. Home Assistant contributes ENTITY STATE (motion, contact,
// occupancy, camera/microphone availability, environmental readings) which the
// adapter converts into validated HomeGuardian observations that flow through the
// existing pipeline. There is no second alert engine, and no actuation: the
// adapter reads `/api/states/<entity_id>` and nothing else. Locks, alarms,
// sirens, cameras and every other actuator are out of scope until explicitly
// approved.
//
// THE TEN REQUIRED BEHAVIOURS, and where each one lives:
//   1 configure the instance address          -> HomeAssistantConfig::base_url
//   2 store credentials securely              -> token_file + read_secret_file()
//                                                (refuses a group/other-readable
//                                                file); token lives only inside the
//                                                transport, never in a log, an
//                                                observation, a feature payload,
//                                                an image layer or describe()
//   3 discover permitted entities             -> entity_allowlist (explicit)
//   4 allowlist what HomeGuardian may consume -> entity_allowlist, and an
//                                                unmapped entity is SKIPPED, never
//                                                guessed
//   5 authorized updates -> validated obs     -> map_entity() + SecurityObservation
//                                                ::from_json-style bounded mapping
//   6 track online/offline                    -> DeviceAvailabilityTracker
//   7 reconnect without fabricating events    -> the reconnect transition emits an
//                                                availability observation only
//   8 no duplicate ingestion                  -> SensorDeduplicator + a
//                                                deterministic observation id
//   9 real-vs-synthetic provenance            -> Provenance::real for HA entities,
//                                                synthetic for fixtures/tests
//  10 disabled until configured               -> enabled = false by default, and
//                                                poll_once() refuses when disabled
//                                                or unauthorized
//
// NOT DONE ON PURPOSE: the skill suggests Frigate/YOLO/MediaPipe inference on the
// hub. That is deferred, not adopted - those runtimes are not present, and the
// project forbids downloading model weights without a licensing/resource review.

#include "core/HttpClient.h"
#include "core/SecurityObservation.h"

#include <chrono>
#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace homeguardian {

struct HomeAssistantConfig {
    // Disabled until an operator configures and enables the integration.
    bool enabled = false;
    std::string base_url;      // e.g. "http://homeassistant.local:8123"
    std::string token_file;    // path to a mounted long-lived token (0600)
    std::vector<std::string> entity_allowlist;  // explicit; nothing else is read

    std::string consent_subject = "home-assistant";
    std::string consent_purpose = "security_monitor";

    // The plaintext gate. An http:// endpoint is used ONLY when this is true:
    // there is no TLS in this build, so a bearer token would otherwise be sent in
    // the clear by default.
    bool allow_plaintext = false;

    int timeout_ms = HttpPolicy::kDefaultTimeoutMs;
    std::chrono::seconds offline_after{300};
    std::chrono::seconds dedup_cooldown{60};
    size_t max_entities = 64;

    static constexpr size_t kMaxAllowlist = 64;
    static constexpr size_t kMaxEntityIdChars = 96;

    // Fail-closed: every problem is returned. An empty vector means "usable".
    std::vector<std::string> problems() const;
};

// Read a secret file, refusing one that group or other can read. Returns
// std::nullopt with a bounded reason on any failure.
std::optional<std::string> read_secret_file(const std::string& path, std::string* reason,
                                            size_t max_bytes = 4096);

// The transport seam. Tests inject a deterministic transport; the hub uses the
// HTTP one. `describe()` must never return a credential.
class IHomeAssistantTransport {
public:
    struct Response {
        bool ok = false;
        int status = 0;
        std::string body;
        std::string error;
    };

    virtual ~IHomeAssistantTransport() = default;
    virtual Response get(const std::string& path) = 0;
    virtual std::string describe() const = 0;
};

class HttpHomeAssistantTransport : public IHomeAssistantTransport {
public:
    HttpHomeAssistantTransport(const std::string& base_url, std::string token,
                               HttpClient::Config cfg = HttpClient::defaults());

    Response get(const std::string& path) override;
    std::string describe() const override;
    bool has_token() const { return !token_.empty(); }
    bool url_valid() const { return base_.valid; }

private:
    HttpClient client_;
    HttpClient::Url base_;
    std::string token_;  // private, never exposed by any accessor
};

// One entity's mapping decision. `handled == false` means "not ingested", with
// the reason recorded - the adapter never guesses a meaning for an entity it does
// not understand.
struct EntityMapping {
    bool handled = false;
    std::string reason;
    SecuritySourceType source_type = SecuritySourceType::motion;
    std::string state;
    std::optional<double> value;
    std::string device_class;
    std::string unit;
};

class HomeAssistantAdapter {
public:
    struct PollResult {
        std::vector<SecurityObservation> observations;
        std::vector<DeviceAvailabilityTracker::Transition> availability;
        size_t entities_checked = 0;
        size_t entities_skipped = 0;
        size_t duplicates = 0;
        size_t errors = 0;
        bool polled = false;
        std::string error;  // first error, bounded
    };

    HomeAssistantAdapter(HomeAssistantConfig cfg,
                         std::unique_ptr<IHomeAssistantTransport> transport);

    // Consumption requires an explicit consent decision. Until set_authorized(true)
    // is called, poll_once() performs NO request at all (fail-closed).
    void set_authorized(bool authorized) { authorized_ = authorized; }
    bool authorized() const { return authorized_; }

    bool enabled() const { return cfg_.enabled; }
    bool configured() const { return cfg_.enabled && cfg_.problems().empty(); }
    const std::vector<std::string>& problems() const { return problems_; }

    PollResult poll_once(std::chrono::system_clock::time_point now);

    // Pure mapping: entity id + HA state + attributes -> what HomeGuardian would
    // ingest. Unit-tested directly, so the mapping contract is verifiable without
    // a Home Assistant instance.
    static EntityMapping map_entity(const std::string& entity_id, const std::string& state,
                                    const nlohmann::json& attributes);

    const DeviceAvailabilityTracker& availability() const { return availability_; }
    std::string transport_description() const;

    // Number of entities the operator has allowed. Bounded by kMaxAllowlist.
    size_t allowed_entity_count() const { return cfg_.entity_allowlist.size(); }

private:
    HomeAssistantConfig cfg_;
    std::unique_ptr<IHomeAssistantTransport> transport_;
    DeviceAvailabilityTracker availability_;
    SensorDeduplicator dedup_;
    std::vector<std::string> problems_{};
    bool authorized_ = false;
};

}  // namespace homeguardian
