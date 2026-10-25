// Host tests for FrameSelector — pick the few frames worth analysing, and crop without resizing.
//
// The crop tests are the ones that matter: the whole saving of a region crop is thrown away if the
// result is scaled back up to a model input size. These tests assert the extracted dimensions ARE
// the box dimensions, and that a small crop really is a small number of pixels.
#include "edge/FrameSelector.h"

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

using namespace hg::edge;
using Clock = std::chrono::system_clock;
using ms = std::chrono::milliseconds;

static int fails = 0, checks = 0;
static void ck(bool c, const char* what) {
    ++checks;
    if (c) { std::printf("  ok    %s\n", what); }
    else   { ++fails; std::printf("  FAIL  %s\n", what); }
}
static Clock::time_point T(ms t) { return Clock::time_point{} + t; }

static std::vector<FrameCandidate> every(ms from, ms to, ms step) {
    std::vector<FrameCandidate> v;
    std::uint64_t s = 0;
    for (ms t = from; t <= to; t += step) {
        FrameCandidate c; c.captured_at = T(t); c.sequence = s++; v.push_back(c);
    }
    return v;
}

static SelectionRequest req(std::size_t want = 6) {
    SelectionRequest r;
    r.event_id = 42;
    r.window_start = T(ms(0));
    r.window_end = T(ms(1000));
    r.source_width = 640;
    r.source_height = 360;
    r.want_frames = want;
    r.region = MotionRegion{true, 0.5, 0.5, 0.10, 0.0};
    return r;
}

