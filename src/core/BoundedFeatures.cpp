#include "BoundedFeatures.h"

#include <cmath>

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
