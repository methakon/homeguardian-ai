// Host tests for Tier0Gate — the event trigger / pre-filter.
//
// Hermetic: no camera, no network, no gateway, no clock. The gate is driven by real CameraFinding
// values, so these tests exercise the same types the analyzer uses.
#include "edge/Tier0Gate.h"

#include <chrono>
#include <cstdio>
#include <string>

using namespace hg::edge;
using homeguardian::CameraFinding;
using homeguardian::CameraFindingKind;
using Clock = std::chrono::system_clock;
using ms = std::chrono::milliseconds;

static int fails = 0, checks = 0;
static void ck(bool c, const char* what) {
    ++checks;
    if (c) { std::printf("  ok    %s\n", what); }
    else   { ++fails; std::printf("  FAIL  %s\n", what); }
}

static Clock::time_point T(ms t) { return Clock::time_point{} + t; }

// A finding shaped exactly like the host's. The idle band is the value measured on the real
// workstation camera, so the relative thresholds are exercised rather than absolute fallbacks.
static constexpr double kBanded = 0.0098;

static CameraFinding mk(const char* character, double motion, ms /*t*/, bool character_valid = true,
                        double idle_band = kBanded, std::size_t travel = 3,
                        CameraFindingKind kind = CameraFindingKind::quiet_baseline) {
    CameraFinding f;
    f.valid = true;
    f.kind = kind;
    f.real_source = true;
    f.motion_level = motion;
    f.character_valid = character_valid;
    f.change_character = character;
    f.idle_band = idle_band;
    f.region_travel = 0.02;
    f.travel_consistent = travel;
    return f;
}

