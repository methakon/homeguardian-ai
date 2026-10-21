#include "core/EnvironmentalContext.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <sstream>

namespace homeguardian {
namespace {

constexpr std::size_t kMaxReasonChars = 160;
constexpr double kPositionCoarseDecimals = 1e2;  // 2 dp ~= 1.1 km at the equator

std::string bounded(std::string value, std::size_t max) {
    if (value.size() > max) value.resize(max);
    return value;
}

bool finite(double v) { return std::isfinite(v); }

// Authorization convention shared with EnvironmentalAdapter: a grant must be
// recorded in the context string. An absent/denied context is refused.
bool authorization_granted(const std::string& auth) {
    return !auth.empty() && auth.find("granted") != std::string::npos;
}

std::string json_escape(const std::string& in, std::size_t max) {
    std::string out;
    out.reserve(in.size());
    std::size_t n = 0;
    for (char c : in) {
        if (n++ >= max) break;
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) out += ' ';
                else out += c;
        }
    }
    return out;
}

std::string number(double v, int places) {
    if (!finite(v)) return "null";
    std::ostringstream os;
    os.setf(std::ios::fixed);
    os.precision(places);
    os << v;
    return os.str();
}

}  // namespace

// ---------------------------------------------------------------------------
// Taxonomy
// ---------------------------------------------------------------------------

const char* channel_class_to_string(ChannelClass c) {
    switch (c) {
        case ChannelClass::atmospheric:    return "atmospheric";
        case ChannelClass::illuminance:    return "illuminance";
        case ChannelClass::device_thermal: return "device_thermal";
        case ChannelClass::positional:     return "positional";
        case ChannelClass::kinematic:      return "kinematic";
        case ChannelClass::external:       return "external";
    }
    return "unknown";
}

const char* freshness_to_string(Freshness f) {
    switch (f) {
        case Freshness::fresh:   return "fresh";
        case Freshness::stale:   return "stale";
        case Freshness::expired: return "expired";
        case Freshness::unknown: return "unknown";
    }
    return "unknown";
}

// The registry. Plausibility ranges for the four metrics the engine already bounds
// are deliberately IDENTICAL to EnvironmentalBounds::plausible() so there is one
// definition of "physically possible", not two that can drift apart.
const std::vector<ChannelSpec>& channel_registry() {
    using namespace std::chrono;
    static const std::vector<ChannelSpec> kRegistry = {
        // channel                          class                        unit       min      max        max_age  auth  notes
        {"atmospheric.temperature",         ChannelClass::atmospheric,    "C",      -90.0,    70.0,      minutes(15), true,  "AIR temperature. The Mi A3 has no such sensor; supply from an external source."},
        {"atmospheric.humidity",            ChannelClass::atmospheric,    "%",        0.0,   100.0,      minutes(15), true,  "relative humidity; absent on the Mi A3"},
        {"atmospheric.pressure",            ChannelClass::atmospheric,    "hPa",    850.0,  1100.0,      minutes(15), true,  "barometric pressure; absent on the Mi A3"},
        {"external.weather.temperature",    ChannelClass::external,       "C",      -90.0,    70.0,      hours(2),    false, "weather service / Home Assistant, already a processed observation"},
        {"external.weather.humidity",       ChannelClass::external,       "%",        0.0,   100.0,      hours(2),    false, ""},
        {"external.weather.pressure",       ChannelClass::external,       "hPa",    850.0,  1100.0,      hours(2),    false, ""},
        {"external.terrain.elevation",      ChannelClass::external,       "m",     -500.0,  9000.0,      hours(24),   false, "terrain/elevation dataset; changes only if the device moves"},
        {"illuminance",                     ChannelClass::illuminance,    "lux",      0.0, 200000.0,      minutes(5),  false, "stk_stk3x3x ambient light - the ONE environmental metric this phone can measure"},
        {"device.battery.temperature",      ChannelClass::device_thermal, "C",      -20.0,    80.0,      minutes(2),  false, "THE HANDSET'S OWN CELL. Never an atmospheric reading."},
        {"device.soc.temperature",          ChannelClass::device_thermal, "C",      -20.0,   120.0,      minutes(2),  false, "SoC / thermal zone. Device health only."},
        {"position.latitude",               ChannelClass::positional,     "deg",    -90.0,    90.0,      minutes(10), true,  "GNSS. Off by default; requires authorization AND positional_enabled."},
        {"position.longitude",              ChannelClass::positional,     "deg",   -180.0,   180.0,      minutes(10), true,  "GNSS. Off by default; coarsened when enabled."},
        {"position.altitude",               ChannelClass::positional,     "m",     -500.0,  9000.0,      minutes(10), true,  "GNSS-derived or terrain-derived height"},
        {"motion.acceleration_magnitude",   ChannelClass::kinematic,      "m/s^2",    0.0,   200.0,      minutes(1),  false, "vector magnitude; orientation-invariant"},
        {"motion.orientation_change",       ChannelClass::kinematic,      "deg",      0.0,   360.0,      minutes(1),  false, "absolute change since previous sample"},
        {"motion.presence",                 ChannelClass::kinematic,      "bool",     0.0,     1.0,      minutes(5),  false, "1 = motion_detect/significant_motion fired, 0 = stationary_detect"},
    };
    return kRegistry;
}

