#include "backend/linux/PwAudioCaptureDevice.h"

#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstring>
#include <stdexcept>
#include <thread>

namespace homeguardian {

namespace {

constexpr int kPcmBytesPerSample = 2;  // s16le

}  // namespace

PwAudioCaptureDevice::PwAudioCaptureDevice(Config config) : config_(std::move(config)) {
    if (config_.sample_rate < 8000 || config_.sample_rate > 48000) {
        config_.sample_rate = 48000;  // the supported range; PipeWire converts the rest
    }
    if (config_.channels < 1 || config_.channels > 2) config_.channels = 1;
    if (config_.ring_capacity_frames < 1024) config_.ring_capacity_frames = 1024;
}

PwAudioCaptureDevice::~PwAudioCaptureDevice() {
    // Deterministic cleanup: a destructor must never leave a capture child running.
    stop();
    close();
}

std::vector<std::string> PwAudioCaptureDevice::capture_command_line() const {
    // pw-record writes raw PCM to stdout with --raw. parec does the same by default.
    std::vector<std::string> argv;
    argv.push_back(config_.capture_command);
    argv.push_back("--raw");
    argv.push_back("--format=" + config_.sample_format);
    argv.push_back("--rate=" + std::to_string(config_.sample_rate));
    argv.push_back("--channels=" + std::to_string(config_.channels));
    if (!config_.device.empty()) argv.push_back("--target=" + config_.device);
    argv.push_back("-");  // stdout
    return argv;
}

void PwAudioCaptureDevice::initialize() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (state_ == DeviceState::Capturing) throw std::runtime_error("already capturing");
    if (state_ == DeviceState::Error) throw std::runtime_error("device is in an error state: " + last_error_);
    state_ = DeviceState::Initialized;
}

void PwAudioCaptureDevice::authorize_capture(bool authorized) {
    capture_authorized_.store(authorized);
    if (!authorized) {
        // Revocation is immediate and unstoppable by design: the child is killed and the
        // ring is cleared so no sample captured under the previous grant can be read after
        // the grant is gone.
        stop();
        std::lock_guard<std::mutex> lock(mutex_);
        ring_.clear();
    }
}

void PwAudioCaptureDevice::spawn_capture_child(std::string* error) {
    int pipe_fds[2] = {-1, -1};
    if (::pipe(pipe_fds) != 0) {
        *error = std::string("pipe() failed: ") + std::strerror(errno);
        return;
    }
    const pid_t pid = ::fork();
    if (pid < 0) {
        ::close(pipe_fds[0]);
        ::close(pipe_fds[1]);
        *error = std::string("fork() failed: ") + std::strerror(errno);
        return;
    }
    if (pid == 0) {
        // Child: stdout becomes the pipe; stderr is silenced (the parent reports failures
        // through the exit status and its own diagnostics).
        ::dup2(pipe_fds[1], STDOUT_FILENO);
        ::close(pipe_fds[0]);
        ::close(pipe_fds[1]);
        const int devnull = ::open("/dev/null", O_WRONLY);
        if (devnull >= 0) {
            ::dup2(devnull, STDERR_FILENO);
            ::close(devnull);
        }
        const std::vector<std::string> argv = capture_command_line();
        std::vector<char*> raw;
        raw.reserve(argv.size() + 1);
        for (const std::string& arg : argv) raw.push_back(const_cast<char*>(arg.c_str()));
        raw.push_back(nullptr);
        ::execvp(raw[0], raw.data());
        _exit(127);  // exec failed
    }
    ::close(pipe_fds[1]);
    child_pid_ = static_cast<int>(pid);
    child_stdout_ = pipe_fds[0];
    const int flags = ::fcntl(child_stdout_, F_GETFL, 0);
    if (flags >= 0) ::fcntl(child_stdout_, F_SETFL, flags | O_NONBLOCK);
}

void PwAudioCaptureDevice::start() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!capture_authorized_.load()) {
        // FAIL CLOSED. Constructing this object is not authorization, and neither is the
        // OS granting the process access to a sound server.
        throw std::runtime_error(
            "microphone capture refused: no explicit operator authorization (call "
            "authorize_capture(true) after the operator's Start action)");
    }
    if (state_ == DeviceState::Capturing) return;
    if (state_ == DeviceState::Idle) state_ = DeviceState::Initialized;
    if (state_ == DeviceState::Error) throw std::runtime_error("device is in an error state");
    if (config_.dry_run) {
        state_ = DeviceState::Capturing;  // no child: the plan is reported instead
        return;
    }
    std::string why;
    spawn_capture_child(&why);
    if (child_pid_ < 0) {
        ++stats_.spawn_failures;
        last_error_ = why.empty() ? "capture child did not start" : why;
        state_ = DeviceState::Error;
        throw std::runtime_error("could not start microphone capture: " + last_error_);
    }
    state_ = DeviceState::Capturing;
}

