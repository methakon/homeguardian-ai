// Host tests for ThermalWorkController — heat relief that cannot cost the event.
//
// The suite is built around one invariant, and it is checked by SWEEPING the input space rather
// than by sampling it at the points where a failure would be convenient to notice.
#include "edge/ThermalWorkController.h"

#include <cstdio>
#include <string>
#include <vector>

using namespace hg::edge;

static int fails = 0, checks = 0;
static void ck(bool c, const char* what) {
    ++checks;
    if (c) { std::printf("  ok    %s\n", what); }
    else   { ++fails; std::printf("  FAIL  %s\n", what); }
}

// The handset's shape: a skin/quiet zone that binds well below the CPU zones. These are the
// numbers that make "minimum passive trip" the right rule rather than "the CPU limit".
static std::vector<ThermalZone> handset(double skin_c, double cpu_c) {
    return {
        {"skin",  skin_c,  56.0,  90.0},
        {"quiet", skin_c,  60.0, 100.0},
        {"cpu",   cpu_c,  125.0, 125.0},
    };
}

int main() {
    std::printf("== THE INVARIANT: Tier-0 is never reduced, across an exhaustive sweep ==\n");
    {
        // Every combination of zone shape and temperature, including absurd extremes. If any
        // input could reduce the observation rate, this finds it.
        ThermalWorkController c;
        int swept = 0;
        bool violated = false;
        std::string where;
        for (double skin = 20.0; skin <= 100.0; skin += 1.0) {
            for (double cpu = 20.0; cpu <= 130.0; cpu += 5.0) {
                for (int cfg = 0; cfg < 3; ++cfg) {
                    const auto zones = handset(skin, cpu);
                    ThermalDecision d;
                    if (cfg == 0)      d = c.update(zones);
                    else if (cfg == 1) d = c.update({});
                    else               d = c.update({{"x", skin, 0.0, 0.0}});
                    ++swept;
                    if (d.tier0_rate_multiplier != 1.0) {
                        violated = true;
                        where = "skin=" + std::to_string(skin) + " cpu=" + std::to_string(cpu) +
                                " cfg=" + std::to_string(cfg) + " -> " +
                                std::to_string(d.tier0_rate_multiplier);
                    }
                }
            }
        }
        ck(!violated, ("Tier-0 multiplier is 1.0 for every one of " + std::to_string(swept) +
                       " swept inputs").c_str());
        if (violated) std::printf("        violated at %s\n", where.c_str());

        // And it is 1.0 even in the worst state, which is the case that matters.
        auto crit = c.for_state(ThermalState::critical);
        ck(crit.tier0_rate_multiplier == 1.0, "and still 1.0 in the CRITICAL state");
        ck(!crit.semantic_enabled, "even though semantic work is suspended there");
        ck(crit.semantic_frames == 0 && crit.semantic_threads == 0, "and its semantic share is zero");
    }

    std::printf("== the envelope is the MINIMUM passive trip, and the skin zone binds ==\n");
    {
        ThermalWorkController c;
        // skin 33 C, cpu 70 C: the CPU is far hotter but the skin zone is what will throttle.
        auto d = c.update(handset(33.0, 70.0));
        ck(d.envelope_known, "the envelope is known");
        ck(d.envelope_c == 56.0, "the envelope is 56 C - the skin zone's trip, not the CPU's 125 C");
        ck(d.binding_zone == "skin", "and the binding zone is named");
        ck(d.hottest_c == 70.0 && d.hottest_zone == "cpu", "while the hottest zone is reported honestly");
        ck(d.load > 1.19 && d.load < 1.26, "load is hottest/envelope, i.e. above 1 - the CPU already exceeds the skin trip");
        ck(d.state == ThermalState::critical, "so the state is critical");
        ck(!d.semantic_enabled, "and semantic work is suspended");
    }

    std::printf("== states step at the configured fractions of the envelope ==\n");
    {
        ThermalWorkController c;
        // The CPU must stay BELOW the varied skin value, or the CPU becomes the hottest zone and
        // the load stops tracking the percentage being tested. Pinning it at 40 made 50% read as
        // 0.71 load, which is correctly "warm" - the fault was in the fixture, not the rule.
        auto at = [&](double pct) {
            return c.update(handset(56.0 * pct, 20.0));
        };
        ck(at(0.50).state == ThermalState::normal, "50% of the envelope is normal");
        ck(at(0.75).state == ThermalState::warm, "75% is warm (>= 0.70)");
        ck(at(0.88).state == ThermalState::hot, "88% is hot (>= 0.85)");
        ck(at(0.97).state == ThermalState::critical, "97% is critical (>= 0.95)");
    }

    std::printf("== degrading is immediate; recovering is slow (hysteresis) ==\n");
    {
        ThermalWorkController c;
        // climb to hot
        c.update(handset(56.0 * 0.90, 20.0));
        ck(c.state() == ThermalState::hot, "climbs straight to hot");
        // step back just under the hot threshold: must NOT recover yet
        auto d = c.update(handset(56.0 * 0.84, 20.0));
        ck(d.state == ThermalState::hot, "a reading just below the hot threshold does NOT recover");
        ck(c.held_by_hysteresis() > 0, "and the hold is counted, so the behaviour is visible");
        // now clearly below it
        auto d2 = c.update(handset(56.0 * 0.70, 20.0));
        ck(d2.state != ThermalState::hot, "clearly below it, the state recovers");
        ck(c.step_ups() > 0, "and the recovery is counted");

        // A device hovering at the boundary must not flap.
        ThermalWorkController c2;
        int transitions = 0;
        ThermalState prev = ThermalState::normal;
        for (int i = 0; i < 40; ++i) {
            const double pct = (i % 2 == 0) ? 0.90 : 0.83;    // oscillating across the threshold
            auto dd = c2.update(handset(56.0 * pct, 40.0));
            if (dd.state != prev) { ++transitions; prev = dd.state; }
        }
        ck(transitions <= 1, "oscillating across a threshold causes at most one transition, not flapping");
    }

    std::printf("== semantic work shrinks with the state; observation rate does not ==\n");
    {
        ThermalWorkController c;
        const auto n = c.for_state(ThermalState::normal);
        const auto w = c.for_state(ThermalState::warm);
        const auto h = c.for_state(ThermalState::hot);
        const auto k = c.for_state(ThermalState::critical);
        ck(n.semantic_frames > w.semantic_frames, "normal allows more frames than warm");
        ck(w.semantic_frames > h.semantic_frames, "warm more than hot");
        ck(h.semantic_frames > k.semantic_frames, "hot more than critical");
        ck(n.semantic_frames == 8 && h.semantic_frames == 3,
           "and they sit in the 3-8 frame band the design specifies");
        ck(n.semantic_threads <= 2, "threads stay at 1-2, never all cores");
        for (auto* d : {&n, &w, &h, &k}) {
            if (d->tier0_rate_multiplier != 1.0) { ck(false, "a state reduced the observation rate"); break; }
        }
        ck(true, "no state reduced the observation rate");
    }

    std::printf("== an UNKNOWN envelope is reported as unknown, not as cool ==\n");
    {
        ThermalWorkController c;
        auto d = c.update({});
        ck(!d.envelope_known, "no zones means the envelope is NOT known");
        ck(d.reason.find("unknown") != std::string::npos, "and the reason says so");
        ck(d.reason.find("not a claim that it is cool") != std::string::npos,
           "explicitly refusing to call it cool");
        ck(d.tier0_rate_multiplier == 1.0, "the observation rate is still untouched");

        ThermalWorkController c2;
        auto d2 = c2.update({{"skin", 40.0, 0.0, 0.0}});   // a zone with no declared trip
        ck(!d2.envelope_known, "a zone with no passive trip point leaves the envelope unknown too");
        ck(d2.hottest_c == 40.0, "though the temperature it did report is carried");

        // And an unknown envelope must not fabricate safety.
        ThermalWorkController c3;
        auto d3 = c3.update({});
        ck(d3.state == ThermalState::normal && d3.envelope_known == false,
           "the state stays normal WHILE the envelope is flagged unknown - the caller can tell");
    }

    std::printf("== the first reading establishes the state directly ==\n");
    {
        ThermalWorkController c;
        auto d = c.update(handset(56.0 * 0.97, 40.0));
        ck(d.state == ThermalState::critical,
           "a device that is ALREADY critical on the first sample is treated as critical, "
           "not ramped up from normal");
        ck(!d.semantic_enabled, "with semantic work suspended at once");
    }

    std::printf("\n%s  %d/%d checks passed, %d failed\n",
                fails == 0 ? "PASS" : "FAIL", checks - fails, checks, fails);
    return fails == 0 ? 0 : 1;
}
