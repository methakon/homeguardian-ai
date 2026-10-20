#include "BoundedFeatures.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace homeguardian {

namespace {

std::string u32(size_t v) { return std::to_string(static_cast<unsigned long long>(v)); }

}  // namespace

AudioFrameFeatures extract_audio_features(const int16_t* pcm, size_t frames,
                                          uint32_t channels, uint32_t sample_rate) {
    AudioFrameFeatures f;
    f.sample_frames = static_cast<uint32_t>(frames);
    f.channels = channels;
    f.sample_rate = sample_rate;

    if (pcm == nullptr) {
        f.reason = "null pcm buffer";
        return f;
    }
    if (frames < FeatureBounds::kMinAudioFrames) {
        f.reason = "too few frames (" + u32(frames) + " < " +
                   u32(FeatureBounds::kMinAudioFrames) + ")";
        return f;
    }
    if (frames > FeatureBounds::kMaxAudioFrames) {
        f.reason = "too many frames (" + u32(frames) + " > " +
                   u32(FeatureBounds::kMaxAudioFrames) + ")";
        return f;
    }
    if (channels == 0 || channels > FeatureBounds::kMaxChannels) {
        f.reason = "unsupported channel count (" + u32(channels) + ")";
        return f;
    }
    if (sample_rate < FeatureBounds::kMinSampleRate || sample_rate > FeatureBounds::kMaxSampleRate) {
        f.reason = "unsupported sample rate (" + u32(sample_rate) + ")";
        return f;
    }

    const size_t total = frames * channels;
    double sum_sq = 0.0;
    size_t peak = 0;
    size_t crossings = 0;
    bool have_previous = false;
    bool previous_positive = false;

    for (size_t i = 0; i < total; ++i) {
        const int32_t s = pcm[i];
        const size_t magnitude = static_cast<size_t>(s < 0 ? -s : s);
        if (magnitude > peak) peak = magnitude;
        sum_sq += static_cast<double>(s) * static_cast<double>(s);

        // Zero crossings are counted on the first channel only, so the rate is
        // comparable regardless of channel count.
        if (channels == 1 || (i % channels) == 0) {
            const bool positive = s >= 0;
            if (have_previous && positive != previous_positive) ++crossings;
            previous_positive = positive;
            have_previous = true;
        }
    }

    const double count = static_cast<double>(total);
    f.rms = std::sqrt(sum_sq / count) / 32768.0;
    f.peak = static_cast<double>(peak) / 32768.0;
    const size_t per_channel = frames;
    f.zero_crossing_rate =
        per_channel > 1 ? static_cast<double>(crossings) / static_cast<double>(per_channel - 1) : 0.0;

    // Clamp defensively: a caller cannot receive an out-of-range feature even if
    // the arithmetic above surprises us.
    if (f.rms < 0.0) f.rms = 0.0;
    if (f.rms > 1.0) f.rms = 1.0;
    if (f.peak < 0.0) f.peak = 0.0;
    if (f.peak > 1.0) f.peak = 1.0;
    if (f.zero_crossing_rate < 0.0) f.zero_crossing_rate = 0.0;
    if (f.zero_crossing_rate > 1.0) f.zero_crossing_rate = 1.0;

    f.valid = true;
    return f;
}

// ---------------------------------------------------------------------------------------------
// Region-aware change descriptors
// ---------------------------------------------------------------------------------------------

