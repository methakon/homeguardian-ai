#include "core/ChangeCharacter.h"

#include <iomanip>
#include <sstream>

namespace homeguardian {

ChangeCharacter classify_change(const RegionChangeFeatures& f, const ChangeCharacterPolicy& policy) {
    if (!f.valid) return ChangeCharacter::unknown;
    if (f.changed_pixels == 0) return ChangeCharacter::quiet;

    // Scale-free: the changed mass's RMS radius, not a fixed pixel count or cell count, decides whether
    // the change was compact (a body) or spread (the whole frame). A 30x40 block and a whole-frame step
    // separate cleanly on this measure at any frame size.
    const bool compact = f.spread_radius < policy.compact_radius;

    // Quiet means small AND spread - the shape of sensor noise and of a slow illumination flutter. A
    // small COMPACT change is a small object, and calling that quiet is how a real animal-sized movement
    // would be lost (0.7% of pixels is a small body in a 160x120 frame; the same body in a 640x480 frame
    // is 0.04%). The two conditions are therefore joined, and the tests pin both sides.
    if (f.changed_fraction < policy.quiet_fraction && !compact) return ChangeCharacter::quiet;
    const bool one_directional = f.bipolar_balance <= policy.uniform_polarity;

    // A change that SCALES with the scene's own brightness is illumination, whatever its spatial shape:
    // a gain, brightness or exposure change multiplies what is already there. Checked first, because on
    // real frames the automatic-gain response is both compact and structured, and the spatial tests
    // cannot tell it from a body.
    if (f.gain_correlation >= policy.gain_correlation_illumination) {
        return ChangeCharacter::global_illumination;
    }

    // A change that covers most of the frame and moves every pixel the same way is illumination (or an
    // exposure shift): no body can brighten every pixel of a static scene at once.
    if (!compact && one_directional) return ChangeCharacter::global_illumination;

    // Compact and one-directional: something appeared or a light switched on IN the scene. Real, but
    // not motion - and the honest vocabulary says so rather than calling it movement.
    if (one_directional) {
        return compact ? ChangeCharacter::localized_unipolar : ChangeCharacter::global_illumination;
    }

    // Both polarities: the classic signature of something moving - a body covering here while
    // revealing there.
    if (compact) return ChangeCharacter::localized_motion;

    // Both polarities but spread widely: large or close movement (or rain, or foliage in wind, or a
    // moving shadow). Reported as motion, with the confounders named in the reason.
    return ChangeCharacter::diffuse_motion;
}

std::string change_character_reason(const RegionChangeFeatures& f, ChangeCharacter verdict) {
    if (!f.valid) return "no region descriptor: " + f.reason;

    std::ostringstream os;
    os << std::fixed << std::setprecision(3) << "changed=" << f.changed_fraction
       << " peak_cell=" << f.peak_cell_share << " cells=" << f.occupied_cell_fraction
       << " polarity_balance=" << f.bipolar_balance << " spread=" << f.spread_radius
       << " gain_corr=" << f.gain_correlation << " -> "
       << change_character_to_string(verdict);

    switch (verdict) {
        case ChangeCharacter::global_illumination:
            os << (f.gain_correlation >= ChangeCharacterPolicy{}.gain_correlation_illumination
                       ? " (the change scales with the scene's own brightness: an illumination, gain or "
                         "exposure change)"
                       : " (spread over the frame and one-directional: an illumination or exposure "
                         "change)");
            break;
        case ChangeCharacter::localized_unipolar:
            os << " (compact and one-directional: something appeared or a source switched on in view)";
            break;
        case ChangeCharacter::localized_motion:
            os << " (compact and two-directional: something moved through the field)";
            break;
        case ChangeCharacter::diffuse_motion:
            os << " (two-directional but spread: large or close movement, foliage, rain or a shadow)";
            break;
        case ChangeCharacter::quiet:
        case ChangeCharacter::unknown:
            break;
    }
    return os.str();
}

}  // namespace homeguardian
