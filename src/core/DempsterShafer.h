#pragma once

// Dempster-Shafer fusion of disagreeing sensors, with an explicit ignorance
// model and an explicit conflict brake.
//
// WHY (ported from the `animal-calamity-precursor-detection` skill, which is the
// one place in the source material where a fusion method is justified for this
// problem): IoT sensors disagree, and a probability-only fusion hides the
// disagreement. DST models it as *ignorance* and exposes it as the conflict mass
// K, which is the number an incident review will ask about.
//
// ADAPTED, NOT COPIED. The skill's frame of discernment is
// {Routine, SeismicPrecursor, WeatherAnomaly}. "SeismicPrecursor" is deliberately
// NOT in this frame: the same skill's own correction ledger records that animal
// seismology is DEBUNKED (ants react to shaking above M2.0, they do not predict)
// and that the Haicheng claim is overstated. A life-safety hypothesis has no
// place in an alerting frame here, so this build uses:
//
//     Theta = { routine, activity_change, environmental_anomaly }
//
// `environmental_anomaly` means "an authorized environmental measurement moved",
// NOT "a storm/flood/earthquake is coming". The subsystem that consumes this can
// therefore never emit a disaster prediction from animal behaviour - the frame
// makes it unrepresentable rather than merely discouraged.
//
// RULES (from the skill, kept verbatim because they are correct):
//   * K -> 1 means the sensors CONTRADICT. Suppress; do not average.
//   * Log K alongside every decision.
//   * Re-normalise after each pairwise fold so K stays interpretable.
//
// ~150 lines rather than a library: the skill's own table notes there is no
// permissively-licensed C++ DST library (mkurdej/bft is LGPL-3.0, which would
// constrain static linking), and Dempster's rule over an 8-element power set is
// shorter than its dependency.

#include <array>
#include <cstddef>
#include <string>

namespace homeguardian {

enum class Hypothesis { routine = 0, activity_change = 1, environmental_anomaly = 2 };

inline constexpr size_t kHypothesisCount = 3;

// A mass function assigns mass to every SUBSET of Theta. Subsets are bitmasks:
// bit 0 = routine, bit 1 = activity_change, bit 2 = environmental_anomaly; the
// empty set (0) always has mass 0 in a valid BPA, and the full mask (7) is
// Theta itself, i.e. total ignorance.
inline constexpr size_t kMassSize = 1u << kHypothesisCount;  // 8
inline constexpr size_t kThetaMask = kMassSize - 1;          // 7

using MassFunction = std::array<double, kMassSize>;

inline const char* hypothesis_to_string(Hypothesis h) {
    switch (h) {
        case Hypothesis::routine: return "routine";
        case Hypothesis::activity_change: return "activity_change";
        case Hypothesis::environmental_anomaly: return "environmental_anomaly";
    }
    return "unknown";
}

struct Combination {
    MassFunction combined{};      // re-normalised by (1 - K)
    double conflict_k = 0.0;      // [0,1): 0 = agree, ->1 = contradict
    bool suppressed = false;      // true when K exceeded the suppression threshold
    std::string reason;
};

struct DempsterShafer {
    // Acceptance target, NOT a measured performance figure: above this conflict
    // mass the evidence is treated as contradictory and no belief is reported.
    static constexpr double kSuppressionK = 0.90;

    // Total ignorance: all mass on Theta, zero belief anywhere.
    static MassFunction vacuous();

    // {h} gets `belief`, Theta keeps the remainder (the standard simple support
    // function). `belief` is clamped to [0,1].
    static MassFunction simple(Hypothesis h, double belief);

    // Dempster's rule over two sources, with K and the suppression verdict.
    //   K        = sum over pairs whose intersection is empty of m1[a]*m2[b]
    //   m(x)     = sum over pairs with a&b == x of m1[a]*m2[b] / (1 - K)
    // When K > kSuppressionK the combined distribution is NOT normalised (it
    // would amplify the contradiction into false certainty); `combined` holds the
    // un-normalised product, `suppressed` is true, and `reason` says so.
    static Combination combine(const MassFunction& a, const MassFunction& b);

    // Belief in a single hypothesis: Bel({h}) = sum of the masses of the subsets
    // OF {h}, i.e. m[{h}] plus m[empty]=0. (Note the direction: summing over
    // SUPERsets is commonality, not belief - an early implementation got this
    // backwards and the unit test caught it.)
    static double belief(const MassFunction& m, Hypothesis h);

    // Plausibility: Pl({h}) = 1 - Bel(not h), i.e. every subset that does not
    // exclude h. The belief/plausibility interval is the honest way to show how
    // much the fusion does NOT know; the gap is the ignorance.
    static double plausibility(const MassFunction& m, Hypothesis h);

    // Uncommitted mass (mass on Theta) - the ignorance the fusion admits.
    static double ignorance_mass(const MassFunction& m);

    // Hypothesis with the largest belief. `tie` is set when the top two beliefs
    // are equal within 1e-9 (caller should treat a tie as undecided).
    static Hypothesis argmax_belief(const MassFunction& m, bool* tie = nullptr);

    // Valid BPA: every mass finite and >= 0, empty set has 0 mass, and the masses
    // sum to 1 within 1e-6.
    static bool valid(const MassFunction& m, std::string* why = nullptr);

    // Compact, non-sensitive summary for a log line (e.g. "K=0.12 belief=0.71
    // ignorance=0.20 suppressed=no").
    static std::string summarize(const Combination& c, Hypothesis top);
};

}  // namespace homeguardian
