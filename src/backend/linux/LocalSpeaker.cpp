#include "backend/linux/LocalSpeaker.h"

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
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

// A DIRECT synthesizer is preferred over Speech Dispatcher: it speaks without a daemon and
// without output modules, so there is one fewer process between us and the speaker. Speech
// Dispatcher installed WITHOUT a module accepts a request and produces silence while exiting 0,
// so it is only usable when a synthesizer is there to give it a voice - and in that case the
// synthesizer itself is already the better choice.
LocalSpeaker::SpeechEngine LocalSpeaker::choose_speech_engine(
    const std::function<bool(const std::string&)>& exists) {
    SpeechEngine engine;
    for (const char* candidate : {"espeak-ng", "espeak"}) {
        if (exists(candidate)) {
            engine.program = candidate;
            engine.available = true;
            engine.direct = true;
            return engine;
        }
    }
    if (exists("spd-say")) {
        engine.why = "spd-say is installed but no synthesizer backend (espeak-ng/espeak) is, so "
                     "Speech Dispatcher would accept the request and produce no audio";
        return engine;
    }
    engine.why = "no offline speech engine is installed (looked for espeak-ng, espeak, spd-say)";
    return engine;
}

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

void LocalSpeaker::cancel() noexcept {
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
    if (muted_.load()) {
        // Mute is explicit and independent of microphone consent: the operator may keep
        // listening while silencing the speaker, or the reverse.
        ++stats_.refused;
        return false;
    }
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
    const SpeechEngine engine = choose_speech_engine(command_exists);
    if (!engine.available) {
        if (why != nullptr) {
            *why = engine.why.empty() ? std::string("no speech engine is available") : engine.why;
        }
        return std::string();
    }
    return engine.program;
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
    // The text is a SINGLE argv element, so a semicolon, a quote or a backtick inside it can
    // never be interpreted by anything: there is no shell on this path, at all.
    const SpeechEngine engine = choose_speech_engine(command_exists);
    if (!engine.available) return argv;  // empty: nothing would run, and nothing is claimed
    const std::string voice = job.voice.empty() ? config_.voice : job.voice;
    const int rate = job.rate > 0 ? job.rate : (config_.rate > 0 ? config_.rate : 160);
    return build_speech_argv(engine, job, static_cast<int>(volume_limit() * 100), voice, rate);
}

