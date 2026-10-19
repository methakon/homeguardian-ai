#include "core/ActivityPattern.h"

#include <algorithm>
#include <cmath>
#include <numeric>
#include <iomanip>
#include <sstream>

namespace homeguardian {

const char* activity_pattern_to_string(ActivityPattern p) {
    switch (p) {
        case ActivityPattern::quiet:              return "quiet";
        case ActivityPattern::isolated_change:    return "isolated_change";
        case ActivityPattern::repeated_localized: return "repeated_localized";
        case ActivityPattern::sustained_activity: return "sustained_activity";
        case ActivityPattern::erratic:            return "erratic";
    }
    return "quiet";
}

ActivityPatternFinding characterise_activity_pattern(const std::vector<ActivitySample>& samples,
                                                    const ActivityPatternConfig& config) {
    ActivityPatternFinding finding;
    finding.uncertainty =
        "a motion-pattern description only: it does not identify a species, a size class or a cause, and "
        "a repeated small moving change is equally consistent with a small animal, an insect, a curtain in "
        "a draught or a shadow";

    if (!config.enabled) {
        finding.reason = "the activity-pattern analysis is opt-in and is not enabled";
        return finding;
    }
    if (samples.empty()) {
        finding.reason = "no samples: there is nothing to characterise";
        return finding;
    }

    // Bounded: the most recent samples only, so a long session cannot change the shape of the analysis.
    const size_t take = std::min(samples.size(), config.max_samples);
    const std::vector<ActivitySample> window(samples.end() - static_cast<long>(take), samples.end());
    finding.samples_used = window.size();

    // What counts as an episode is the character classifier's own rule, not the bare floor: a change is
    // NOT an event only when it is small AND spread. Requiring the floor alone would make the single case
    // this project most wants to keep - a small COMPACT change, i.e. a small body - invisible, which is
    // the mistake the classifier's quiet rule exists to avoid.
    const auto is_episode = [&config](const ActivitySample& s) {
        return s.changed_fraction >= config.episode_min_fraction ||
               s.spread_radius <= config.compact_radius_max;
    };
    std::vector<double> amplitudes;
    bool sustained = false;
    for (const auto& s : window) {
        if (s.changed_fraction >= config.sustained_fraction) sustained = true;
        if (is_episode(s)) amplitudes.push_back(s.changed_fraction);
    }
    finding.episodes = amplitudes.size();

    if (sustained) {
        finding.pattern = ActivityPattern::sustained_activity;
        finding.confidence = 0.9;
        finding.valid = true;
        std::ostringstream os;
        os << "a change of at least " << std::fixed << std::setprecision(2) << config.sustained_fraction
           << " of the frame was measured in the window";
        finding.description = os.str();
        return finding;
    }
    if (amplitudes.empty()) {
        finding.pattern = ActivityPattern::quiet;
        finding.confidence = 0.9;
        finding.valid = true;
        finding.description = "no change above the quiet floor in the window";
        return finding;
    }

    const double mean = std::accumulate(amplitudes.begin(), amplitudes.end(), 0.0) /
                        static_cast<double>(amplitudes.size());
    finding.mean_amplitude = mean;

    // A repeat is an episode that is small, compact, travelling, and within tolerance of the mean
    // amplitude. All four conditions are required: dropping any one of them would call a person walking
    // past, or a stationary flicker, a repetition.
    // "Repeats" means episodes that resemble EACH OTHER. With a single episode there is nothing to
    // resemble: it would trivially match its own mean amplitude and be counted as a repetition.
    size_t repeats = 0;
    for (const auto& s : window) {
        if (amplitudes.size() < 2) break;
        if (!is_episode(s)) continue;
        const bool small = s.changed_fraction <= config.small_fraction_max;
        const bool compact = s.spread_radius <= config.compact_radius_max;
        const bool travels = s.region_travel >= config.travel_min;
        const bool alike = std::fabs(s.changed_fraction - mean) <= config.amplitude_tolerance * mean;
        if (small && compact && travels && alike) ++repeats;
    }
    finding.repeats = repeats;

    // Consistency of the amplitudes, bounded: 1 when every episode is identical, 0 when the spread is as
    // large as the mean.
    double spread = 0.0;
    for (const double a : amplitudes) spread += std::fabs(a - mean);
    spread /= static_cast<double>(amplitudes.size());
    const double consistency = mean > 0.0 ? std::max(0.0, 1.0 - std::min(1.0, spread / mean)) : 0.0;

    if (repeats >= config.min_repeats) {
        finding.pattern = ActivityPattern::repeated_localized;
        finding.confidence = std::min(1.0, 0.5 * std::min(1.0, static_cast<double>(repeats) /
                                                                  (2.0 * static_cast<double>(
                                                                             config.min_repeats))) +
                                           0.5 * consistency);
    } else if (finding.episodes == 1) {
        finding.pattern = ActivityPattern::isolated_change;
        finding.confidence = 0.5;
    } else {
        finding.pattern = ActivityPattern::erratic;
        finding.confidence = consistency;
    }
    finding.valid = true;

    std::ostringstream os;
    os << finding.episodes << " change(s) above the " << std::fixed << std::setprecision(3)
       << config.episode_min_fraction << " floor, mean amplitude " << mean << "; " << repeats
       << " were small, compact, travelling and within " << std::setprecision(0)
       << config.amplitude_tolerance * 100.0 << "% of each other";
    finding.description = os.str();
    return finding;
}

const char* audio_activity_pattern_to_string(AudioActivityPattern p) {
    switch (p) {
        case AudioActivityPattern::quiet:           return "quiet";
        case AudioActivityPattern::isolated_event:  return "isolated_event";
        case AudioActivityPattern::repeated_events: return "repeated_events";
        case AudioActivityPattern::sustained_level: return "sustained_level";
        case AudioActivityPattern::varying:         return "varying";
    }
    return "quiet";
}

AudioPatternFinding characterise_audio_activity(const std::vector<AudioActivitySample>& samples,
                                               const AudioPatternConfig& config) {
    AudioPatternFinding finding;
    finding.uncertainty =
        "a level-and-repetition description only: not speech, not a keyword, not a species and not a sound "
        "classification, and it does not distinguish a door, a tap, a voice or an animal from each other";

    if (!config.enabled) {
        finding.reason = "the audio-pattern analysis is opt-in and is not enabled";
        return finding;
    }
    if (samples.empty()) {
        finding.reason = "no samples: there is nothing to characterise";
        return finding;
    }

    const size_t take = std::min(samples.size(), config.max_samples);
    const std::vector<AudioActivitySample> window(samples.end() - static_cast<long>(take), samples.end());
    finding.samples_used = window.size();

    std::vector<double> events;
    size_t audible = 0;
    for (const auto& s : window) {
        if (s.rms >= config.floor_rms) ++audible;
        if (s.rms >= config.event_min_rms) events.push_back(s.rms);
    }
    finding.events = events.size();
    finding.occupancy = static_cast<double>(audible) / static_cast<double>(window.size());

    if (finding.occupancy >= config.sustained_occupancy) {
        finding.pattern = AudioActivityPattern::sustained_level;
        finding.confidence = 0.9;
        finding.valid = true;
        std::ostringstream os;
        os << "the level stayed above the " << std::fixed << std::setprecision(3) << config.floor_rms
           << " floor in " << std::setprecision(2) << finding.occupancy * 100.0 << "% of the window";
        finding.description = os.str();
        return finding;
    }
    if (events.empty()) {
        finding.pattern = AudioActivityPattern::quiet;
        finding.confidence = 0.9;
        finding.valid = true;
        finding.description = "nothing above the floor in the window";
        return finding;
    }

    std::vector<double> sorted = events;
    std::sort(sorted.begin(), sorted.end());
    const double median = sorted[sorted.size() / 2];
    finding.mean_event_rms = std::accumulate(events.begin(), events.end(), 0.0) /
                             static_cast<double>(events.size());

    if (events.size() == 1) {
        finding.pattern = AudioActivityPattern::isolated_event;
        finding.confidence = 0.5;
        finding.valid = true;
        finding.description = "one sound above the floor, and no repetition";
        return finding;
    }

    // A repeat needs a similar LEVEL and a similar SPACING-INDEPENDENT count: two loud events of wildly
    // different levels are not a repetition, and one event cannot repeat.
    size_t repeats = 0;
    for (const double level : events) {
        if (std::fabs(level - median) <= config.similar_ratio * median) ++repeats;
    }
    finding.repeats = repeats;

    // ... and the event SET must itself be tight: mean absolute deviation relative to the median level.
    // Without this, an alternating loud/quiet burst pattern counts as a repetition because half the events
    // sit near the median, however far the other half is from it.
    double deviation = 0.0;
    for (const double level : events) deviation += std::fabs(level - median);
    deviation /= static_cast<double>(events.size());
    const double dispersion = median > 0.0 ? deviation / median : 1.0;
    const bool levels_alike = dispersion <= config.similar_ratio;

    if (repeats >= config.min_repeats && levels_alike) {
        finding.pattern = AudioActivityPattern::repeated_events;
        finding.confidence = std::min(1.0, 0.5 * std::min(1.0, static_cast<double>(repeats) /
                                                                  (2.0 * static_cast<double>(
                                                                             config.min_repeats))) +
                                           0.5 * std::min(1.0, static_cast<double>(repeats) /
                                                                   static_cast<double>(events.size())));
    } else {
        finding.pattern = AudioActivityPattern::varying;
        finding.confidence = 0.4;
    }
    // (dispersion is stated in the description below so a reader can see why the verdict was varying)
    finding.valid = true;
    std::ostringstream os;
    os << finding.events << " sound(s) above the " << std::fixed << std::setprecision(3)
       << config.event_min_rms << " event level, " << repeats << " of them within "
       << std::setprecision(0) << config.similar_ratio * 100.0 << "% of the median level "
       << std::setprecision(3) << median << " (dispersion " << deviation / (median > 0.0 ? median : 1.0)
       << ")";
    finding.description = os.str();
    return finding;
}

}  // namespace homeguardian