const ChannelSpec* channel_spec(const std::string& channel) {
    for (const auto& spec : channel_registry()) {
        if (channel == spec.channel) return &spec;
    }
    return nullptr;
}

bool channel_supported(const std::string& channel) { return channel_spec(channel) != nullptr; }

bool is_atmospheric(const std::string& channel) {
    const ChannelSpec* s = channel_spec(channel);
    return s != nullptr && s->cls == ChannelClass::atmospheric;
}

bool is_device_thermal(const std::string& channel) {
    const ChannelSpec* s = channel_spec(channel);
    return s != nullptr && s->cls == ChannelClass::device_thermal;
}

// ---------------------------------------------------------------------------
// Validation
// ---------------------------------------------------------------------------

EnvironmentalContextEngine::IngestResult EnvironmentalContextEngine::validate(
    const EnvironmentalReading& r, std::chrono::system_clock::time_point now) const {
    auto fail = [](const std::string& why) {
        return IngestResult{false, bounded(why, kMaxReasonChars)};
    };

    if (r.channel.empty()) return fail("channel is empty");
    if (r.channel.size() > cfg_.max_channel_name_chars) return fail("channel name too long");

    const ChannelSpec* spec = channel_spec(r.channel);
    if (spec == nullptr) return fail("unsupported channel '" + r.channel + "'");

    if (r.source.empty()) return fail("source is empty");
    if (r.source.size() > cfg_.max_source_chars) return fail("source too long");
    if (r.note.size() > cfg_.max_note_chars) return fail("note too long");

    if (!finite(r.value)) return fail("value is not finite");
    if (r.value < spec->min_value || r.value > spec->max_value) {
        return fail("value implausible for '" + r.channel + "' (outside the declared physical range)");
    }

    if (!finite(r.quality)) return fail("quality is not finite");
    if (r.quality < 0.0 || r.quality > 1.0) return fail("quality outside [0,1]");

    if (!finite(r.uncertainty)) return fail("uncertainty is not finite");
    if (r.uncertainty < 0.0) return fail("uncertainty is negative");

    if (r.observed_at.time_since_epoch().count() == 0) return fail("missing observation timestamp");
    if (r.observed_at > now + cfg_.max_future_skew) return fail("observation timestamp is in the future");

    // Positional channels are doubly gated, and the gate is checked BEFORE the
    // authorization string so a caller cannot enable them by supplying a grant alone.
    if (spec->cls == ChannelClass::positional && !cfg_.positional_enabled) {
        return fail("positional channels are disabled in this configuration");
    }
    if (spec->requires_authorization && !authorization_granted(r.authorization)) {
        return fail("missing or non-granting authorization context for '" + r.channel + "'");
    }
    return IngestResult{true, ""};
}