std::vector<std::string> LocalSpeaker::build_speech_argv(const SpeechEngine& engine,
                                                         const Job& job, int volume_percent,
                                                         const std::string& voice, int rate) {
    std::vector<std::string> argv;
    if (!engine.available) return argv;
    // The text is a SINGLE argv element on every path. A semicolon, a quote, a backtick or a
    // newline inside it is just a character in a string handed to execvp - there is no shell.
    const std::string text = job.text.empty() ? std::string("<text>") : job.text;
    if (!engine.direct) {
        argv.push_back("spd-say");
        argv.push_back("--wait");
        argv.push_back("--volume=" + std::to_string(volume_percent));
        argv.push_back(text);
        return argv;
    }
    argv.push_back(engine.program);  // espeak-ng or espeak
    if (!voice.empty()) {
        argv.push_back("-v");
        argv.push_back(voice);
    }
    argv.push_back("-s");
    argv.push_back(std::to_string(rate > 0 ? rate : 160));
    argv.push_back("-a");
    argv.push_back(std::to_string(volume_percent));
    argv.push_back(text);
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
        // Same builder the preview uses, so what a test inspects is what runs.
        argv = command_line_for(job);
        if (argv.empty()) {
            *error = "no speech engine is available";
            note_failure(*error);
            return false;
        }
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
        // Aim the engine at the chosen sink WITHOUT a shell. espeak-ng reaches the sound server
        // through the PulseAudio-compatible layer, which reads PULSE_SINK; the native PipeWire
        // name is set as well so either path lands on the same device.
        if (job.kind == JobKind::speech && !config_.device.empty()) {
            ::setenv("PULSE_SINK", config_.device.c_str(), 1);
            ::setenv("PIPEWIRE_NODE", config_.device.c_str(), 1);
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
        note_failure(*error);
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(failure_mutex_);
        last_failure_.clear();  // a success clears the previous failure
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

bool LocalSpeaker::speech_available(std::string* why) const {
    return !engine_available(JobKind::speech, why).empty();
}

bool LocalSpeaker::speak_with_options(const std::string& text, int rate,
                                     const std::string& voice) {
    Job job;
    job.kind = JobKind::speech;
    job.text = text;
    job.rate = rate > 0 ? rate : (config_.rate > 0 ? config_.rate : 160);
    job.voice = voice;
    return enqueue(job);
}

void LocalSpeaker::note_failure(const std::string& failure) {
    std::lock_guard<std::mutex> lock(failure_mutex_);
    last_failure_ = failure;
}

// Run a probe WITHOUT a shell: fork + execvp, bounded read, bounded wait, always reaped.
std::string LocalSpeaker::run_probe(const std::vector<std::string>& argv, size_t max_bytes,
                                    int timeout_ms, int* exit_code) {
    if (exit_code != nullptr) *exit_code = -1;
    if (argv.empty()) return std::string();
    int pipe_fds[2];
    if (::pipe(pipe_fds) != 0) return std::string();
    const pid_t pid = ::fork();
    if (pid < 0) {
        ::close(pipe_fds[0]);
        ::close(pipe_fds[1]);
        return std::string();
    }
    if (pid == 0) {
        ::close(pipe_fds[0]);
        ::dup2(pipe_fds[1], STDOUT_FILENO);
        const int devnull = ::open("/dev/null", O_WRONLY);
        if (devnull >= 0) {
            ::dup2(devnull, STDERR_FILENO);
            ::close(devnull);
        }
        ::close(pipe_fds[1]);
        for (int fd = 3; fd < 1024; ++fd) ::close(fd);  // inherit nothing
        std::vector<char*> raw;
        raw.reserve(argv.size() + 1);
        for (const std::string& arg : argv) raw.push_back(const_cast<char*>(arg.c_str()));
        raw.push_back(nullptr);
        ::execvp(raw[0], raw.data());
        _exit(127);
    }
    ::close(pipe_fds[1]);
    std::string output;
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(timeout_ms > 0 ? timeout_ms : 500);
    char buffer[512];
    for (;;) {
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
                              deadline - std::chrono::steady_clock::now()).count();
        if (left <= 0) break;
        struct pollfd fds[1];
        fds[0].fd = pipe_fds[0];
        fds[0].events = POLLIN;
        fds[0].revents = 0;
        const int ready = ::poll(fds, 1, static_cast<int>(left > 200 ? 200 : left));
        if (ready < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (ready == 0) continue;
        const ssize_t n = ::read(pipe_fds[0], buffer, sizeof(buffer));
        if (n > 0) {
            const size_t room = output.size() < max_bytes ? max_bytes - output.size() : 0;
            const size_t take = static_cast<size_t>(n) < room ? static_cast<size_t>(n) : room;
            output.append(buffer, take);
            continue;
        }
        if (n == 0) break;  // EOF: the probe finished
        if (errno == EINTR) continue;
        break;
    }
    ::close(pipe_fds[0]);
    int status = 0;
    while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
    if (exit_code != nullptr) *exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    return output;
}

// A Bluetooth speaker that went away must be REPORTED, never assumed present and never fatal:
// the next request simply fails, is counted, and the speaker comes back on its own when the
// device returns.
bool LocalSpeaker::output_present(std::string* why) const {
    int rc = -1;
    if (config_.device.empty()) {
        const std::string out = run_probe({"pactl", "info"}, 4096, 2000, &rc);
        if (rc != 0 || out.find("Server Name") == std::string::npos) {
            if (why != nullptr) *why = "the sound server did not answer";
            return false;
        }
        return true;
    }
    const std::string sinks = run_probe({"pactl", "list", "short", "sinks"}, 8192, 2000, &rc);
    if (rc != 0) {
        if (why != nullptr) *why = "could not ask the sound server for its sinks";
        return false;
    }
    if (sinks.find(config_.device) == std::string::npos) {
        if (why != nullptr) *why = "the output " + config_.device + " is not present";
        return false;
    }
    return true;
}

LocalSpeaker::OutputStatus LocalSpeaker::get_output_status(bool probe_device) const {
    OutputStatus status;
    status.device = config_.device;
    status.muted = muted();
    status.busy = busy();
    status.queued = queued();
    status.volume_limit = volume_limit();
    status.tone_engine = engine_available(JobKind::tone, nullptr);
    status.tone_available = !status.tone_engine.empty();
    std::string why;
    status.speech_engine = engine_available(JobKind::speech, &why);
    status.speech_available = !status.speech_engine.empty();
    status.speech_why = why;
    status.played = stats_.played;
    status.dropped = stats_.dropped;
    status.refused = stats_.refused;
    {
        std::lock_guard<std::mutex> lock(failure_mutex_);
        status.last_failure = last_failure_;
    }
    if (probe_device) {
        std::string device_why;
        status.device_present = output_present(&device_why);
        status.device_why = device_why;
    }
    return status;
}

}  // namespace homeguardian
