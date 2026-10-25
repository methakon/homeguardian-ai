#include "edge/FrameSelector.h"

#include <algorithm>
#include <cmath>

namespace hg::edge {

namespace {

// Place a box of the requested size inside [0, limit], keeping the size when it fits and shrinking
// to the limit when it does not. The centre is preserved where possible, so a subject near an edge
// stays as centred as the frame allows rather than being pinned to the corner.
void place(int& origin, int& size, int limit) {
    if (size > limit) {
        size = limit;
        origin = 0;
        return;
    }
    if (origin < 0) origin = 0;
    if (origin + size > limit) origin = limit - size;
    if (origin < 0) origin = 0;
}

}  // namespace

bool FrameSelector::region_usable(const CameraFinding& finding, bool centroid_valid,
                                  std::string* why) {
    // The host classifies the change, so this component does not re-decide what motion is - it asks.
    // `is_localized_or_diffuse_motion` is the host's own predicate, documented as the thing to test
    // when you want "motion, not merely change", so a light switching on or an exposure decay can
    // never produce a motion crop.
    if (!finding.character_valid) {
        if (why) *why = "the host reported no change character for this event";
        return false;
    }
    const ChangeCharacter c = change_character_from_string(finding.change_character);
    if (!homeguardian::is_localized_or_diffuse_motion(c)) {
        if (why) {
            *why = std::string("the host classified the change as ") +
                   homeguardian::change_character_to_string(c) + ", which is not motion";
        }
        return false;
    }
    if (!centroid_valid) {
        // The finding carries a spread and a travel but no centroid, so without one there is no
        // "where" to crop around - only a "how much".
        if (why) *why = "no centroid was supplied, so the region has no location";
        return false;
    }
    if (why) {
        *why = std::string("the host classified the change as ") +
               homeguardian::change_character_to_string(c);
    }
    return true;
}

RoiBox FrameSelector::roi_for(const CameraFinding& finding, bool centroid_valid, double centroid_x,
                              double centroid_y, int source_width, int source_height,
                              double min_roi_fraction, double roi_padding, bool& from_motion) const {
    RoiBox box;
    if (source_width <= 0 || source_height <= 0) {
        from_motion = false;
        return box;
    }
    const int shorter = std::min(source_width, source_height);

    // The floor exists so a degenerate spread cannot produce a box no model can use. Without it a
    // single-pixel spread yields a single-pixel crop and the "crop" saves nothing worth having.
    const int floor_px = std::max(8, static_cast<int>(std::ceil(min_roi_fraction * shorter)));

    std::string why;
    if (!region_usable(finding, centroid_valid, &why)) {
        // No usable region. A centred box of the floor size, and the caller is told it was a
        // fallback - along with WHY the host's own classification did not yield one.
        from_motion = false;
        box.width = std::min(floor_px, source_width);
        box.height = std::min(floor_px, source_height);
        box.x = (source_width - box.width) / 2;
        box.y = (source_height - box.height) / 2;
        return box;
    }

    // The host's own descriptors, consumed rather than copied. `spread_radius` is the RMS spread of
    // the changed mass over the frame diagonal; `region_travel` is how far the centroid moved. The
    // extent covers BOTH: a subject that moved across the frame during the event is not contained
    // by its final spread alone.
    double half_norm = std::max(finding.spread_radius + finding.region_travel * 0.5, 0.0);
    half_norm *= (1.0 + std::max(roi_padding, 0.0));

    int w = static_cast<int>(std::ceil(half_norm * 2.0 * source_width));
    int h = static_cast<int>(std::ceil(half_norm * 2.0 * source_height));
    w = std::max(w, floor_px);
    h = std::max(h, floor_px);
    w = std::min(w, source_width);
    h = std::min(h, source_height);

    const double cx = std::clamp(centroid_x, 0.0, 1.0);
    const double cy = std::clamp(centroid_y, 0.0, 1.0);
    box.x = static_cast<int>(std::lround(cx * source_width - w / 2.0));
    box.y = static_cast<int>(std::lround(cy * source_height - h / 2.0));
    box.width = w;
    box.height = h;
    place(box.x, box.width, source_width);
    place(box.y, box.height, source_height);

    from_motion = true;
    return box;
}

SelectionResult FrameSelector::select(const std::vector<FrameCandidate>& candidates,
                                      const SelectionRequest& request) const {
    SelectionResult r;
    r.event_id = request.event_id;
    r.candidates_considered = candidates.size();

    if (request.source_width <= 0 || request.source_height <= 0) {
        r.reason = "source geometry is unknown; cannot compute a crop";
        return r;
    }
    if (request.window_end < request.window_start) {
        r.reason = "window is inverted";
        return r;
    }

    // Frames inside the event window, in capture order. Candidates outside are not "rejected" for
    // a defect - they simply are not part of this event.
    std::vector<FrameCandidate> in_window;
    in_window.reserve(candidates.size());
    for (const auto& c : candidates) {
        if (c.captured_at >= request.window_start && c.captured_at <= request.window_end) {
            in_window.push_back(c);
        }
    }
    r.candidates_in_window = in_window.size();

    if (in_window.size() < kMinSelectedFrames) {
        // Too few frames is not a thin analysis, it is no analysis. Said plainly so it is not
        // mistaken for a quiet scene.
        r.reason = "only " + std::to_string(in_window.size()) + " frame(s) in the window; " +
                   std::to_string(kMinSelectedFrames) + " are required";
        return r;
    }

    // Clamped, never trusted. A caller asking for 40 frames of a 6-frame event must not be able to
    // turn a gated pipeline into an ungated one by passing a large number.
    std::size_t want = request.want_frames;
    if (want > kMaxSelectedFrames) {
        want = kMaxSelectedFrames;
        r.hit_frame_ceiling = true;
    }
    if (want < kMinSelectedFrames) want = kMinSelectedFrames;
    if (want > in_window.size()) want = in_window.size();

    std::string why;
    const bool usable = region_usable(request.finding, request.centroid_valid, &why);
    r.character_reason = why;

    bool from_motion = false;
    r.roi = roi_for(request.finding, request.centroid_valid, request.centroid_x, request.centroid_y,
                    request.source_width, request.source_height, request.min_roi_fraction,
                    request.roi_padding, from_motion);
    r.roi_from_motion = from_motion;

    if (!r.roi.valid()) {
        r.reason = "no usable crop could be computed";
        return r;
    }
    if (!usable && !request.allow_centre_fallback) {
        // The caller has said a fallback crop is not acceptable evidence. Honour that rather than
        // returning a centre crop the caller cannot distinguish from a located subject.
        r.reason = "no usable motion region (" + why +
                   "), and a centre-crop fallback was not permitted";
        return r;
    }

    // Even spread across the window, endpoints included. Selected by position rather than by
    // timestamp comparison, so an irregular arrival cadence still yields a spread sample.
    const std::size_t n = in_window.size();
    std::vector<std::size_t> idx;
    idx.reserve(want);
    if (want == 1) {
        idx.push_back(0);
    } else {
        for (std::size_t i = 0; i < want; ++i) {
            const std::size_t k = (i * (n - 1)) / (want - 1);
            if (idx.empty() || idx.back() != k) idx.push_back(k);
        }
    }

    r.frames.reserve(idx.size());
    for (auto k : idx) {
        SelectedFrame f;
        f.captured_at = in_window[k].captured_at;
        f.sequence = in_window[k].sequence;
        f.roi = r.roi;
        f.roi_from_motion = from_motion;
        r.frames.push_back(f);
    }

    if (r.frames.size() < kMinSelectedFrames) {
        // The spread collapsed below the minimum (only possible for a tiny window).
        r.frames.clear();
        r.reason = "even spread yielded only " + std::to_string(idx.size()) + " frame(s)";
        return r;
    }

    r.ok = true;
    r.reason = from_motion
                   ? "selected " + std::to_string(r.frames.size()) +
                         " frames cropped to the host-classified motion region"
                   : "selected " + std::to_string(r.frames.size()) +
                         " frames cropped to a FALLBACK centre box - " + why;
    return r;
}

bool FrameSelector::crop(const std::uint8_t* plane, int plane_width, int plane_height,
                         const RoiBox& roi, std::vector<std::uint8_t>& out,
                         int& out_w, int& out_h) {
    out.clear();
    out_w = 0;
    out_h = 0;
    if (!plane || !roi.valid()) return false;
    if (roi.x < 0 || roi.y < 0) return false;
    if (roi.x + roi.width > plane_width || roi.y + roi.height > plane_height) return false;

    // The output dimensions ARE the box dimensions. There is no model size here to resize to, and
    // resizing the crop back up would give back the entire saving the crop was made for.
    out_w = roi.width;
    out_h = roi.height;
    out.resize(static_cast<std::size_t>(roi.width) * static_cast<std::size_t>(roi.height));
    for (int row = 0; row < roi.height; ++row) {
        const std::uint8_t* src = plane + static_cast<std::size_t>(roi.y + row) * plane_width + roi.x;
        std::uint8_t* dst = out.data() + static_cast<std::size_t>(row) * roi.width;
        std::copy(src, src + roi.width, dst);
    }
    return true;
}

}  // namespace hg::edge
