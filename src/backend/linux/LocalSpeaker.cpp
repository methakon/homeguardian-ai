#include "backend/linux/LocalSpeaker.h"

#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cmath>
#include <cstring>
#include <fstream>
#include <stdexcept>

namespace homeguardian {

namespace {

void put_u32le(std::ofstream& out, uint32_t v) {
    const char bytes[4] = {static_cast<char>(v & 0xFF), static_cast<char>((v >> 8) & 0xFF),
                           static_cast<char>((v >> 16) & 0xFF), static_cast<char>((v >> 24) & 0xFF)};
    out.write(bytes, 4);
}

void put_u16le(std::ofstream& out, uint16_t v) {
    const char bytes[2] = {static_cast<char>(v & 0xFF), static_cast<char>((v >> 8) & 0xFF)};
    out.write(bytes, 2);
}

bool command_exists(const std::string& name) {
    const std::string path = std::string("/usr/bin/") + name;
    return ::access(path.c_str(), X_OK) == 0;
}

}  // namespace

LocalSpeaker::LocalSpeaker(Config config) : config_(std::move(config)) {
    if (config_.queue_capacity == 0) config_.queue_capacity = 1;
    set_volume_limit(config_.volume_limit);
}

LocalSpeaker::~LocalSpeaker() {
    stop();
}

void LocalSpeaker::set_volume_limit(double limit) {
    if (!(limit >= 0.0)) limit = 0.0;          // also catches NaN
    if (limit > kMaxVolumeLimit) limit = kMaxVolumeLimit;
    volume_limit_.store(limit);
}

void LocalSpeaker::start() {
    if (running_.load()) return;
    stop_requested_.store(false);
    running_.store(true);
    worker_ = std::thread([this] { worker_loop(); });
}

void LocalSpeaker::stop() noexcept {
    stop_requested_.store(true);
    cancel();
    cv_.notify_all();
    if (worker_.joinable()) worker_.join();
    running_.store(false);
}

size_t LocalSpeaker::queued() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return queue_.size();
}

void LocalSpeaker::cancel() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        queue_.clear();
    }
    const int child = current_child_.exchange(-1);
    if (child > 0) {
        ::kill(child, SIGTERM);
        ++stats_.cancelled;
    }
}

bool LocalSpeaker::enqueue(const Job& job) {
    if (microphone_active_.load()) {
        // Feedback guard: refuse rather than squeal into a live microphone.
        ++stats_.refused;
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (queue_.size() >= config_.queue_capacity) {
            queue_.pop_front();  // BOUNDED: oldest dropped, and counted
            ++stats_.dropped;
        }
        queue_.push_back(job);
    }
    cv_.notify_one();
    return true;
}

bool LocalSpeaker::play_tone(double frequency_hz, uint32_t duration_ms) {
    Job job;
    job.kind = JobKind::tone;
    job.frequency_hz = frequency_hz;
    job.duration_ms = duration_ms;
    return enqueue(job);
}

bool LocalSpeaker::speak(const std::string& text) {
    Job job;
    job.kind = JobKind::speech;
    job.text = text;
    return enqueue(job);
}

std::string LocalSpeaker::engine_name(JobKind kind) const {
    if (kind == JobKind::tone) return "homeguardian-generated-tone";
    return "speech-dispatcher(spd-say)";
}

std::string LocalSpeaker::engine_available(JobKind kind, std::string* why) const {
    if (kind == JobKind::tone) {
        if (!command_exists(config_.player_command)) {
            if (why != nullptr) *why = config_.player_command + " is not installed";
            return std::string();
        }
        return config_.player_command;
    }
    if (!command_exists("spd-say")) {
        if (why != nullptr) *why = "spd-say is not installed";
        return std::string();
    }
    if (!command_exists("espeak-ng") && !command_exists("espeak")) {
        // Speech Dispatcher without a voice backend accepts the request and produces
        // nothing, which is exactly the kind of silent no-op this project refuses to call a
        // success. Report it as unavailable instead.
        if (why != nullptr) {
            *why = "no voice backend is installed (espeak-ng/espeak absent), so spd-say "
                   "would produce no audio";
        }
        return std::string();
    }
    return "spd-say";
}

std::vector<std::string> LocalSpeaker::command_line_for(const Job& job) const {
    std::vector<std::string> argv;
    if (job.kind == JobKind::tone) {
        argv.push_back(config_.player_command);
        argv.push_back("--playback");
        if (!config_.device.empty()) argv.push_back("--target=" + config_.device);
        argv.push_back("<generated-tone.wav>");
        return argv;
    }
    argv.push_back("spd-say");
    argv.push_back("--wait");
    argv.push_back("--volume=" + std::to_string(static_cast<int>(volume_limit() * 100)));
    argv.push_back("<text>");
    return argv;
}

