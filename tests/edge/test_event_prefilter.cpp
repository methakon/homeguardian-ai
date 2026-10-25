// Host tests for EventPreFilter — the insertion point between detection and analysis.
//
// These drive the pre-filter with the same real CameraFinding values the analyzer produces, over
// frame sequences that model a quiet room, a crossing, a light switch and a dead device. The
// point of the suite is not only that the trigger fires correctly, but that the ANALYSIS VOLUME
// drops - which is the entire reason the pre-filter exists.
#include "edge/EventPreFilter.h"

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
static constexpr double kBanded = 0.0098;

static CameraFinding quiet(double motion = 0.0) {
    CameraFinding f;
    f.valid = true;
    f.kind = CameraFindingKind::quiet_baseline;
    f.real_source = true;
    f.motion_level = motion;
    f.character_valid = true;
    f.change_character = "quiet";
    f.idle_band = kBanded;
    return f;
}
static CameraFinding motion(const char* character, double level, std::size_t travel = 3) {
    CameraFinding f = quiet(level);
    f.change_character = character;
    f.region_travel = 0.02;
    f.travel_consistent = travel;
    return f;
}
static CameraFinding stalled() {
    CameraFinding f;
    f.valid = true;
    f.kind = CameraFindingKind::frames_stalled;
    f.reason = "device timeout";
    return f;
}

