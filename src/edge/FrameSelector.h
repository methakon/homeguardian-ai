// FrameSelector — pick the few frames worth analysing, and crop them to the motion region.
//
// WHY THIS EXISTS
// ---------------
// An event can span hundreds of frames and only a handful carry information. Analysing all of them
// is how a gated design quietly becomes an ungated one: the gate saves the idle cost, then the
// event itself costs everything it saved. The design fixes the target at 3-8 selected frames.
//
// WHAT IT CONSUMES, AND WHAT IT DELIBERATELY DOES NOT INVENT
// ---------------------------------------------------------
// The geometry comes from the host's own types. `CameraFinding` already carries `spread_radius`
// ("RMS spread of the changed mass / frame diagonal"), `region_travel` and the classified
// `change_character`; the centroid lives in the host's `RegionChangeFeatures` (`centroid_x`,
// `centroid_y`, `centroid_valid`) and is NOT a member of the finding, so it is passed in as an
// explicit parameter. That is the only honest option: a component that invented its own region
// struct would be a second source of truth for a measurement the analyzer already owns, and the
// copy would be invisible because both structs compile.
//
// Because the host already CLASSIFIES the change, the ROI is gated on that classification: a region
// is motion-derived only when the host's own predicate says the change was movement. A light
// switching on cannot produce a motion crop, and neither can a decaying exposure.
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
// So when the classification is absent or is not motion, the selector says so rather than inventing
// a region: a fallback centre crop is used only if the caller explicitly permits it, and the result
// records which happened.
#pragma once

#include "core/CameraActivitySession.h"   // homeguardian::CameraFinding
#include "core/ChangeCharacter.h"
#include "edge/Tier0Gate.h"               // change_character_from_string

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace hg::edge {

using homeguardian::CameraFinding;

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

    // The host's own verdict for this event, consumed directly. `character_valid`,
    // `change_character`, `spread_radius` and `region_travel` all come from here.
    CameraFinding finding{};

    // The centroid the finding does not carry, from the host's RegionChangeFeatures. Passed in by
    // the caller rather than invented here.
    bool centroid_valid = false;
    double centroid_x = 0.5;
    double centroid_y = 0.5;

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
    std::string character_reason;        // why the host's classification did or did not yield a ROI

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

    // Whether the host's classification and the supplied centroid make a region usable for a crop.
    // True only for the host's two MOTION characters, never for illumination, unipolar or unknown.
    static bool region_usable(const CameraFinding& finding, bool centroid_valid,
                              std::string* why = nullptr);

    // Compute the crop box. Exposed so the box rule is testable on its own, without a selection.
    RoiBox roi_for(const CameraFinding& finding, bool centroid_valid, double centroid_x,
                   double centroid_y, int source_width, int source_height, double min_roi_fraction,
                   double roi_padding, bool& from_motion) const;

    // Extract the ROI from a luma plane. The output dimensions are the BOX dimensions - this
    // function has no model input size to resize to, and does not resize. `out_w`/`out_h` are
    // outputs, not requests.
    static bool crop(const std::uint8_t* plane, int plane_width, int plane_height,
                     const RoiBox& roi, std::vector<std::uint8_t>& out,
                     int& out_w, int& out_h);
};

}  // namespace hg::edge
