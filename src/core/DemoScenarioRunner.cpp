#include "core/DemoScenarioRunner.h"

#include "core/AnomalyMonitor.h"
#include "core/Application.h"
#include "core/FusionEngine.h"
#include "core/Observation.h"
#include "core/PrivacyAuditLog.h"
#include "core/PersistenceAudit.h"
#include "core/RuntimeStatus.h"
#include "core/SimulatorRuntime.h"
#include "core/SyntheticActivityDetector.h"

#include <exception>
#include <string>

namespace homeguardian {

ScenarioRunResult run_demo_scenario(Application& app, const DemoScenario& spec) {
    ScenarioRunResult r;
    r.scenario_id = spec.id;
    r.title = spec.title;
    r.description = spec.description;
    r.expectation = spec.expectation;

    SimulatorRuntime& sim = app.get_simulator_runtime();
    RuntimeStatus& st = app.get_status();

    const bool capture_on = st.capture_enabled.load();
    r.capture_enabled = capture_on;

    // Fresh-run semantics for a single long-lived process: reset the per-run
    // counters/state only (never the config kill switch, consent grants, or the
    // durable audit DB). Mirrors the previous demo's per-process behaviour.
    st.reset_counters();
    app.get_alert_policy().reset();
    app.get_pipeline().clear_history();
    sim.reset_for_reuse();
    // Clear consent state so this scenario is isolated from any previous run in
    // the same long-lived process (a prior run's grants/withdrawals must not
    // leak into this one). Deterministic per-scenario results depend on this.
    sim.reset_consent_state();

    if (!capture_on) {
        // Fail-closed: capture disabled means nothing is delivered, so no
        // observation, detection, or alert can occur.
        r.cycles = 0;
        return r;
    }

    // Enable capture on the simulator's OWN gate and reflect it in
    // RuntimeStatus (Application constructs it disabled by default).
    sim.set_media_capture_enabled(true);
    st.capture_enabled.store(true);

    // Synthetic consent for the scenario (simulator test consent only).
    if (spec.grant_enabled) {
        sim.grant_consent(SimulatorRuntime::enabled_device_id(), "sim_alexa_enabled");
    }
    if (spec.grant_assistant) {
        sim.grant_consent(SimulatorRuntime::assistant_device_id(), "sim_alexa_assistant");
    }
    if (spec.withdraw_assistant) {
        // The withdrawal is the latest decision, so the gate denies the
        // assistant's subsequent acquisition (fail closed).
        sim.withdraw_consent(SimulatorRuntime::assistant_device_id(), "sim_alexa_assistant");
    }

    // Capture the REAL delivered alerts from the existing pipeline. (The bridge
    // persists exactly these; nothing is fabricated.)
    app.set_alert_sink([&r](const Alert& a) { r.delivered_alerts.push_back(a); });

    // Route every AUTHORIZED synthetic observation through the real application
    // event path. Denied/withdrawn consumers never reach the sink.
    sim.set_event_sink([&app](const Event& ev) { app.process_event(ev); });

    // H-03 privacy audit: record every per-consumer delivery decision.
    PrivacyAuditLog privacy;
    sim.set_privacy_audit(&privacy);

    bool started = true;
    std::string err;
    try {
        sim.start();
    } catch (const std::exception& e) {
        started = false;
        err = e.what();
    }

    // --- Detection is DRIVEN BY AUTHORIZED ACQUISITIONS (L-11) ---
    // Each tick that actually delivered media (i.e. at least one consumer was
    // authorized) runs ONE detection step on the shared synthetic payload the
    // consumers received. The resulting inference Observation flows through the
    // SAME pipeline, where the detection/fusion rule decides whether to alert.
    //
    // Consequences that the tests assert: with capture off, with consent
    // withdrawn, or with no authorized consumer, no media is delivered, so no
    // detection step runs and no detection-driven alert can exist.
    SyntheticActivityDetector detector;
    AnomalyMonitor anomaly;                  // L-12 statistical anomaly stage
    std::vector<Observation> observations;   // all detection evidence this run
    // Anomaly sample times are synthetic and deliberately in the PAST, so the
    // events built from them are never rejected as "timestamp in the future".
    const auto anomaly_base =
        std::chrono::system_clock::now() -
        std::chrono::seconds(static_cast<int>(spec.cycles < 0 ? 0 : spec.cycles) + 2);
    int anomaly_observations = 0;
    if (started) {
        const int cycles = spec.cycles < 0 ? 0 : spec.cycles;
        for (int i = 0; i < cycles; ++i) {
            const int delivered = sim.tick();
            st.media_authorized.fetch_add(static_cast<uint64_t>(delivered));
            if (delivered <= 0) continue;            // no acquisition -> no detection
            if (spec.detection_energies.empty()) continue;
            const double energy = spec.detection_energies[
                static_cast<size_t>(i) % spec.detection_energies.size()];
            if (spec.run_detection) {
                Observation o = detector.detect("det-obs-" + std::to_string(i), energy);
                o.validate();
                observations.push_back(o);
                switch (o.status) {
                    case DetectorStatus::detected: ++r.detection.detected; break;
                    case DetectorStatus::not_detected: ++r.detection.not_detected; break;
                    default: ++r.detection.inconclusive; break;  // inconclusive/unavailable
                }
                app.process_event(o.to_event());
            }
            if (spec.run_anomaly) {
                // The SAME bounded numeric feature the activity detector sees, but
                // judged against a LEARNED baseline instead of a fixed threshold.
                const auto sample_time =
                    anomaly_base + std::chrono::seconds(i);
                const AnomalyResult ar = anomaly.observe(energy, sample_time);
                Observation ao = anomaly.to_observation(
                    ar, "anom-obs-" + std::to_string(i),
                    std::chrono::system_clock::now(), anomaly.config().min_samples);
                observations.push_back(ao);
                ++anomaly_observations;
                app.process_event(ao.to_event());
            }
        }
        if (spec.run_anomaly && spec.anomaly_missing_probe) {
            // Deliberate gap: advance the synthetic clock past max_gap and ask
            // the detector whether the feature has gone quiet.
            const auto gap_time = anomaly_base +
                std::chrono::seconds(static_cast<int>(spec.cycles < 0 ? 0 : spec.cycles) - 1) +
                anomaly.config().max_gap + std::chrono::seconds(1);
            const AnomalyResult mr = anomaly.check_missing(gap_time);
            Observation mo = anomaly.to_observation(
                mr, "anom-missing-probe", std::chrono::system_clock::now(),
                anomaly.config().min_samples);
            observations.push_back(mo);
            ++anomaly_observations;
            app.process_event(mo.to_event());
        }
    }
    sim.shutdown();

    // Detach observers so a later run (or the main lifecycle) is unaffected.
    sim.set_privacy_audit(nullptr);
    sim.set_event_sink(nullptr);
    app.set_alert_sink(nullptr);

    if (!started && !err.empty()) {
        r.error = err;
    }

    // Detection/fusion summary for display. These are the SAME observations the
    // detection/fusion rule evaluated (all within the pipeline window), so the
    // outcome shown to the user is the one that caused (or did not cause) the
    // alert.
    r.evidence_detectors = app.evidence_detector_ids();
    // Only the detectors the rule accepts are evidence; the displayed fusion
    // therefore uses the same allowlist, so what the user sees is what the rule
    // evaluated.
    std::vector<Observation> evidence;
    for (const auto& o : observations) {
        if (r.evidence_detectors.empty()) { evidence.push_back(o); continue; }
        for (const auto& id : r.evidence_detectors) {
            if (id == o.detector_id) { evidence.push_back(o); break; }
        }
    }
    if (!evidence.empty()) {
        FusionEngine fusion;
        const FusionResult fr = fusion.fuse(evidence);
        r.detection.ran = true;
        r.detection.detector_id = detector.config().detector_id;
        r.detection.model_id = detector.config().model_id;
        r.detection.provenance = provenance_to_string(Provenance::synthetic);
        r.detection.observations = evidence.size();
        r.detection.fusion_verdict = fusion_verdict_to_string(fr.verdict);
        r.detection.fusion_confidence = fr.confidence;
        r.detection.fusion_contributing = fr.contributing;
        r.detection.fusion_reason = fr.reason;
    }

    if (spec.run_anomaly) {
        r.anomaly = anomaly.snapshot();
        r.anomaly.ran = true;
    }

    // --- Counters + privacy audit ---
    r.cycles = spec.cycles;
    r.media_authorized = st.media_authorized.load();
    r.events_processed = st.events_processed.load();
    r.alerts_generated = st.alerts_generated.load();
    r.alerts_delivered = st.alerts_delivered.load();
    r.alerts_suppressed = st.alerts_suppressed.load();

    {
        const auto c = privacy.counts();
        r.privacy.entries = c.total;
        r.privacy.authorized = c.authorized;
        r.privacy.denied = c.denied;
        r.privacy.acquired = c.acquired;
        r.privacy.enforcement_ok = privacy.verify_enforcement();
        r.privacy.capacity = privacy.capacity();
    }
    r.denied_acquisitions = static_cast<uint64_t>(r.privacy.denied);
    // Reflect the real denial count in the status counter (accurate, bounded).
    st.media_denied.store(static_cast<uint64_t>(r.privacy.denied));

    return r;
}

size_t persist_run_alerts(PersistenceAudit& audit, const ScenarioRunResult& r) {
    size_t written = 0;
    for (const auto& a : r.delivered_alerts) {
        if (audit.record_alert(a)) ++written;
    }
    return written;
}

}  // namespace homeguardian