int main() {
    std::printf("== a quiet room is not analysed, and says why ==\n");
    {
        EventPreFilter pf;
        int analysed = 0;
        for (int i = 0; i < 120; ++i) {
            auto d = pf.submit(quiet(), T(ms(i * 100)));
            if (d.analyse) ++analysed;
        }
        ck(analysed == 0, "120 quiet frames produce zero analysis");
        ck(pf.stats().frames_seen == 120, "all 120 were seen");
        ck(pf.stats().suppressed_no_motion == 120, "and all 120 are attributed to 'no motion'");
        ck(pf.stats().analysis_ratio() == 0.0, "the analysis ratio is zero");
    }

    std::printf("== a crossing is analysed, but only around the crossing ==\n");
    {
        EventPreFilter pf;
        int analysed = 0, begins = 0;
        std::uint64_t id = 0;
        // 100 quiet frames, one strong crossing, 100 quiet frames
        for (int i = 0; i < 100; ++i) pf.submit(quiet(), T(ms(i * 100)));
        for (int i = 100; i < 103; ++i) {                 // the subject passes
            auto d = pf.submit(motion("localized_motion", 0.09), T(ms(i * 100)));
            if (d.analyse) ++analysed;
            if (d.begins_event) { ++begins; id = d.event_id; }
        }
        for (int i = 103; i < 203; ++i) {
            auto d = pf.submit(quiet(), T(ms(i * 100)));
            if (d.analyse) ++analysed;
        }
        ck(begins == 1, "exactly ONE event begins across the whole sequence");
        ck(id != 0, "and it carries an id");
        ck(analysed >= 3, "the crossing frames are analysed");
        // 1000 ms latch at 100 ms cadence keeps ~10 frames open after the crossing
        ck(analysed <= 20, "and the analysed set stays bounded - the latch does not run away");
        ck(pf.stats().analysis_ratio() < 0.15,
           "so under 15% of frames reach the analysis process over this sequence");
        std::printf("        (analysis ratio %.3f, analysed %d of %llu)\n",
                    pf.stats().analysis_ratio(), analysed,
                    static_cast<unsigned long long>(pf.stats().frames_seen));
    }

    std::printf("== the quiet frames INSIDE the latch are analysed, not skipped ==\n");
    {
        EventPreFilter pf;
        pf.submit(motion("localized_motion", 0.09), T(ms(0)));
        auto during = pf.submit(quiet(), T(ms(200)));
        ck(during.analyse, "a quiet frame inside the latch is still analysed");
        ck(during.continues_event, "and reported as continuing the event");
        ck(during.event_id != 0, "with the same event id");
        ck(during.anchor == T(ms(0)), "and the anchor still points at where it began");
    }

    std::printf("== a light switching on is not analysed as movement ==\n");
    {
        EventPreFilter pf;
        int analysed = 0;
        for (int i = 0; i < 20; ++i) pf.submit(quiet(), T(ms(i * 100)));
        auto d1 = pf.submit(motion("global_illumination", 0.30), T(ms(2000)));
        auto d2 = pf.submit(motion("illumination_settling", 0.12), T(ms(2100)));
        auto d3 = pf.submit(motion("localized_unipolar", 0.25), T(ms(2200)));
        if (d1.analyse) ++analysed;
        if (d2.analyse) ++analysed;
        if (d3.analyse) ++analysed;
        ck(analysed == 0, "none of a brightness step, its settling tail, or a unipolar change is analysed");
        ck(pf.stats().suppressed_illumination == 2, "the two illumination frames are attributed to illumination");
        ck(pf.stats().suppressed_unipolar == 1, "and the unipolar one to unipolar");
    }

    std::printf("== a dead device is not a quiet scene ==\n");
    {
        EventPreFilter pf;
        for (int i = 0; i < 30; ++i) pf.submit(stalled(), T(ms(i * 100)));
        ck(pf.stats().analysed == 0, "a stalled device produces no analysis");
        ck(pf.stats().stalled_frames == 30, "and every stall is counted as a stall");
        ck(pf.stats().suppressed_no_motion == 0,
           "NOT as 'no motion' - a broken camera must never look like a still room");
    }

    std::printf("== the ratio is honest about what it measured ==\n");
    {
        EventPreFilter pf;
        ck(pf.stats().analysis_ratio() == 0.0, "with no frames seen the ratio is 0, not a division by zero");
        for (int i = 0; i < 10; ++i) pf.submit(motion("localized_motion", 0.09), T(ms(i * 100)));
        ck(pf.stats().frames_seen == 10, "ten frames seen");
        ck(pf.stats().analysis_ratio() > 0.0, "and a non-zero ratio once events are present");
        ck(pf.stats().events == 1, "still exactly one event, not one per frame");
    }

    std::printf("== reset() stops analysis on an event that lost its authority ==\n");
    {
        EventPreFilter pf;
        pf.submit(motion("localized_motion", 0.09), T(ms(0)));
        ck(pf.state() == GateState::active, "an event is open");
        pf.reset();
        ck(pf.state() == GateState::quiet, "reset() closes it");
        auto after = pf.submit(quiet(), T(ms(100)));
        ck(!after.analyse, "and nothing is analysed afterwards on the strength of the old event");
    }

    std::printf("== a mixed realistic minute, end to end ==\n");
    {
        EventPreFilter pf;
        // 600 frames = 60 s at 100 ms cadence. Two crossings, one light change, one stall burst.
        for (int i = 0; i < 600; ++i) {
            const auto t = T(ms(i * 100));
            if (i == 100 || i == 101 || i == 102)      pf.submit(motion("localized_motion", 0.09), t);
            else if (i == 300)                         pf.submit(motion("global_illumination", 0.30), t);
            else if (i >= 400 && i < 405)              pf.submit(stalled(), t);
            else if (i == 500 || i == 501 || i == 502) pf.submit(motion("localized_motion", 0.09), t);
            else                                       pf.submit(quiet(), t);
        }
        auto s = pf.stats();
        ck(s.frames_seen == 600, "600 frames seen");
        ck(s.events == 2, "exactly two events, one per crossing");
        ck(s.suppressed_illumination == 1, "the light change was suppressed and attributed");
        ck(s.stalled_frames == 5, "the stall burst is counted as a device fault");
        ck(s.analysis_ratio() < 0.10, "under 10% of a realistic minute reached the analyser");
        std::printf("        (analysed %llu of %llu, ratio %.3f; events %llu)\n",
                    static_cast<unsigned long long>(s.analysed),
                    static_cast<unsigned long long>(s.frames_seen),
                    s.analysis_ratio(), static_cast<unsigned long long>(s.events));
    }

    std::printf("\n%s  %d/%d checks passed, %d failed\n",
                fails == 0 ? "PASS" : "FAIL", checks - fails, checks, fails);
    return fails == 0 ? 0 : 1;
}