int main() {
    std::printf("== the 3-8 band is enforced, not trusted ==\n");
    {
        FrameSelector sel;
        auto cands = every(ms(0), ms(1000), ms(100));       // 11 frames

        auto d = sel.select(cands, req(6));
        ck(d.ok, "a normal request selects");
        ck(d.frames.size() == 6, "and returns the six asked for");

        auto low = sel.select(cands, req(1));
        ck(low.frames.size() == kMinSelectedFrames, "asking for 1 returns the minimum of 3, not 1");

        auto high = sel.select(cands, req(40));
        ck(high.frames.size() == kMaxSelectedFrames, "asking for 40 returns 8, not 40");
        ck(high.hit_frame_ceiling, "and the ceiling being hit is reported");
        for (auto& s : {d, low, high}) {
            if (s.frames.size() < kMinSelectedFrames || s.frames.size() > kMaxSelectedFrames) {
                ck(false, "a selection left the 3-8 band"); break;
            }
        }
        ck(true, "no selection left the 3-8 band");
    }

    std::printf("== frames are spread evenly across the window, endpoints included ==\n");
    {
        FrameSelector sel;
        auto cands = every(ms(0), ms(1000), ms(100));       // 11 frames, 0..1000
        auto d = sel.select(cands, req(6));
        ck(d.frames.front().captured_at == T(ms(0)), "the first frame is the window start");
        ck(d.frames.back().captured_at == T(ms(1000)), "the last is the window end");
        // 11 frames, 6 picks -> indices 0,2,4,6,8,10 -> 0,200,400,600,800,1000 ms
        bool spread = true;
        for (std::size_t i = 1; i < d.frames.size(); ++i) {
            if (d.frames[i].captured_at <= d.frames[i - 1].captured_at) spread = false;
        }
        ck(spread, "and they are strictly increasing in time");
        ck(d.frames[1].captured_at == T(ms(200)), "the spacing is even (200 ms here)");
    }

    std::printf("== candidates outside the window are not part of the event ==\n");
    {
        FrameSelector sel;
        auto cands = every(ms(-500), ms(1500), ms(100));    // extends past both ends
        auto r = req(6);
        auto d = sel.select(cands, r);
        ck(d.ok, "the selection still succeeds");
        ck(d.candidates_considered == 21, "every candidate is accounted for as considered");
        ck(d.candidates_in_window == 11, "but only the in-window ones are eligible");
        ck(d.frames.front().captured_at >= r.window_start && d.frames.back().captured_at <= r.window_end,
           "and nothing outside the window is selected");
    }

    std::printf("== THE CROP IS NOT RESIZED - the saving is real ==\n");
    {
        // crop() is static: it needs no instance, because it needs no model size to resize to.
        // a 640x360 plane with a recognisable pattern
        const int W = 640, H = 360;
        std::vector<std::uint8_t> plane(static_cast<std::size_t>(W) * H);
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x)
                plane[static_cast<std::size_t>(y) * W + x] = static_cast<std::uint8_t>((x + y) & 0xFF);

        RoiBox box{100, 50, 80, 60};
        std::vector<std::uint8_t> out;
        int ow = 0, oh = 0;
        ck(FrameSelector::crop(plane.data(), W, H, box, out, ow, oh), "the crop succeeds");

        ck(ow == box.width && oh == box.height,
           "the output dimensions ARE the box dimensions - no resize to any model input size");
        ck(out.size() == box.pixels(), "and the buffer is exactly the box's pixels");
        ck(out.size() < plane.size(), "which is FEWER pixels than the source - the saving is real");
        const double frac = static_cast<double>(out.size()) / static_cast<double>(plane.size());
        ck(frac < 0.25, "here under a quarter of the frame, as the box implies");
        // and the pixels are the right ones
        ck(out[0] == plane[static_cast<std::size_t>(50) * W + 100], "the first pixel is the box's origin");
        ck(out[static_cast<std::size_t>(59) * 80 + 79] ==
               plane[static_cast<std::size_t>(109) * W + 179],
           "and the last is the box's far corner");

        // a full-frame box is allowed and is not "saving" anything - it should say so by its size
        RoiBox full{0, 0, W, H};
        int fw = 0, fh = 0;
        ck(FrameSelector::crop(plane.data(), W, H, full, out, fw, fh), "a full-frame box crops");
        ck(out.size() == plane.size(), "and yields the full frame - no hidden downscale either");

        // out-of-bounds is refused rather than clamped silently
        RoiBox bad{600, 50, 80, 60};
        ck(!FrameSelector::crop(plane.data(), W, H, bad, out, ow, oh), "an out-of-bounds box is refused");
        ck(out.empty() && ow == 0 && oh == 0, "and leaves nothing behind");
    }

    std::printf("== the ROI follows the motion region ==\n");
    {
        FrameSelector sel;
        bool from_motion = false;
        MotionRegion r{true, 0.25, 0.75, 0.10, 0.0};
        auto box = sel.roi_for(r, 640, 360, 0.10, 0.25, from_motion);
        ck(from_motion, "a valid region produces a motion-derived box");
        ck(box.valid(), "and a usable one");
        // centre of the box should be near (0.25*640, 0.75*360) = (160, 270)
        const double cx = box.x + box.width / 2.0, cy = box.y + box.height / 2.0;
        ck(std::abs(cx - 160.0) <= 1.5, "horizontally centred on the centroid");
        ck(std::abs(cy - 270.0) <= 1.5, "vertically centred on the centroid");
        ck(box.width > 0 && box.height > 0, "with a positive extent");

        // a bigger spread gives a bigger box
        MotionRegion small{true, 0.5, 0.5, 0.02, 0.0};
        MotionRegion big{true, 0.5, 0.5, 0.30, 0.0};
        bool m1 = false, m2 = false;
        auto b1 = sel.roi_for(small, 640, 360, 0.10, 0.25, m1);
        auto b2 = sel.roi_for(big, 640, 360, 0.10, 0.25, m2);
        ck(b2.pixels() > b1.pixels(), "a larger spread produces a larger box");

        // travel widens it further: a subject that moved is not contained by its final spread
        MotionRegion travelled{true, 0.5, 0.5, 0.02, 0.20};
        bool m3 = false;
        auto b3 = sel.roi_for(travelled, 640, 360, 0.10, 0.25, m3);
        ck(b3.pixels() > b1.pixels(), "and travel widens the box beyond the spread alone");
    }

    std::printf("== a degenerate region cannot produce an unusable box ==\n");
    {
        FrameSelector sel;
        bool fm = false;
        MotionRegion zero{true, 0.5, 0.5, 0.0, 0.0};
        auto box = sel.roi_for(zero, 640, 360, 0.10, 0.25, fm);
        ck(fm, "a zero spread is still a valid region");
        ck(box.width >= 36 && box.height >= 36,
           "but the box is floored at 10% of the shorter side (36 px of 360), not zero");

        // the floor tracks the source size
        bool fm2 = false;
        auto box2 = sel.roi_for(zero, 1920, 1080, 0.10, 0.25, fm2);
        ck(box2.width >= 108, "and the floor scales with the source (108 px of 1080)");
    }

    std::printf("== a box near an edge stays inside the frame ==\n");
    {
        FrameSelector sel;
        bool fm = false;
        MotionRegion corner{true, 0.98, 0.02, 0.20, 0.0};
        auto box = sel.roi_for(corner, 640, 360, 0.10, 0.25, fm);
        ck(box.x >= 0 && box.y >= 0, "the origin is not negative");
        ck(box.x + box.width <= 640, "and the box does not run off the right edge");
        ck(box.y + box.height <= 360, "nor off the bottom");
        ck(box.width > 0 && box.height > 0, "while still being usable");
    }

    std::printf("== an absent region is reported, not invented ==\n");
    {
        FrameSelector sel;
        auto cands = every(ms(0), ms(1000), ms(100));
        auto r = req(6);
        r.region = MotionRegion{};                      // valid == false

        auto d = sel.select(cands, r);
        ck(d.ok, "a fallback crop is produced when the caller permits it");
        ck(!d.roi_from_motion, "and it is flagged as NOT motion-derived");
        ck(d.reason.find("FALLBACK") != std::string::npos, "the reason says FALLBACK in the clear");
        ck(d.roi.valid(), "and the box is still usable");

        auto r2 = req(6);
        r2.region = MotionRegion{};
        r2.allow_centre_fallback = false;
        auto d2 = sel.select(cands, r2);
        ck(!d2.ok, "when the caller forbids a fallback, it fails instead");
        ck(d2.frames.empty(), "with no frames");
        ck(d2.reason.find("not permitted") != std::string::npos, "and a reason naming the refusal");
    }

    std::printf("== too few frames is no analysis, and says so ==\n");
    {
        FrameSelector sel;
        auto cands = every(ms(0), ms(1000), ms(400));       // 0, 400, 800 - three
        auto d = sel.select(cands, req(6));
        ck(d.ok, "three frames is enough");
        ck(d.frames.size() == 3, "and all three are selected");

        auto few = every(ms(0), ms(400), ms(400));          // two
        auto d2 = sel.select(few, req(6));
        ck(!d2.ok, "two frames is not");
        ck(d2.reason.find("2 frame(s)") != std::string::npos, "the reason counts them exactly");
        ck(d2.frames.empty(), "and nothing is returned to analyse");
    }

    std::printf("== unknown geometry fails rather than guessing ==\n");
    {
        FrameSelector sel;
        auto cands = every(ms(0), ms(1000), ms(100));
        auto r = req(6);
        r.source_width = 0;
        auto d = sel.select(cands, r);
        ck(!d.ok, "a zero source width fails");
        ck(d.reason.find("geometry") != std::string::npos, "naming the missing geometry");
    }

    std::printf("\n%s  %d/%d checks passed, %d failed\n",
                fails == 0 ? "PASS" : "FAIL", checks - fails, checks, fails);
    return fails == 0 ? 0 : 1;
}