void PwAudioCaptureDevice::pump_child_output() {
    if (child_stdout_ < 0) return;
    int16_t chunk[4096];
    for (;;) {
        const ssize_t n = ::read(child_stdout_, chunk, sizeof(chunk));
        if (n > 0) {
            const size_t samples = static_cast<size_t>(n) / kPcmBytesPerSample;
            for (size_t i = 0; i < samples; ++i) {
                ring_.push_back(chunk[i]);
                ++stats_.frames_captured;
                if (ring_.size() > config_.ring_capacity_frames * config_.channels) {
                    // BOUNDED: the oldest sample is dropped, never the newest, and the drop
                    // is counted so back-pressure is visible instead of invisible.
                    ring_.pop_front();
                    ++stats_.frames_dropped;
                }
            }
            continue;
        }
        if (n == 0) {
            // EOF: the capture child exited. Either a normal stop or device removal.
            int status = 0;
            const pid_t reaped = ::waitpid(child_pid_, &status, WNOHANG);
            if (reaped == child_pid_) {
                stats_.last_exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
            }
            ::close(child_stdout_);
            child_stdout_ = -1;
            child_pid_ = -1;
            ++stats_.disconnects;
            return;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) return;  // nothing more right now
        if (errno == EINTR) continue;
        last_error_ = std::string("read from the capture child failed: ") + std::strerror(errno);
        ++stats_.disconnects;
        return;
    }
}

size_t PwAudioCaptureDevice::read_frames(int16_t* out, size_t frames_requested) {
    if (out == nullptr || frames_requested == 0) return 0;
    if (frames_requested > kMaxFramesPerRead) frames_requested = kMaxFramesPerRead;
    std::lock_guard<std::mutex> lock(mutex_);
    if (state_ != DeviceState::Capturing) return 0;

    // A short wait so a caller polling at a modest rate does not spin: this is the only
    // place the capture path blocks, and it blocks briefly and boundedly.
    if (ring_.empty() && !config_.dry_run) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    pump_child_output();
    if (state_ == DeviceState::Capturing && child_pid_ < 0 && !config_.dry_run && ring_.empty()) {
        // The child is gone and nothing is buffered: report the device loss rather than
        // returning silence that would look like a quiet room.
        last_error_ = "the capture device stopped delivering samples (disconnected or released)";
        state_ = DeviceState::Error;
        return 0;
    }

    const size_t wanted = frames_requested * config_.channels;
    const size_t available = ring_.size() < wanted ? ring_.size() : wanted;
    for (size_t i = 0; i < available; ++i) {
        out[i] = ring_.front();
        ring_.pop_front();
    }
    const size_t frames = available / config_.channels;
    if (frames > 0) ++stats_.windows_read;
    return frames;
}

void PwAudioCaptureDevice::terminate_child() noexcept {
    if (child_pid_ > 0) {
        ::kill(child_pid_, SIGTERM);
        int status = 0;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
        for (;;) {
            const pid_t reaped = ::waitpid(child_pid_, &status, WNOHANG);
            if (reaped == child_pid_) break;
            if (std::chrono::steady_clock::now() >= deadline) {
                ::kill(child_pid_, SIGKILL);
                ::waitpid(child_pid_, &status, 0);
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        stats_.last_exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
        child_pid_ = -1;
    }
    if (child_stdout_ >= 0) {
        ::close(child_stdout_);
        child_stdout_ = -1;
    }
}

void PwAudioCaptureDevice::stop() noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    terminate_child();
    if (state_ == DeviceState::Capturing) state_ = DeviceState::Stopped;
}

void PwAudioCaptureDevice::close() noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    terminate_child();
    ring_.clear();
    state_ = DeviceState::Idle;
}

void PwAudioCaptureDevice::fail() noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    terminate_child();
    if (last_error_.empty()) last_error_ = "device failure was injected";
    state_ = DeviceState::Error;
}

void PwAudioCaptureDevice::set_error(const std::string& why) noexcept {
    last_error_ = why;
    state_ = DeviceState::Error;
}

DeviceState PwAudioCaptureDevice::state() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return state_;
}

std::string PwAudioCaptureDevice::name() const {
    std::string label = "microphone (PipeWire via " + config_.capture_command + ")";
    if (!config_.device.empty()) label += " [" + config_.device + "]";
    return label;
}

}  // namespace homeguardian
