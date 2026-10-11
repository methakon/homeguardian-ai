#include "RuntimeStatus.h"

#include <sstream>

namespace homeguardian {

void RuntimeStatus::reset_counters() {
    media_authorized = 0;
    media_denied = 0;
    events_processed = 0;
    events_rejected = 0;
    events_failed = 0;
    alerts_generated = 0;
    alerts_delivered = 0;
    alerts_suppressed = 0;
    alerts_persist_failed = 0;
    speaker_queue_depth = 0;
    speaker_queue_overflow = 0;
    speaker_played = 0;
    recoveries = 0;
    // app_state, capture_enabled, persistence_healthy, startups,
    // clean_shutdowns are lifecycle facts, not per-cycle counters; left as-is.
}

std::string RuntimeStatus::summary() const {
    std::ostringstream os;
    os << "state=" << app_state_to_string(app_state.load())
       << " capture=" << (capture_enabled.load() ? "on" : "off")
       << " persist=" << (persistence_healthy.load() ? "ok" : "DEGRADED")
       << " media_auth=" << media_authorized.load()
       << " media_denied=" << media_denied.load()
       << " events_proc=" << events_processed.load()
       << " events_rej=" << events_rejected.load()
       << " events_fail=" << events_failed.load()
       << " alerts_gen=" << alerts_generated.load()
       << " alerts_deliv=" << alerts_delivered.load()
       << " alerts_supp=" << alerts_suppressed.load()
       << " persist_fail=" << alerts_persist_failed.load()
       << " spk_depth=" << speaker_queue_depth.load()
       << " spk_overflow=" << speaker_queue_overflow.load()
       << " spk_played=" << speaker_played.load()
       << " startups=" << startups.load()
       << " clean_shutdowns=" << clean_shutdowns.load()
       << " recoveries=" << recoveries.load();
    return os.str();
}

}  // namespace homeguardian
