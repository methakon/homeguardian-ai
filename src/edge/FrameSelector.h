// FrameSelector — pick the few frames worth analysing, and crop them to the motion region.
//
// WHY THIS EXISTS
// ---------------
// An event can span hundreds of frames and only a handful carry information. Analysing all of them
// is how a gated design quietly becomes an ungated one: the gate saves the idle cost, then the
// event itself costs everything it saved. The design fixes the target at 3-8 selected frames.
//
// THE PITFALL THIS FILE IS BUILT AROUND
// -------------------------------------
// Cropping to the region and then RESIZING the crop back up to the model's input size gives up the
// entire saving. You pay for the crop and compute the same number of pixels as before it. The
// selector therefore produces a crop BOX and, when asked to extract, returns exactly the box's
// pixels - it never scales, and it has no model input size to scale to. That is deliberate: the
// component that cannot know the model's input size cannot resize to it.
//
// THE ROI IS A HINT, NOT A CROP OF CERTAINTY
// ------------------------------------------
// The region comes from the detector's own geometry - a centroid and a spread - so it can be
// absent, degenerate or stale. When it is absent the selector says so rather than inventing a
// region: a fallback centre crop is used only if the caller explicitly permits it, and the result
// records which happened.
#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace hg::edge {

// A crop box in SOURCE pixels. Never a model input size.
struct RoiBox {
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;

    bool valid() const { return width > 0 && height > 0; }
    std::size_t pixels() const {
        return valid() ? static_cast<std::size_t>(width) * static_cast<std::size_t>(height) : 0;
    }
};

// Where the change was, as the detector reported it. Normalised to [0,1] over the frame.
struct MotionRegion {
    bool valid = false;
    double centroid_x = 0.5;
    double centroid_y = 0.5;
    double spread_radius = 0.0;   // RMS spread of the changed mass / frame diagonal
    double travel = 0.0;          // centroid displacement over the event; widens the box
};

// A candidate is a frame's position and time - not its pixels, so the selector never copies media.
struct FrameCandidate {
    std::chrono::system_clock::time_point captured_at{};
    std::uint64_t sequence = 0;
};

struct SelectionRequest {
    std::uint64_t event_id = 0;
    std::chrono::system_clock::time_point window_start{};
    std::chrono::system_clock::time_point window_end{};
    int source_width = 0;
    int source_height = 0;
    MotionRegion region{};
    // 3-8. Clamped, not trusted: a caller asking for 40 frames of a 6-frame event should not get
    // an ungated pipeline by accident.
    std::size_t want_frames = 6;
    // Minimum ROI side as a fraction of the shorter source side, so a degenerate spread cannot
    // produce a one-pixel box that no model can use.
    double min_roi_fraction = 0.10;
    // Padding around the region, as a fraction of its extent, so the subject is not clipped at
    // the edge of its own bounding evidence.
    double roi_padding = 0.25;
    // When the region is absent, may a centre crop be used? If false the selection fails instead.
    bool allow_centre_fallback = true;
};

struct SelectedFrame {
    std::chrono::system_clock::time_point captured_at{};
    std::uint64_t sequence = 0;
    RoiBox roi{};
    bool roi_from_motion = false;
};

struct SelectionResult {
    bool ok = false;
    std::string reason;

    std::uint64_t event_id = 0;
    std::vector<SelectedFrame> frames;   // between 3 and 8 when ok
    RoiBox roi{};
    bool roi_from_motion = false;        // false => a fallback centre crop was used
    bool hit_frame_ceiling = false;      // the request asked for more than the 8 allowed

    std::size_t candidates_considered = 0;
    std::size_t candidates_in_window = 0;
};

// How many frames a request may select. Exposed so a caller can state the bound rather than
// rediscover it.
constexpr std::size_t kMinSelectedFrames = 3;
constexpr std::size_t kMaxSelectedFrames = 8;

class FrameSelector {
public:
    // Choose up to `want_frames` candidates, spread evenly across the window.
    SelectionResult select(const std::vector<FrameCandidate>& candidates,
                           const SelectionRequest& request) const;

    // Compute the crop box for a region, clamped to the source bounds. Exposed so the box rule is
    // testable on its own, without a selection.
    RoiBox roi_for(const MotionRegion& region, int source_width, int source_height,
                   double min_roi_fraction, double roi_padding, bool& from_motion) const;

    // Extract the ROI from a luma plane. The output dimensions are the BOX dimensions - this
    // function has no model input size to resize to, and does not resize. `out_w`/`out_h` are
    // outputs, not requests.
    static bool crop(const std::uint8_t* plane, int plane_width, int plane_height,
                     const RoiBox& roi, std::vector<std::uint8_t>& out,
                     int& out_w, int& out_h);
};

}  // namespace hg::edge
