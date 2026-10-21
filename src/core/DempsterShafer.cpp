#include "DempsterShafer.h"

#include <cmath>
#include <cstdio>

namespace homeguardian {

namespace {

size_t singleton_mask(Hypothesis h) { return size_t{1} << static_cast<size_t>(h); }

std::string fmt(const char* label, double v) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%s=%.3f", label, v);
    return std::string(buf);
}

}  // namespace

MassFunction DempsterShafer::vacuous() {
    MassFunction m{};
    m[kThetaMask] = 1.0;
    return m;
}

MassFunction DempsterShafer::simple(Hypothesis h, double belief_value) {
    double b = belief_value;
    if (!(b > 0.0)) b = 0.0;
    if (b > 1.0) b = 1.0;
    MassFunction m{};
    m[singleton_mask(h)] = b;
    m[kThetaMask] = 1.0 - b;
    return m;
}

Combination DempsterShafer::combine(const MassFunction& a, const MassFunction& b) {
    Combination out;
    std::string why;
    if (!valid(a, &why)) {
        out.suppressed = true;
        out.reason = "invalid mass function (a): " + why;
        return out;
    }
    if (!valid(b, &why)) {
        out.suppressed = true;
        out.reason = "invalid mass function (b): " + why;
        return out;
    }

    MassFunction product{};
    double k = 0.0;
    for (size_t i = 0; i < kMassSize; ++i) {
        if (a[i] == 0.0) continue;
        for (size_t j = 0; j < kMassSize; ++j) {
            const double m = a[i] * b[j];
            if (m == 0.0) continue;
            const size_t intersection = i & j;
            if (intersection == 0) {
                k += m;  // the sources pointed at disjoint sets: conflict mass
            } else {
                product[intersection] += m;  // Dempster's rule: mass to the intersection
            }
        }
    }

    out.conflict_k = k;
    if (k > kSuppressionK) {
        // Do NOT normalise: dividing by (1-K) here would inflate the surviving
        // mass into false certainty exactly when the sensors contradict.
        out.combined = product;
        out.suppressed = true;
        out.reason = "conflict mass above suppression threshold; sensors contradict" + std::string(" (") +
                     fmt("K", k) + ")";
        return out;
    }

    const double norm = 1.0 - k;
    for (size_t i = 0; i < kMassSize; ++i) {
        out.combined[i] = norm > 1e-12 ? product[i] / norm : product[i];
    }
    out.suppressed = false;
    out.reason = "combined" + std::string(" (") + fmt("K", k) + ")";
    return out;
}

double DempsterShafer::belief(const MassFunction& m, Hypothesis h) {
    const size_t mask = singleton_mask(h);
    double total = 0.0;
    for (size_t s = 0; s < kMassSize; ++s) {
        // Only subsets OF {h} support h. The empty set is excluded by validity
        // (its mass is always 0), so this is simply m[{h}] for a singleton frame.
        if ((s & ~mask) == 0) total += m[s];
    }
    return total;
}

double DempsterShafer::plausibility(const MassFunction& m, Hypothesis h) {
    const size_t mask = singleton_mask(h);
    double total = 0.0;
    for (size_t s = 0; s < kMassSize; ++s) {
        if ((s & mask) != 0) total += m[s];  // any subset that is compatible with h
    }
    return total;
}

double DempsterShafer::ignorance_mass(const MassFunction& m) { return m[kThetaMask]; }

Hypothesis DempsterShafer::argmax_belief(const MassFunction& m, bool* tie) {
    Hypothesis best = Hypothesis::routine;
    double best_value = -1.0;
    double second = -1.0;
    const Hypothesis all[] = {Hypothesis::routine, Hypothesis::activity_change,
                             Hypothesis::environmental_anomaly};
    for (Hypothesis h : all) {
        const double b = belief(m, h);
        if (b > best_value) {
            second = best_value;
            best_value = b;
            best = h;
        } else if (b > second) {
            second = b;
        }
    }
    if (tie != nullptr) {
        *tie = (second >= 0.0) && (std::fabs(best_value - second) <= 1e-9);
    }
    return best;
}

bool DempsterShafer::valid(const MassFunction& m, std::string* why) {
    double sum = 0.0;
    for (size_t i = 0; i < kMassSize; ++i) {
        if (!std::isfinite(m[i])) {
            if (why) *why = "non-finite mass at index " + std::to_string(i);
            return false;
        }
        if (m[i] < 0.0) {
            if (why) *why = "negative mass at index " + std::to_string(i);
            return false;
        }
        sum += m[i];
    }
    if (m[0] != 0.0) {
        if (why) *why = "mass assigned to the empty set";
        return false;
    }
    if (std::fabs(sum - 1.0) > 1e-6) {
        if (why) *why = "masses sum to " + std::to_string(sum) + ", not 1";
        return false;
    }
    return true;
}

std::string DempsterShafer::summarize(const Combination& c, Hypothesis top) {
    std::string s = fmt("K", c.conflict_k);
    s += " belief=" + std::to_string(belief(c.combined, top));
    s += " plausibility=" + std::to_string(plausibility(c.combined, top));
    s += " ignorance=" + std::to_string(ignorance_mass(c.combined));
    s += std::string(" top=") + hypothesis_to_string(top);
    s += std::string(" suppressed=") + (c.suppressed ? "yes" : "no");
    return s;
}

}  // namespace homeguardian