std::string LocalSpeaker::write_tone_wav(double frequency_hz, uint32_t duration_ms) const {
    const uint32_t sample_rate = 48000;
    const uint32_t frames = static_cast<uint32_t>((static_cast<uint64_t>(sample_rate) * duration_ms) / 1000);
    const std::string path = config_.scratch_dir + "/hg_tone_" + std::to_string(::getpid()) + "_" +
                             std::to_string(static_cast<int>(frequency_hz)) + ".wav";
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) return std::string();
    const uint32_t data_bytes = frames * 2;
    out.write("RIFF", 4);
    put_u32le(out, 36 + data_bytes);
    out.write("WAVEfmt ", 8);
    put_u32le(out, 16);
    put_u16le(out, 1);                                  // PCM
    put_u16le(out, 1);                                  // mono
    put_u32le(out, sample_rate);
    put_u32le(out, sample_rate * 2);                    // byte rate
    put_u16le(out, 2);                                  // block align
    put_u16le(out, 16);                                 // bits
    out.write("data", 4);
    put_u32le(out, data_bytes);
    const double amplitude = 0.25 * volume_limit();     // quiet by construction
    for (uint32_t i = 0; i < frames; ++i) {
        // Fade in/out so the tone cannot click, and keep it well under full scale.
        const double t = static_cast<double>(i) / sample_rate;
        const double fade = std::min(1.0, std::min(i, frames - i) / (sample_rate * 0.01));
        const double sample = std::sin(2.0 * M_PI * frequency_hz * t) * amplitude * fade;
        put_u16le(out, static_cast<uint16_t>(static_cast<int16_t>(sample * 32767.0)));
    }
    out.flush();
    return out ? path : std::string();
}

bool LocalSpeaker::run_job(const Job& job, std::string* error) {
    std::vector<std::string> argv;
    std::string tone_path;
    if (job.kind == JobKind::tone) {
        tone_path = write_tone_wav(job.frequency_hz, job.duration_ms);
        if (tone_path.empty()) {
            *error = "could not write the generated tone";
            return false;
        }
        argv.push_back(config_.player_command);
        argv.push_back("--playback");
        if (!config_.device.empty()) argv.push_back("--target=" + config_.device);
        argv.push_back(tone_path);
    } else {
        std::string why;
        if (engine_available(JobKind::speech, &why).empty()) {
            *error = why;
            return false;
        }
        argv.push_back("spd-say");
        argv.push_back("--wait");
        argv.push_back("--volume=" + std::to_string(static_cast<int>(volume_limit() * 100)));
        argv.push_back(job.text);
    }

    const pid_t pid = ::fork();
    if (pid < 0) {
        *error = std::string("fork() failed: ") + std::strerror(errno);
        return false;
    }
    if (pid == 0) {
        const int devnull = ::open("/dev/null", O_WRONLY);
        if (devnull >= 0) {
            ::dup2(devnull, STDOUT_FILENO);
            ::dup2(devnull, STDERR_FILENO);
            ::close(devnull);
        }
        std::vector<char*> raw;
        raw.reserve(argv.size() + 1);
        for (const std::string& arg : argv) raw.push_back(const_cast<char*>(arg.c_str()));
        raw.push_back(nullptr);
        ::execvp(raw[0], raw.data());
        _exit(127);
    }
    current_child_.store(static_cast<int>(pid));
    playing_.store(true);
    int status = 0;
    while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
    playing_.store(false);
    current_child_.store(-1);
    stats_.last_exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    if (!tone_path.empty()) ::unlink(tone_path.c_str());  // never leave the fixture behind
    if (stats_.last_exit_code != 0) {
        *error = "playback command exited with status " + std::to_string(stats_.last_exit_code);
        return false;
    }
    return true;
}

void LocalSpeaker::worker_loop() {
    while (!stop_requested_.load()) {
        Job job;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait_for(lock, std::chrono::milliseconds(200),
                         [this] { return stop_requested_.load() || !queue_.empty(); });
            if (queue_.empty()) continue;
            job = queue_.front();
            queue_.pop_front();
        }
        std::string error;
        if (run_job(job, &error)) {
            ++stats_.played;
        } else {
            ++stats_.refused;
        }
    }
}

}  // namespace homeguardian