RegionChangeFeatures extract_region_change_features(const uint8_t* previous_luma,
                                                    const uint8_t* current_luma, size_t pixels,
                                                    uint32_t width, uint32_t height,
                                                    uint8_t delta_threshold, uint32_t grid_x,
                                                    uint32_t grid_y) {
    RegionChangeFeatures f;
    f.pixels = static_cast<uint32_t>(pixels);
    f.width = width;
    f.height = height;

    if (previous_luma == nullptr || current_luma == nullptr) {
        f.reason = "null luma plane";
        return f;
    }
    if (pixels < FeatureBounds::kMinPixels) {
        f.reason = "too few pixels (" + u32(pixels) + ")";
        return f;
    }
    if (pixels > FeatureBounds::kMaxPixels) {
        f.reason = "too many pixels (" + u32(pixels) + " > " + u32(FeatureBounds::kMaxPixels) + ")";
        return f;
    }
    if (width == 0 || height == 0 || static_cast<size_t>(width) * height != pixels) {
        f.reason = "geometry does not describe " + u32(pixels) + " pixel(s)";
        return f;
    }
    if (grid_x == 0 || grid_y == 0 || grid_x * grid_y > FeatureBounds::kMaxRegionCells) {
        f.reason = "unsupported grid (" + u32(grid_x) + "x" + u32(grid_y) + ")";
        return f;
    }

    std::vector<uint32_t> cell_counts(static_cast<size_t>(grid_x) * grid_y, 0u);
    uint32_t changed = 0;
    uint32_t brighter = 0;
    uint32_t darker = 0;
    double sum_x = 0.0;
    double sum_y = 0.0;
    double sum_xx = 0.0;
    double sum_yy = 0.0;
    double sum_d = 0.0;      // signed delta, over changed pixels only
    double sum_p = 0.0;      // previous value
    double sum_dd = 0.0;
    double sum_pp = 0.0;
    double sum_dp = 0.0;

    for (size_t i = 0; i < pixels; ++i) {
        const int diff = static_cast<int>(current_luma[i]) - static_cast<int>(previous_luma[i]);
        const int magnitude = diff < 0 ? -diff : diff;
        if (magnitude < static_cast<int>(delta_threshold)) continue;
        ++changed;
        if (diff > 0) {
            ++brighter;
        } else if (diff < 0) {
            ++darker;
        }
        const uint32_t x = static_cast<uint32_t>(i % width);
        const uint32_t y = static_cast<uint32_t>(i / width);
        const uint32_t cx = (x * grid_x) / width;
        const uint32_t cy = (y * grid_y) / height;
        const size_t cell = static_cast<size_t>(cy) * grid_x + (cx < grid_x ? cx : grid_x - 1);
        ++cell_counts[cell < cell_counts.size() ? cell : cell_counts.size() - 1];
        sum_x += static_cast<double>(x);
        sum_y += static_cast<double>(y);
        sum_xx += static_cast<double>(x) * static_cast<double>(x);
        sum_yy += static_cast<double>(y) * static_cast<double>(y);
        const double d = static_cast<double>(diff);
        const double pv = static_cast<double>(previous_luma[i]);
        sum_d += d;
        sum_p += pv;
        sum_dd += d * d;
        sum_pp += pv * pv;
        sum_dp += d * pv;
    }

    f.changed_pixels = changed;
    f.changed_fraction = static_cast<double>(changed) / static_cast<double>(pixels);

    if (changed > 0) {
        const uint32_t peak = *std::max_element(cell_counts.begin(), cell_counts.end());
        f.peak_cell_share = static_cast<double>(peak) / static_cast<double>(changed);
        size_t occupied = 0;
        for (uint32_t c : cell_counts) {
            if (c > 0) ++occupied;
        }
        f.occupied_cell_fraction = static_cast<double>(occupied) / static_cast<double>(cell_counts.size());
        f.brighter_share = static_cast<double>(brighter) / static_cast<double>(changed);
        f.darker_share = static_cast<double>(darker) / static_cast<double>(changed);
        const uint32_t low = brighter < darker ? brighter : darker;
        const uint32_t high = brighter < darker ? darker : brighter;
        if (high > 0) f.bipolar_balance = 2.0 * static_cast<double>(low) / static_cast<double>(high);
        const double n = static_cast<double>(changed);
        const double mean_x = sum_x / n;
        const double mean_y = sum_y / n;
        f.centroid_x = mean_x / static_cast<double>(width);
        f.centroid_y = mean_y / static_cast<double>(height);
        f.centroid_valid = true;

        const double var_x = std::max(0.0, sum_xx / n - mean_x * mean_x);
        const double var_y = std::max(0.0, sum_yy / n - mean_y * mean_y);
        const double n_d = static_cast<double>(changed);
        const double cov = n_d * sum_dp - sum_d * sum_p;
        const double var_d = n_d * sum_dd - sum_d * sum_d;
        const double var_p = n_d * sum_pp - sum_p * sum_p;
        if (var_d > 0.0 && var_p > 0.0) {
            f.gain_correlation = cov / std::sqrt(var_d * var_p);
        }

        const double diagonal = std::sqrt(static_cast<double>(width) * width +
                                          static_cast<double>(height) * height);
        if (diagonal > 0.0) {
            f.spread_radius = std::sqrt(var_x + var_y) / diagonal;
        }
    }

    // Bounded outputs, so a caller can never receive a value outside the documented range.
    auto clamp01 = [](double v) { return v < 0.0 ? 0.0 : (v > 1.0 ? 1.0 : v); };
    f.changed_fraction = clamp01(f.changed_fraction);
    f.peak_cell_share = clamp01(f.peak_cell_share);
    f.occupied_cell_fraction = clamp01(f.occupied_cell_fraction);
    f.brighter_share = clamp01(f.brighter_share);
    f.darker_share = clamp01(f.darker_share);
    f.bipolar_balance = clamp01(f.bipolar_balance);
    f.centroid_x = clamp01(f.centroid_x);
    f.centroid_y = clamp01(f.centroid_y);
    if (f.spread_radius < 0.0) f.spread_radius = 0.0;
    if (f.spread_radius > 1.0) f.spread_radius = 1.0;
    if (f.gain_correlation < -1.0) f.gain_correlation = -1.0;
    if (f.gain_correlation > 1.0) f.gain_correlation = 1.0;

    f.valid = true;
    return f;
}