int main() {
    std::printf("== brightness is not motion (the host's own vocabulary) ==\n");
    {
        Tier0Gate g;
        auto e1 = g.update(mk("global_illumination", 0.30, ms(0)), T(ms(0)));
        ck(!e1.fire, "a global brightness step does not fire");
        auto e2 = g.update(mk("illumination_settling", 0.10, ms(100)), T(ms(100)));
        ck(!e2.fire, "the camera's own settling tail after it does not fire");
        auto e3 = g.update(mk("localized_unipolar", 0.25, ms(200)), T(ms(200)));
        ck(!e3.fire, "a compact UNIPOLAR change (a light on in the scene) does not fire");
        ck(g.stats().rejected_illumination == 2, "both illumination classes are counted");
        ck(g.stats().rejected_unipolar == 1, "and the unipolar rejection is counted");
    }

    std::printf("== one strong observation fires immediately ==\n");
    {
        Tier0Gate g;
        // strong threshold = 6 x 0.0098 = 0.0588
        auto e = g.update(mk("localized_motion", 0.09, ms(0)), T(ms(0)));
        ck(e.fire, "one strong localized_motion observation fires on its own");
        ck(e.state == GateState::active, "state becomes active");
        ck(e.anchor == T(ms(0)), "the anchor is this observation");
        ck(e.was_strong, "and it is recorded as the strong path, not K-of-N");
        ck(e.event_id != 0, "a non-zero event id is emitted");
    }

    std::printf("== weak motion needs 2 of the last 3 OBSERVATIONS ==\n");
    {
        Tier0Gate g;
        // weak threshold = 2 x 0.0098 = 0.0196
        auto e1 = g.update(mk("localized_motion", 0.025, ms(0)), T(ms(0)));
        ck(!e1.fire, "a single weak observation does not fire");
        ck(e1.state == GateState::candidate, "it opens a candidate");

        auto e2 = g.update(mk("quiet", 0.0, ms(70)), T(ms(70)));
        ck(!e2.fire, "an intervening quiet observation does not fire");
        ck(e2.state == GateState::candidate,
           "and does NOT cancel the candidate - a miss takes a slot, it does not clear the window");

        auto e3 = g.update(mk("localized_motion", 0.025, ms(140)), T(ms(140)));
        ck(e3.fire, "two weak hits within three observations fire");
        ck(e3.anchor == T(ms(0)),
           "ANCHOR IS THE FIRST WEAK HIT (0 ms), not the confirming observation (140 ms)");
        ck(!e3.was_strong, "and it is recorded as the K-of-N path");
    }

    std::printf("== a lone weak hit does not latch forever ==\n");
    {
        Tier0Gate g;
        g.update(mk("localized_motion", 0.025, ms(0)), T(ms(0)));
        auto e = g.update(mk("quiet", 0.0, ms(9000)), T(ms(9000)));   // past candidate_window
        ck(e.state == GateState::quiet, "the candidate expires on its own timeout");
    }

    std::printf("== diffuse motion (rain, foliage) must clear a higher bar ==\n");
    {
        Tier0Gate g;
        auto e = g.update(mk("diffuse_motion", 0.05, ms(0), true, kBanded, /*travel=*/0), T(ms(0)));
        ck(!e.fire && e.state == GateState::quiet,
           "diffuse_motion without travel consistency is suppressed entirely");

        GateConfig cfg; cfg.diffuse_requires_travel = false;
        Tier0Gate g2(cfg);
        auto e2 = g2.update(mk("diffuse_motion", 0.05, ms(0), true, kBanded, 0), T(ms(0)));
        ck(e2.state == GateState::candidate,
           "with the guard off the same input reaches candidate - so the guard is what suppressed it");
    }

    std::printf("== a stalled window is not a quiet scene ==\n");
    {
        Tier0Gate g;
        CameraFinding s;
        s.valid = true;
        s.kind = CameraFindingKind::frames_stalled;
        s.reason = "timeout";
        auto e = g.update(s, T(ms(0)));
        ck(!e.fire, "a stalled window never fires");
        ck(e.reason.find("stalled") != std::string::npos, "and says so");
        ck(g.stats().stalled_windows == 1, "counted separately from ordinary non-motion");
        ck(g.stats().observations == 0, "and it is not counted as an observation of the world");
    }

    std::printf("== an uncomputed character can never drive a trigger ==\n");
    {
        Tier0Gate g;
        auto e = g.update(mk("localized_motion", 0.50, ms(0), /*character_valid=*/false), T(ms(0)));
        ck(!e.fire, "a high motion_level with no valid character does NOT fire");
    }

    std::printf("== an unrecognised character is not motion ==\n");
    {
        Tier0Gate g;
        auto e = g.update(mk("something_new_from_a_future_version", 0.50, ms(0)), T(ms(0)));
        ck(!e.fire, "an unknown character string is treated as non-motion, not as an event");
        ck(change_character_from_string("localized_motion") == ChangeCharacter::localized_motion,
           "and the parser does resolve the real vocabulary");
        ck(change_character_from_string("") == ChangeCharacter::unknown, "empty maps to unknown");
    }

    std::printf("== one continuous movement is ONE event ==\n");
    {
        Tier0Gate g;
        auto f1 = g.update(mk("localized_motion", 0.09, ms(0)), T(ms(0)));
        auto f2 = g.update(mk("localized_motion", 0.09, ms(300)), T(ms(300)));
        ck(f1.fire, "the first observation fires");
        ck(f2.extend && !f2.fire, "a later strong observation EXTENDS rather than firing again");
        ck(f2.event_id == f1.event_id, "and it keeps the SAME event id - no duplicate event");
    }

    std::printf("== the latch holds, then releases to cooldown ==\n");
    {
        Tier0Gate g;
        g.update(mk("localized_motion", 0.09, ms(0)), T(ms(0)));
        auto e1 = g.update(mk("quiet", 0.0, ms(500)), T(ms(500)));
        ck(e1.extend, "a quiet observation inside the latch keeps the event open");
        ck(e1.event_id != 0, "still the same event");
        auto e2 = g.update(mk("quiet", 0.0, ms(1500)), T(ms(1500)));
        ck(e2.state == GateState::cooldown, "past the latch it releases to cooldown");
        ck(!e2.fire, "and does not fire");
    }

    std::printf("== a new event after cooldown gets a NEW id ==\n");
    {
        Tier0Gate g;
        auto a = g.update(mk("localized_motion", 0.09, ms(0)), T(ms(0)));
        g.update(mk("quiet", 0.0, ms(1500)), T(ms(1500)));    // -> cooldown
        g.update(mk("quiet", 0.0, ms(4000)), T(ms(4000)));    // cooldown expires
        auto b = g.update(mk("localized_motion", 0.09, ms(5000)), T(ms(5000)));
        ck(b.fire, "a later movement fires a new event");
        ck(b.event_id != a.event_id, "with a DIFFERENT id, so the two are not conflated");
    }

    std::printf("== never fires while warming up, and never on an invalid finding ==\n");
    {
        Tier0Gate g;
        auto w = g.update(mk("localized_motion", 0.50, ms(0), true, kBanded, 3,
                             CameraFindingKind::warming_up), T(ms(0)));
        ck(!w.fire, "no baseline yet means no trigger");
        CameraFinding inv;  inv.valid = false;
        auto i = g.update(inv, T(ms(100)));
        ck(!i.fire, "an invalid finding does not fire");
        ck(g.stats().invalid_findings == 1, "and is counted as invalid, not as quiet");
    }

    std::printf("== absolute thresholds apply ONLY before the band is calibrated ==\n");
    {
        // With idle_band == 0 the session has not calibrated, so the fallbacks apply:
        // strong 0.02, weak 0.004. 0.025 would be STRONG and fire at once - which is why the
        // weak-only case has to sit strictly between the two floors.
        Tier0Gate g;
        auto e = g.update(mk("localized_motion", 0.010, ms(0), true, /*idle_band=*/0.0), T(ms(0)));
        ck(!e.fire, "0.010 is weak-only against the uncalibrated floors, so it does not fire alone");
        ck(e.state == GateState::candidate, "it opens a candidate instead");

        Tier0Gate g2;
        auto e2 = g2.update(mk("localized_motion", 0.001, ms(0), true, /*idle_band=*/0.0), T(ms(0)));
        ck(!e2.fire && e2.state == GateState::quiet,
           "below the uncalibrated weak floor (0.004) nothing opens at all");

        Tier0Gate g3;
        auto e3 = g3.update(mk("localized_motion", 0.025, ms(0), true, /*idle_band=*/0.0), T(ms(0)));
        ck(e3.fire && e3.was_strong,
           "0.025 clears the uncalibrated STRONG floor (0.02), so it fires on one observation");

        // And the same motion_level with a CALIBRATED band behaves differently, which is the point
        // of using multiples of the measured floor rather than absolute constants.
        Tier0Gate g4;
        auto e4 = g4.update(mk("localized_motion", 0.025, ms(0), true, kBanded), T(ms(0)));
        ck(!e4.fire && e4.state == GateState::candidate,
           "the SAME 0.025 against the calibrated band (0.0098) is weak-only, not strong");
    }

    std::printf("== reset() closes an event so it cannot outlive its authority ==\n");
    {
        Tier0Gate g;
        g.update(mk("localized_motion", 0.09, ms(0)), T(ms(0)));
        ck(g.state() == GateState::active, "an event is open");
        g.reset();
        ck(g.state() == GateState::quiet, "reset() closes it");
        auto e = g.update(mk("quiet", 0.0, ms(100)), T(ms(100)));
        ck(!e.fire && !e.extend, "and no extension survives the reset");
    }

    std::printf("\n%s  %d/%d checks passed, %d failed\n",
                fails == 0 ? "PASS" : "FAIL", checks - fails, checks, fails);
    return fails == 0 ? 0 : 1;
}