EnvironmentalContextEngine::IngestResult EnvironmentalContextEngine::ingest(
    const EnvironmentalReading& reading, std::chrono::system_clock::time_point now) {
    IngestResult res = validate(reading, now);
    if (!res.accepted) return res;

    EnvironmentalReading stored = reading;
    // Coarsen position before anything is retained, so no precise coordinate is
    // ever stored. Applied on ingest, not on read.
    if (cfg_.coarse_position) {
        const ChannelSpec* spec = channel_spec(stored.channel);
        if (spec != nullptr && spec->cls == ChannelClass::positional) {
            stored.value = std::round(stored.value * kPositionCoarseDecimals) / kPositionCoarseDecimals;
        }
    }
    stored.channel = bounded(stored.channel, cfg_.max_channel_name_chars);
    stored.source = bounded(stored.source, cfg_.max_source_chars);
    stored.note = bounded(stored.note, cfg_.max_note_chars);

    auto& bucket = by_channel_[stored.channel];
    bucket.push_back(std::move(stored));
    // Bounded: keep the newest, drop the oldest.
    if (bucket.size() > cfg_.max_readings_per_channel) {
        bucket.erase(bucket.begin(), bucket.begin() + static_cast<std::ptrdiff_t>(
                                        bucket.size() - cfg_.max_readings_per_channel));
    }
    return res;
}

std::size_t EnvironmentalContextEngine::reading_count() const {
    std::size_t n = 0;
    for (const auto& [channel, bucket] : by_channel_) {
        (void)channel;
        n += bucket.size();
    }
    return n;
}

std::size_t EnvironmentalContextEngine::channel_count() const { return by_channel_.size(); }

void EnvironmentalContextEngine::clear() { by_channel_.clear(); }

// ---------------------------------------------------------------------------
// Fusion
// ---------------------------------------------------------------------------

