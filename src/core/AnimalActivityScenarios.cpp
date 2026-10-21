#include "core/AnimalActivityScenarios.h"

namespace homeguardian {

const std::vector<DemoScenario>& animal_activity_scenarios() {
    static const std::vector<DemoScenario> kScenarios = [] {
        std::vector<DemoScenario> v;

        // 1) A quiet night. The value of this scenario is what does NOT happen: a low, flat
        //    energy sequence must produce no activity observation and no alert. A detector that
        //    alerts on silence is worse than no detector.
        {
            DemoScenario s;
            s.id = "animal-activity-quiet";
            s.title = "Animal activity - quiet baseline";
            s.description =
                "Low, flat synthetic activity energy for the whole run: the detector reports "
                "not_detected and no alert is raised.";
            s.cycles = 5;
            s.grant_enabled = true;
            s.grant_assistant = true;
            s.run_detection = true;
            s.detection_energies = {0.05, 0.04, 0.06, 0.05, 0.05};
            s.expectation =
                "no alert: not_detected throughout, so there is no evidence for the correlation "
                "or alert policy to act on. A quiet window stays quiet.";
            v.push_back(std::move(s));
        }

        // 2) Sustained activity above baseline. The core demonstration: an activity-level change
        //    is detected, fusion agrees, and the resulting observation flows through the real
        //    pipeline to a delivered alert.
        {
            DemoScenario s;
            s.id = "animal-activity-sustained";
            s.title = "Animal activity - sustained change";
            s.description =
                "Sustained synthetic activity above the learned baseline for every tick: the "
                "detector and the fusion stage agree and the alert comes from that evidence.";
            s.cycles = 5;
            s.grant_enabled = true;
            s.grant_assistant = true;
            s.run_detection = true;
            s.detection_energies = {0.95, 0.90, 0.88, 0.92, 0.85};
            s.expectation =
                "detection-driven alert: every observation is detected, fusion confidence is "
                "1.00, and the alert severity follows that confidence. This is an ACTIVITY "
                "CHANGE, not a species and not a prediction.";
            v.push_back(std::move(s));
        }

        // 3) A single isolated spike. This is the scenario that keeps the detector honest: the
        //    confirmation rule requires consecutive windows above threshold, so one anomalous
        //    sample among quiet ones must NOT raise an alert. Pairs with the quiet scenario.
        {
            DemoScenario s;
            s.id = "animal-activity-single-spike";
            s.title = "Animal activity - one isolated spike";
            s.description =
                "A single high-energy sample surrounded by baseline samples: the confirmation "
                "window requirement means the spike is not accepted as a sustained change.";
            s.cycles = 5;
            s.grant_enabled = true;
            s.grant_assistant = true;
            s.run_detection = true;
            s.detection_energies = {0.05, 0.05, 0.97, 0.05, 0.05};
            s.expectation =
                "no alert, or at most an inconclusive one: one anomalous window is a door "
                "slamming, not an animal settling in. Demonstrates why smoothing precedes "
                "thresholding.";
            v.push_back(std::move(s));
        }

        // 4) Ambiguous evidence. Borderline energies must stay inconclusive: an uncertain result
        //    is never promoted to a confirmed alert, and "we could not decide" must remain
        //    distinguishable from "we looked and found nothing".
        {
            DemoScenario s;
            s.id = "animal-activity-inconclusive";
            s.title = "Animal activity - inconclusive evidence";
            s.description =
                "Synthetic energy sitting near the detector threshold for every tick: the "
                "observations are inconclusive and fusion refuses a verdict.";
            s.cycles = 3;
            s.grant_enabled = true;
            s.grant_assistant = true;
            s.run_detection = true;
            s.detection_energies = {0.52, 0.49, 0.51};
            s.expectation =
                "no alert: uncertain evidence is recorded as uncertain and stops there. The "
                "inconclusive state is the honest output, not a failure to be smoothed away.";
            v.push_back(std::move(s));
        }

        // 5) The same sustained activity, with synthetic consent withheld. Consent is a
        //    precondition for acquisition, not a setting consulted later: with no consent there
        //    are no authorized acquisitions, therefore no observations and no alerts, even
        //    though the activity is present in the fixture.
        {
            DemoScenario s;
            s.id = "animal-activity-no-consent";
            s.title = "Animal activity - consent withheld";
            s.description =
                "Identical sustained activity to the sustained-change scenario, but synthetic "
                "consent is withheld and withdrawn: acquisition is denied before detection.";
            s.cycles = 5;
            s.grant_enabled = false;
            s.grant_assistant = false;
            s.withdraw_assistant = true;
            s.run_detection = true;
            s.detection_energies = {0.95, 0.90, 0.88, 0.92, 0.85};
            s.expectation =
                "no alert and no authorized media: activity that was never authorized to be "
                "observed is never presented to the detectors, so nothing is processed and "
                "no alert can arise. Consent precedes detection.";
            v.push_back(std::move(s));
        }

        return v;
    }();
    return kScenarios;
}

const DemoScenario* find_animal_activity_scenario(std::string_view id) {
    for (const auto& s : animal_activity_scenarios()) {
        if (s.id == id) return &s;
    }
    return nullptr;
}

}  // namespace homeguardian