MotionFrameFeatures extract_motion_features(const uint8_t* previous_luma,
                                            const uint8_t* current_luma, size_t pixels,
                                            uint8_t delta_threshold) {
    MotionFrameFeatures f;
    f.pixels = static_cast<uint32_t>(pixels);

    if (previous_luma == nullptr || current_luma == nullptr) {
        f.reason = "null luma plane";
        return f;
    }
    if (pixels < FeatureBounds::kMinPixels) {
        f.reason = "too few pixels (" + u32(pixels) + ")";
        return f;
    }
    if (pixels > FeatureBounds::kMaxPixels) {
        f.reason = "too many pixels (" + u32(pixels) + " > " + u32(FeatureBounds::kMaxPixels) + ")";
        return f;
    }

    size_t changed = 0;
    double total_delta = 0.0;
    for (size_t i = 0; i < pixels; ++i) {
        const int diff = static_cast<int>(current_luma[i]) - static_cast<int>(previous_luma[i]);
        const int magnitude = diff < 0 ? -diff : diff;
        total_delta += static_cast<double>(magnitude);
        if (magnitude >= static_cast<int>(delta_threshold)) ++changed;
    }

    f.changed_pixels = static_cast<uint32_t>(changed);
    f.changed_fraction = static_cast<double>(changed) / static_cast<double>(pixels);
    f.mean_delta = (total_delta / static_cast<double>(pixels)) / 255.0;
    if (f.changed_fraction < 0.0) f.changed_fraction = 0.0;
    if (f.changed_fraction > 1.0) f.changed_fraction = 1.0;
    if (f.mean_delta < 0.0) f.mean_delta = 0.0;
    if (f.mean_delta > 1.0) f.mean_delta = 1.0;

    f.valid = true;
    return f;
}

bool nv21_geometry_ok(size_t width, size_t height, size_t buffer_bytes) {
    if (width == 0 || height == 0) return false;
    if (width > 4096 || height > 4096) return false;
    const size_t pixels = width * height;
    if (pixels < FeatureBounds::kMinPixels || pixels > FeatureBounds::kMaxPixels) return false;
    // NV21 = Y plane (pixels) + interleaved VU (pixels/2), so 1.5 bytes/pixel.
    const size_t required = pixels + (pixels / 2);
    return buffer_bytes >= required;
}

const uint8_t* nv21_y_plane(const uint8_t* buffer) { return buffer; }

double bounded_mean(const double* values, size_t n) {
    if (values == nullptr || n == 0 || n > FeatureBounds::kMaxVectorDim * 1024) return 0.0;
    double sum = 0.0;
    for (size_t i = 0; i < n; ++i) sum += values[i];
    return sum / static_cast<double>(n);
}

double bounded_stddev(const double* values, size_t n) {
    if (values == nullptr || n < 2 || n > FeatureBounds::kMaxVectorDim * 1024) return 0.0;
    const double mean = bounded_mean(values, n);
    double acc = 0.0;
    for (size_t i = 0; i < n; ++i) {
        const double d = values[i] - mean;
        acc += d * d;
    }
    return std::sqrt(acc / static_cast<double>(n));
}

double cosine_similarity(const double* a, const double* b, size_t n) {
    if (a == nullptr || b == nullptr) return 0.0;
    if (n == 0 || n > FeatureBounds::kMaxVectorDim) return 0.0;
    double dot = 0.0;
    double na = 0.0;
    double nb = 0.0;
    for (size_t i = 0; i < n; ++i) {
        dot += a[i] * b[i];
        na += a[i] * a[i];
        nb += b[i] * b[i];
    }
    const double denom = std::sqrt(na) * std::sqrt(nb);
    if (!(denom > 1e-12)) return 0.0;  // zero-norm input: no direction to compare
    double value = dot / denom;
    if (value < -1.0) value = -1.0;
    if (value > 1.0) value = 1.0;
    return value;
}

double cosine_distance(const double* a, const double* b, size_t n) {
    return 1.0 - cosine_similarity(a, b, n);
}

bool vector_is_finite(const double* a, size_t n) {
    if (a == nullptr || n == 0) return false;
    for (size_t i = 0; i < n; ++i) {
        if (!std::isfinite(a[i])) return false;
    }
    return true;
}

}  // namespace homeguardian