ContextSnapshot EnvironmentalContextEngine::snapshot(std::chrono::system_clock::time_point now) const {
    ContextSnapshot out;
    out.at = now;

    for (const auto& [channel, bucket] : by_channel_) {
        const ChannelSpec* spec = channel_spec(channel);
        if (spec == nullptr) continue;

        // Freshness is COMPUTED, never asserted by the caller.
        std::vector<const EnvironmentalReading*> fresh;
        std::chrono::seconds newest_age = std::chrono::seconds::max();
        for (const auto& r : bucket) {
            if (r.observed_at > now) {
                // Clock skew, or a reading stamped in the future within the allowed
                // tolerance: treat age as zero rather than negative.
                fresh.push_back(&r);
                if (newest_age > std::chrono::seconds(0)) newest_age = std::chrono::seconds(0);
                continue;
            }
            auto age = std::chrono::duration_cast<std::chrono::seconds>(now - r.observed_at);
            if (age <= spec->max_age) {
                fresh.push_back(&r);
                if (age < newest_age) newest_age = age;
            }
        }

        FusedChannel fc;
        fc.channel = channel;
        fc.cls = spec->cls;

        if (fresh.empty()) {
            // Present-but-stale is reported as expired, not silently dropped.
            fc.present = false;
            fc.freshness = Freshness::expired;
            fc.age = newest_age == std::chrono::seconds::max() ? std::chrono::seconds(0) : newest_age;
            out.channels.push_back(std::move(fc));
            continue;
        }

        bool all_declared = true;
        for (const auto* r : fresh) {
            if (!r->uncertainty_declared || r->uncertainty <= 0.0) { all_declared = false; break; }
        }

        double value = 0.0;
        double uncertainty = 0.0;
        double quality = 0.0;

        if (all_declared) {
            // Inverse-variance weighted mean: the estimator that actually uses the
            // uncertainty we insisted on collecting, instead of averaging it away.
            double wsum = 0.0, vsum = 0.0, invvar = 0.0;
            for (const auto* r : fresh) {
                const double w = 1.0 / (r->uncertainty * r->uncertainty);
                wsum += w;
                vsum += w * r->value;
                invvar += w;
                quality += r->quality;
            }
            value = (wsum > 0.0) ? vsum / wsum : fresh.front()->value;
            uncertainty = (invvar > 0.0) ? std::sqrt(1.0 / invvar) : 0.0;
        } else {
            // Mixed/undeclared uncertainties: plain mean, and the fused uncertainty is
            // only as good as the worst declared input. Never invent a precision.
            double sum = 0.0;
            for (const auto* r : fresh) { sum += r->value; quality += r->quality; }
            value = sum / static_cast<double>(fresh.size());
            uncertainty = 0.0;
            for (const auto* r : fresh) {
                if (r->uncertainty_declared) uncertainty = std::max(uncertainty, r->uncertainty);
            }
        }

        fc.present = true;
        fc.value = value;
        fc.uncertainty = uncertainty;
        fc.uncertainty_declared = all_declared;
        fc.quality = quality / static_cast<double>(fresh.size());
        fc.age = newest_age == std::chrono::seconds::max() ? std::chrono::seconds(0) : newest_age;
        fc.freshness = (fc.age <= spec->max_age) ? Freshness::fresh : Freshness::stale;
        fc.contributors = fresh.size();
        // Newest contributor decides the reported provenance/source.
        const EnvironmentalReading* newest = fresh.front();
        for (const auto* r : fresh) {
            if (r->observed_at > newest->observed_at) newest = r;
        }
        fc.provenance = newest->provenance;
        fc.source = newest->source;

        out.channels.push_back(std::move(fc));
    }

    std::sort(out.channels.begin(), out.channels.end(),
              [](const FusedChannel& a, const FusedChannel& b) { return a.channel < b.channel; });

    // Daylight needs a threshold; without one that was injected WITH a stated source
    // we refuse to answer rather than pick a number.
    if (cfg_.daylight_lux_threshold.has_value() && !cfg_.daylight_threshold_source.empty()) {
        const FusedChannel* lux = out.find("illuminance");
        if (lux != nullptr && lux->present) {
            out.daylight_flag = (lux->value >= *cfg_.daylight_lux_threshold);
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
// Snapshot accessors - the atmospheric/device separation lives here
// ---------------------------------------------------------------------------

const FusedChannel* ContextSnapshot::find(const std::string& channel) const {
    for (const auto& c : channels) {
        if (c.channel == channel) return &c;
    }
    return nullptr;
}

bool ContextSnapshot::has(const std::string& channel) const {
    const FusedChannel* c = find(channel);
    return c != nullptr && c->present;
}

bool ContextSnapshot::atmospheric_series_available() const {
    for (const auto& c : channels) {
        if (c.cls == ChannelClass::atmospheric && c.present) return true;
    }
    return false;
}

std::optional<double> ContextSnapshot::atmospheric_temperature() const {
    const FusedChannel* c = find("atmospheric.temperature");
    if (c == nullptr || !c->present) return std::nullopt;
    return c->value;
}

std::optional<double> ContextSnapshot::battery_temperature() const {
    const FusedChannel* c = find("device.battery.temperature");
    if (c == nullptr || !c->present) return std::nullopt;
    return c->value;
}

std::string ContextSnapshot::to_json() const {
    std::ostringstream os;
    os << "{\"at\":" << std::chrono::duration_cast<std::chrono::seconds>(
                            at.time_since_epoch()).count();
    os << ",\"atmospheric_series_available\":"
       << (atmospheric_series_available() ? "true" : "false");
    auto at = atmospheric_temperature();
    os << ",\"atmospheric_temperature\":" << (at ? number(*at, 2) : "null");
    auto bt = battery_temperature();
    os << ",\"battery_temperature\":" << (bt ? number(*bt, 2) : "null");
    os << ",\"channels\":[";
    bool first = true;
    for (const auto& c : channels) {
        if (!first) os << ",";
        first = false;
        os << "{\"channel\":\"" << json_escape(c.channel, 64) << "\""
           << ",\"class\":\"" << channel_class_to_string(c.cls) << "\""
           << ",\"present\":" << (c.present ? "true" : "false")
           << ",\"value\":" << number(c.value, 3)
           << ",\"uncertainty\":" << number(c.uncertainty, 3)
           << ",\"uncertainty_declared\":" << (c.uncertainty_declared ? "true" : "false")
           << ",\"quality\":" << number(c.quality, 3)
           << ",\"freshness\":\"" << freshness_to_string(c.freshness) << "\""
           << ",\"age_s\":" << c.age.count()
           << ",\"contributors\":" << c.contributors
           << ",\"provenance\":\"" << provenance_to_string(c.provenance) << "\""
           << ",\"source\":\"" << json_escape(c.source, 96) << "\"}";
    }
    os << "]}";
    return os.str();
}

// ---------------------------------------------------------------------------
// Thermal health - only ever from INJECTED limits
// ---------------------------------------------------------------------------

EnvironmentalContextEngine::ThermalHealth EnvironmentalContextEngine::thermal_health(
    std::chrono::system_clock::time_point now) const {
    // A health verdict without a stated source of limits would be an invented
    // threshold, so refuse to give one.
    if (!cfg_.thermal_warn_c.has_value() || !cfg_.thermal_critical_c.has_value()) {
        return ThermalHealth::unknown;
    }
    if (cfg_.thermal_limits_source.empty()) return ThermalHealth::unknown;
    if (*cfg_.thermal_warn_c >= *cfg_.thermal_critical_c) return ThermalHealth::unknown;

    ContextSnapshot snap = snapshot(now);
    // Prefer the battery: it is the component whose safety actually matters when the
    // device is plugged in and running unattended.
    std::optional<double> temp = snap.battery_temperature();
    if (!temp.has_value()) {
        const FusedChannel* soc = snap.find("device.soc.temperature");
        if (soc != nullptr && soc->present) temp = soc->value;
    }
    if (!temp.has_value()) return ThermalHealth::unknown;

    if (*temp >= *cfg_.thermal_critical_c) return ThermalHealth::hot;
    if (*temp >= *cfg_.thermal_warn_c) return ThermalHealth::warm;
    return ThermalHealth::nominal;
}

// ---------------------------------------------------------------------------
// Correlation with behavioural detections
// ---------------------------------------------------------------------------

const std::vector<std::string>& correlation_status_vocabulary() {
    static const std::vector<std::string> kVocab = {
        "environmental_correlation",
        "insufficient_evidence",
    };
    return kVocab;
}

EnvironmentalCorrelationResult correlate_change_with_environment(
    const ContextSnapshot& snapshot, const std::string& channel,
    std::chrono::system_clock::time_point change_at, std::chrono::seconds max_lag) {
    EnvironmentalCorrelationResult result;
    result.channel = channel;

    const FusedChannel* c = snapshot.find(channel);
    if (c == nullptr || !c->present) {
        result.status = "insufficient_evidence";
        result.reason = "no fresh reading for channel '" + bounded(channel, 64) + "'";
        return result;
    }

    // The reading is the newest contributor's time, so reconstruct it from age.
    auto reading_at = snapshot.at - c->age;
    auto lag = std::chrono::duration_cast<std::chrono::seconds>(reading_at - change_at);
    result.value = c->value;
    result.uncertainty = c->uncertainty;
    result.lag_seconds = lag.count();

    if (lag.count() > max_lag.count() || lag.count() < -max_lag.count()) {
        result.status = "insufficient_evidence";
        result.reason = "reading and change are further apart than the accepted lag";
        return result;
    }

    result.correlated = true;
    result.status = "environmental_correlation";
    result.change_count = 1;
    // Deliberately worded: association within a window, not causation, not forecast.
    result.reason = "environmental reading moved within the accepted window of the change";
    return result;
}

}  // namespace homeguardian
