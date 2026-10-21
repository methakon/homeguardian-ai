#include "HomeAssistantAdapter.h"

#include "core/NodeIdentity.h"  // sanitize_label

#include <sys/stat.h>

#include <fstream>
#include <set>
#include <sstream>

namespace homeguardian {

namespace {

constexpr size_t kMaxFeaturesBytes = SecurityObservation::kMaxFeatureJsonBytes;

bool entity_id_shape_ok(const std::string& entity_id) {
    if (entity_id.empty() || entity_id.size() > HomeAssistantConfig::kMaxEntityIdChars) return false;
    const size_t dot = entity_id.find('.');
    if (dot == std::string::npos || dot == 0 || dot + 1 >= entity_id.size()) return false;
    for (char c : entity_id) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '.';
        if (!ok) return false;
    }
    return true;
}

std::string domain_of(const std::string& entity_id) {
    const size_t dot = entity_id.find('.');
    return dot == std::string::npos ? std::string() : entity_id.substr(0, dot);
}

std::string bounded_attr_string(const nlohmann::json& attributes, const char* key, size_t max_chars) {
    if (!attributes.is_object() || !attributes.contains(key)) return std::string();
    const nlohmann::json& v = attributes[key];
    if (!v.is_string()) return std::string();
    return sanitize_label(v.get<std::string>(), max_chars);
}

std::optional<double> numeric_state(const std::string& state) {
    if (state.empty()) return std::nullopt;
    try {
        size_t consumed = 0;
        const double value = std::stod(state, &consumed);
        if (consumed != state.size()) return std::nullopt;  // trailing junk is not a number
        return value;
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

}  // namespace

std::vector<std::string> HomeAssistantConfig::problems() const {
    std::vector<std::string> out;
    if (!enabled) {
        out.push_back("integration is disabled (this is the default)");
        return out;
    }
    if (base_url.empty()) out.push_back("base_url is not configured");
    if (base_url.size() > HttpPolicy::kMaxUrlChars) out.push_back("base_url is too long");
    if (token_file.empty()) out.push_back("token_file is not configured");
    if (entity_allowlist.empty()) {
        out.push_back("entity_allowlist is empty: no entity may be consumed");
    }
    if (entity_allowlist.size() > kMaxAllowlist) out.push_back("entity_allowlist is too large");
    for (const std::string& entity : entity_allowlist) {
        if (!entity_id_shape_ok(entity)) {
            out.push_back("entity id has an invalid shape: " + sanitize_label(entity, 48));
        }
    }
    const HttpClient::Url url = HttpClient::parse_url(base_url);
    if (!url.valid) {
        out.push_back("base_url is unusable: " + url.error);
    } else if (!allow_plaintext) {
        // The token would travel in the clear. Refuse unless the operator has
        // explicitly accepted that, since this build cannot do TLS.
        out.push_back(
            "plaintext endpoint refused: set allow_plaintext=true to accept that the bearer token "
            "travels unencrypted on the LAN (no TLS is linked in this build)");
    }
    if (consent_subject.empty()) out.push_back("consent_subject is empty");
    if (consent_purpose.empty()) out.push_back("consent_purpose is empty");
    return out;
}

std::optional<std::string> read_secret_file(const std::string& path, std::string* reason,
                                            size_t max_bytes) {
    auto fail = [reason](const std::string& why) -> std::optional<std::string> {
        if (reason != nullptr) *reason = why;
        return std::nullopt;
    };
    if (path.empty()) return fail("token file path is empty");

    struct stat st {};
    if (::stat(path.c_str(), &st) != 0) return fail("token file is not readable");
    if (!S_ISREG(st.st_mode)) return fail("token file is not a regular file");
    if (st.st_mode & (S_IRWXG | S_IRWXO)) {
        return fail("token file is readable by group or other (expected mode 0600)");
    }
    if (static_cast<size_t>(st.st_size) > max_bytes) return fail("token file is too large");

    std::ifstream in(path, std::ios::binary);
    if (!in) return fail("token file could not be opened");
    std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    while (!content.empty() && (content.back() == '\n' || content.back() == '\r')) content.pop_back();
    if (content.empty()) return fail("token file is empty");
    if (reason != nullptr) reason->clear();
    return content;
}

// ---------------------------------------------------------------------------
// HttpHomeAssistantTransport
// ---------------------------------------------------------------------------

HttpHomeAssistantTransport::HttpHomeAssistantTransport(const std::string& base_url, std::string token,
                                                       HttpClient::Config cfg)
    : client_(cfg), base_(HttpClient::parse_url(base_url)), token_(std::move(token)) {}

IHomeAssistantTransport::Response HttpHomeAssistantTransport::get(const std::string& path) {
    Response out;
    if (!base_.valid) {
        out.error = "base url is unusable: " + base_.error;
        return out;
    }
    if (token_.empty()) {
        out.error = "no token loaded";
        return out;
    }
    // The Authorization header exists only inside this call: it is never stored on
    // an observation, never returned by describe(), and never logged.
    const std::vector<std::pair<std::string, std::string>> headers = {
        {"Authorization", "Bearer " + token_}};
    const HttpClient::Url target = [&] {
        HttpClient::Url u = base_;
        u.path = path;
        return u;
    }();
    const HttpResponse response = client_.get(target, headers);
    out.ok = response.ok;
    out.status = response.status;
    out.body = response.body;
    if (!response.error.empty()) out.error = response.error;
    else if (response.truncated) out.error = "response truncated at the size cap";
    return out;
}

std::string HttpHomeAssistantTransport::describe() const {
    if (!base_.valid) return "invalid base url";
    // Non-sensitive by construction: no token, no fingerprint, no header.
    return "read-only entity state over plaintext http://" + base_.host + ":" +
           std::to_string(base_.port);
}

// ---------------------------------------------------------------------------
// HomeAssistantAdapter
// ---------------------------------------------------------------------------

HomeAssistantAdapter::HomeAssistantAdapter(HomeAssistantConfig cfg,
                                           std::unique_ptr<IHomeAssistantTransport> transport)
    : cfg_(std::move(cfg)), transport_(std::move(transport)) {
    problems_ = cfg_.problems();
    availability_ = DeviceAvailabilityTracker([&] {
        DeviceAvailabilityTracker::Config c;
        c.offline_after = cfg_.offline_after;
        c.max_devices = cfg_.max_entities;
        return c;
    }());
    dedup_ = SensorDeduplicator([&] {
        SensorDeduplicator::Config c;
        c.cooldown = cfg_.dedup_cooldown;
        c.max_entries = cfg_.max_entities * 4;
        return c;
    }());
}

EntityMapping HomeAssistantAdapter::map_entity(const std::string& entity_id,
                                               const std::string& state,
                                               const nlohmann::json& attributes) {
    EntityMapping m;
    m.device_class = bounded_attr_string(attributes, "device_class", 32);
    m.unit = bounded_attr_string(attributes, "unit_of_measurement", 16);

    if (!entity_id_shape_ok(entity_id)) {
        m.reason = "entity id has an invalid shape";
        return m;
    }
    if (state.empty()) {
        m.reason = "entity has no state";
        return m;
    }

    // An unavailable entity is an availability finding, whatever its domain.
    if (state == "unavailable" || state == "unknown") {
        m.handled = true;
        m.source_type = SecuritySourceType::device_availability;
        m.state = "offline";
        m.reason = "entity reports '" + state + "'";
        return m;
    }

    const std::string domain = domain_of(entity_id);
    if (domain == "binary_sensor") {
        // Only an explicitly mapped device_class is ingested. Guessing that an
        // unlabelled binary_sensor is "motion" is exactly the kind of assumption
        // this milestone forbids.
        if (m.device_class == "motion" || m.device_class == "moving") {
            m.source_type = SecuritySourceType::motion;
        } else if (m.device_class == "occupancy" || m.device_class == "presence") {
            m.source_type = SecuritySourceType::presence;
        } else if (m.device_class == "door" || m.device_class == "window" ||
                   m.device_class == "opening" || m.device_class == "garage_door") {
            m.source_type = SecuritySourceType::contact;
        } else if (m.device_class == "tamper") {
            m.source_type = SecuritySourceType::tamper;
        } else {
            m.reason = m.device_class.empty()
                           ? "binary_sensor without a device_class is not ingested"
                           : "unmapped device_class '" + m.device_class + "'";
            return m;
        }
        if (state != "on" && state != "off") {
            m.reason = "unexpected binary state '" + sanitize_label(state, 24) + "'";
            return m;
        }
        m.handled = true;
        // Contact uses the door/window vocabulary; the rest use on/off.
        m.state = (m.source_type == SecuritySourceType::contact)
                      ? (state == "on" ? "open" : "closed")
                      : state;
        m.reason = "mapped from binary_sensor." + m.device_class;
        return m;
    }

    if (domain == "camera") {
        m.handled = true;
        m.source_type = SecuritySourceType::camera_availability;
        m.state = "online";  // a reachable camera entity that is not 'unavailable'
        m.reason = "camera entity is reachable";
        return m;
    }

    if (domain == "sensor") {
        if (m.device_class != "temperature" && m.device_class != "humidity" &&
            m.device_class != "pressure") {
            m.reason = m.device_class.empty() ? "sensor without a mapped device_class"
                                              : "unmapped sensor device_class '" + m.device_class + "'";
            return m;
        }
        const auto value = numeric_state(state);
        if (!value.has_value()) {
            m.reason = "sensor state is not numeric";
            return m;
        }
        m.handled = true;
        m.source_type = SecuritySourceType::environmental_threshold;
        m.value = value;
        // NO threshold is invented here. Without an operator-configured threshold
        // the honest state is "within"; a crossing must be configured explicitly.
        m.state = "within";
        m.reason = "environmental reading ingested; no threshold configured, so no crossing claimed";
        return m;
    }

    m.reason = "domain '" + domain + "' is not consumed by this adapter";
    return m;
}

HomeAssistantAdapter::PollResult HomeAssistantAdapter::poll_once(
    std::chrono::system_clock::time_point now) {
    PollResult result;

    if (!cfg_.enabled) {
        result.error = "integration disabled";
        return result;
    }
    if (!problems_.empty()) {
        result.error = "configuration invalid: " + problems_.front();
        return result;
    }
    if (!authorized_) {
        // Fail-closed: no consent decision, no request.
        result.error = "not authorized: a consent decision is required before reading any entity";
        return result;
    }
    if (!transport_) {
        result.error = "no transport configured";
        return result;
    }
    result.polled = true;

    for (const std::string& entity : cfg_.entity_allowlist) {
        const IHomeAssistantTransport::Response response = transport_->get("/api/states/" + entity);
        if (!response.ok) {
            ++result.errors;
            if (result.error.empty()) {
                result.error = "entity read failed (status " + std::to_string(response.status) +
                               (response.error.empty() ? std::string() : ", " + response.error) + ")";
            }
            // A failed read does NOT mark the entity seen: liveness must decay so
            // a dead entity is distinguishable from a silent one.
            continue;
        }
        ++result.entities_checked;

        const nlohmann::json parsed = nlohmann::json::parse(response.body, nullptr, false);
        if (parsed.is_discarded() || !parsed.is_object()) {
            ++result.errors;
            if (result.error.empty()) result.error = "entity payload is not valid JSON";
            continue;
        }
        const std::string state = parsed.value("state", std::string());
        const nlohmann::json attributes =
            parsed.contains("attributes") && parsed["attributes"].is_object()
                ? parsed["attributes"]
                : nlohmann::json::object();

        const EntityMapping mapping = map_entity(entity, state, attributes);
        if (!mapping.handled) {
            ++result.entities_skipped;
            continue;
        }

        // The entity answered: liveness refreshed (first sight produces a
        // transition the caller may surface as an availability observation).
        const auto transition = availability_.mark_seen(entity, now);

        SecurityObservation observation;
        observation.device_id = entity;
        observation.source = "home-assistant";
        observation.source_type = mapping.source_type;
        observation.acquired_at = now;
        observation.ingested_at = now;
        observation.provenance = Provenance::real;  // a real external entity
        observation.state = mapping.state;
        observation.consent_subject = cfg_.consent_subject;
        observation.consent_purpose = cfg_.consent_purpose;
        observation.authorized = authorized_;
        observation.features = nlohmann::json::object();
        observation.features["entity_id"] = entity;
        observation.features["ha_state"] = sanitize_label(state, 24);
        if (!mapping.device_class.empty()) observation.features["device_class"] = mapping.device_class;
        if (mapping.value.has_value()) observation.features["value"] = mapping.value.value();
        if (!mapping.unit.empty()) observation.features["unit"] = mapping.unit;
        observation.note = mapping.reason;
        if (observation.features.dump().size() > kMaxFeaturesBytes) {
            ++result.errors;
            if (result.error.empty()) result.error = "mapped features exceed the size bound";
            continue;
        }

        SensorDeduplicator::Key key;
        key.device_id = entity;
        key.source_type = mapping.source_type;
        key.state = mapping.state;
        if (dedup_.duplicate(key, now)) {
            ++result.duplicates;
            continue;
        }

        try {
            observation.validate();
        } catch (const std::exception& e) {
            ++result.errors;
            if (result.error.empty()) result.error = std::string("mapped observation invalid: ") + e.what();
            continue;
        }
        if (transition.has_value()) {
            // First sight only; a reconnect is reported through sweep() below.
            // NOTE: built EXPLICITLY rather than copied from `observation`, because
            // the entity observation is moved into the result above and a
            // moved-from object has empty strings (device id, consent context) and
            // a defaulted provenance. The unit test caught exactly that.
            SecurityObservation availability;
            availability.device_id = entity;
            availability.source = "home-assistant";
            availability.source_type = SecuritySourceType::device_availability;
            availability.acquired_at = transition->at;
            availability.ingested_at = now;
            availability.provenance = Provenance::real;
            availability.state = transition->online ? "online" : "offline";
            availability.consent_subject = cfg_.consent_subject;
            availability.consent_purpose = cfg_.consent_purpose;
            availability.authorized = authorized_;
            availability.note = transition->reason;
            availability.features = nlohmann::json::object();
            availability.features["entity_id"] = entity;
            availability.features["first_seen"] = transition->first_seen;
            result.observations.push_back(std::move(availability));
        }

        result.observations.push_back(std::move(observation));
    }

    // Silence past the timeout becomes an offline transition, once per entity.
    const std::vector<DeviceAvailabilityTracker::Transition> offline = availability_.sweep(now);
    for (const DeviceAvailabilityTracker::Transition& t : offline) {
        SecurityObservation observation;
        observation.device_id = t.device_id;
        observation.source = "home-assistant";
        observation.source_type = SecuritySourceType::device_availability;
        observation.acquired_at = t.at;
        observation.ingested_at = now;
        observation.provenance = Provenance::real;
        observation.state = "offline";
        observation.consent_subject = cfg_.consent_subject;
        observation.consent_purpose = cfg_.consent_purpose;
        observation.authorized = authorized_;
        observation.features = nlohmann::json::object();
        observation.features["entity_id"] = t.device_id;
        observation.features["transition"] = "online->offline";
        observation.note = t.reason;
        result.observations.push_back(std::move(observation));
    }
    result.availability = offline;
    return result;
}

std::string HomeAssistantAdapter::transport_description() const {
    return transport_ ? transport_->describe() : std::string("no transport");
}

}  // namespace homeguardian
