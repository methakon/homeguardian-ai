#include "ConsentGate.h"

#include <algorithm>

namespace homeguardian {

ConsentGate::ConsentGate(IConsentRepository& consent_repo, bool media_capture_enabled)
    : consent_repo_(consent_repo), media_capture_enabled_(media_capture_enabled) {}

GateDecision ConsentGate::check(const AcquisitionRequest& request,
                                std::chrono::system_clock::time_point now) const {
    GateDecision decision;

    // 1. Global kill switch. If media capture is disabled in configuration, no
    //    acquisition is authorized regardless of consent records.
    if (!media_capture_enabled_) {
        decision.reason = DenyReason::capture_disabled;
        decision.explanation = "media capture is disabled in configuration";
        return decision;
    }

    // 2. Look up every consent record for this subject and purpose. The latest
    //    active decision governs; anything else fails closed.
    auto records = consent_repo_.find_by_purpose(request.subject_scope, request.purpose);

    if (records.empty()) {
        decision.reason = DenyReason::no_consent_record;
        decision.explanation = "no consent record for subject+purpose";
        return decision;
    }

    // Sort by recorded_at ascending so the last element is the most recent.
    std::sort(records.begin(), records.end(),
              [](const ConsentRecord& a, const ConsentRecord& b) {
                  return a.get_recorded_at() < b.get_recorded_at();
              });

    // The latest decision for this subject+purpose is authoritative. A later
    // withdrawal or denial overrides any earlier grant.
    const ConsentRecord& latest = records.back();
    if (latest.get_decision() != ConsentDecision::granted) {
        decision.reason = DenyReason::decision_not_granted;
        decision.explanation = "latest decision is not granted (denied or withdrawn)";
        return decision;
    }

    // 3. The latest record is a grant: check it is still active (not expired).
    if (!latest.is_active(now)) {
        decision.reason = DenyReason::expired;
        decision.explanation = "grant has expired";
        return decision;
    }

    // 4. Ambiguity: if any OTHER active grant for the same purpose conflicts,
    //    fail closed rather than guess.
    int active_grants = 0;
    for (const auto& r : records) {
        if (r.get_decision() == ConsentDecision::granted && r.is_active(now)) {
            active_grants++;
        }
    }
    if (active_grants > 1) {
        decision.reason = DenyReason::ambiguous;
        decision.explanation = "multiple active grants for the same purpose";
        return decision;
    }

    // 5. Purpose already matched by the query. Verify the requested data
    //    category is actually authorized by the grant.
    const auto& cats = latest.get_data_categories();
    if (std::find(cats.begin(), cats.end(), request.data_category) == cats.end()) {
        decision.reason = DenyReason::category_missing;
        decision.explanation = "requested data category not authorized";
        return decision;
    }

    decision.authorized = true;
    decision.reason = DenyReason::none;
    decision.explanation = "authorized";
    return decision;
}

std::string ConsentGate::reason_to_string(DenyReason reason) {
    switch (reason) {
        case DenyReason::none: return "none";
        case DenyReason::capture_disabled: return "capture_disabled";
        case DenyReason::no_consent_record: return "no_consent_record";
        case DenyReason::decision_not_granted: return "decision_not_granted";
        case DenyReason::purpose_mismatch: return "purpose_mismatch";
        case DenyReason::category_missing: return "category_missing";
        case DenyReason::expired: return "expired";
        case DenyReason::ambiguous: return "ambiguous";
    }
    return "unknown";
}

} // namespace homeguardian
