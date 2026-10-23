#include "backend/linux/PwAudioCaptureDevice.h"

#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <poll.h>

#include <cerrno>
#include <chrono>
#include <cstring>
#include <stdexcept>
#include <thread>

namespace homeguardian {

namespace {

constexpr int kPcmBytesPerSample = 2;  // s16le
constexpr size_t kMaxChildDiagnosticChars = 512;

bool looks_like_output_monitor(const std::string& device) {
    // An output MONITOR is a loopback of what is being played, not a microphone. Treating it
    // as room sound would report the local speaker as the environment - and would feed the
    // speaker's own announcements straight back into detection as a feedback loop.
    return device.size() >= 8 && device.compare(device.size() - 8, 8, ".monitor") == 0;
}

}  // namespace

PwAudioCaptureDevice::PwAudioCaptureDevice(Config config) : config_(std::move(config)) {
    if (config_.sample_rate < 8000 || config_.sample_rate > 48000) {
        config_.sample_rate = 48000;  // the supported range; PipeWire converts the rest
    }
    if (config_.channels < 1 || config_.channels > 2) config_.channels = 1;
    if (config_.ring_capacity_frames < 1024) config_.ring_capacity_frames = 1024;
    // Refuse an output monitor as a microphone, loudly, rather than reporting the desktop's
    // own playback as room activity.
    if (looks_like_output_monitor(config_.device)) {
        capture_refused_reason_ = "the configured device is an output monitor (a loopback of "
                                  "what the machine is playing), not a microphone";
    }
    if (config_.capture_command.find("pw-record") != std::string::npos &&
        config_.sample_format == "s16le") {
        config_.sample_format = "s16";  // pw-record uses the short format name
    }
}

PwAudioCaptureDevice::~PwAudioCaptureDevice() {
    // Deterministic cleanup: a destructor must never leave a capture child running.
    stop();
    close();
}

std::vector<std::string> PwAudioCaptureDevice::capture_command_line() const {
    // TOOL-AWARE. The two tools take different flags:
    //   parec     : --raw --format=s16le --rate=... --channels=... [--device=...]
    //   pw-record :           --format=s16   --rate=... --channels=... [--target=...] -
    // Passing --raw to pw-record makes it exit with "unrecognized option", and the session
    // then reports a stalled microphone - which is exactly what happened before this was
    // written down.
    std::vector<std::string> argv;
    argv.push_back(config_.capture_command);
    const bool is_pw_record = config_.capture_command.find("pw-record") != std::string::npos;
    if (!is_pw_record) argv.push_back("--raw");
    argv.push_back("--format=" + config_.sample_format);
    argv.push_back("--rate=" + std::to_string(config_.sample_rate));
    argv.push_back("--channels=" + std::to_string(config_.channels));
    if (!config_.device.empty()) {
        argv.push_back((is_pw_record ? "--target=" : "--device=") + config_.device);
    }
    if (is_pw_record) argv.push_back("-");  // pw-record needs the output named explicitly
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
    int err_pipe[2] = {-1, -1};
    if (::pipe(pipe_fds) != 0) {
        *error = std::string("pipe() failed: ") + std::strerror(errno);
        return;
    }
    if (::pipe(err_pipe) != 0) {
        ::close(pipe_fds[0]);
        ::close(pipe_fds[1]);
        *error = std::string("stderr pipe() failed: ") + std::strerror(errno);
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
        // Child: stdout becomes the PCM pipe; stderr becomes a SEPARATE pipe so the parent
        // can report what the tool said. Silencing stderr made a one-line "unrecognized
        // option" look like a dead microphone and cost a full debugging cycle.
        ::dup2(pipe_fds[1], STDOUT_FILENO);
        ::close(pipe_fds[0]);
        ::close(pipe_fds[1]);
        if (err_pipe[1] >= 0) {
            ::dup2(err_pipe[1], STDERR_FILENO);
            ::close(err_pipe[0]);
            ::close(err_pipe[1]);
        }
        // CLOSE EVERY INHERITED DESCRIPTOR above stderr.
        //
        // A forked child inherits the parent's whole descriptor table. The Python harness that
        // streams fine sets close_fds=True; this adapter's child inherited everything I hold,
        // and that difference is what made the capture tool exit cleanly after exactly one pipe
        // buffer while the identical command streamed from a shell. Child processes get the
        // descriptors they need (0/1/2) and nothing else.
        for (int fd = 3; fd < 1024; ++fd) ::close(fd);

        const std::vector<std::string> argv = capture_command_line();
        std::vector<char*> raw;
        raw.reserve(argv.size() + 1);
        for (const std::string& arg : argv) raw.push_back(const_cast<char*>(arg.c_str()));
        raw.push_back(nullptr);
        ::execvp(raw[0], raw.data());
        _exit(127);  // exec failed
    }
    ::close(pipe_fds[1]);
    ::close(err_pipe[1]);
    child_pid_ = static_cast<int>(pid);
    child_stdout_ = pipe_fds[0];
    child_stderr_fd_ = err_pipe[0];
    // DELIBERATELY no fcntl(O_NONBLOCK) here: the read description is shared with the child
    // through dup2, so setting it would also make the CHILD's writes non-blocking and stop the
    // tool after its first pipe buffer. The reader thread uses poll() for readiness instead.
}

void PwAudioCaptureDevice::start() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!capture_refused_reason_.empty()) {
        throw std::runtime_error("microphone capture refused: " + capture_refused_reason_);
    }
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
    reader_stop_ = false;
    reader_done_ = false;
    reader_ = std::thread([this] { reader_loop(); });
    state_ = DeviceState::Capturing;
}

void PwAudioCaptureDevice::reader_loop() {
    // Blocking reads, readiness from poll(). The child's descriptors keep their normal
    // blocking semantics, so ITS writes block when the pipe is full - which is what a
    // producer/consumer pair is supposed to do - instead of failing after 64 KB.
    int16_t chunk[4096];
    while (!reader_stop_.load()) {
        if (child_stdout_ < 0) break;
        struct pollfd fds[2];
        fds[0].fd = child_stdout_;
        fds[0].events = POLLIN;
        fds[0].revents = 0;
        fds[1].fd = child_stderr_fd_;
        fds[1].events = POLLIN;
        fds[1].revents = 0;
        const nfds_t count = child_stderr_fd_ >= 0 ? 2 : 1;
        const int ready = ::poll(fds, count, 100);
        if (ready < 0) {
            if (errno == EINTR) continue;
            last_error_ = std::string("poll() on the capture child failed: ") + std::strerror(errno);
            break;
        }
        if (ready == 0) continue;  // nothing yet: re-check the stop flag
        if (count == 2 && (fds[1].revents & (POLLIN | POLLHUP))) drain_child_stderr();
        if (!(fds[0].revents & (POLLIN | POLLHUP))) continue;

        const ssize_t n = ::read(child_stdout_, chunk, sizeof(chunk));
        if (n > 0) {
            const size_t samples = static_cast<size_t>(n) / kPcmBytesPerSample;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                for (size_t i = 0; i < samples; ++i) {
                    ring_.push_back(chunk[i]);
                    ++stats_.frames_captured;
                    if (ring_.size() > config_.ring_capacity_frames * config_.channels) {
                        // BOUNDED: the oldest sample is dropped, never the newest, and the
                        // drop is counted so back-pressure is visible instead of invisible.
                        ring_.pop_front();
                        ++stats_.frames_dropped;
                    }
                }
            }
            ring_cv_.notify_all();
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
            drain_child_stderr();
            if (child_stderr_fd_ >= 0) {
                ::close(child_stderr_fd_);
                child_stderr_fd_ = -1;
            }
            child_pid_ = -1;
            ++stats_.disconnects;
            if (!child_stderr_.empty()) {
                last_error_ = "capture child exited (status " +
                              std::to_string(stats_.last_exit_code) + "): " + child_stderr_;
            }
            break;
        }
        if (errno == EINTR) continue;
        last_error_ = std::string("read from the capture child failed: ") + std::strerror(errno);
        ++stats_.disconnects;
        break;
    }
    reader_done_.store(true);
    ring_cv_.notify_all();  // wake a consumer waiting for samples that will never come
}

void PwAudioCaptureDevice::drain_child_stderr() {
    // ALWAYS drain the child's stderr. A diagnostic pipe nobody reads is a pipe that FILLS,
    // and a process blocked writing its warnings is a process no longer writing PCM: the
    // capture then looks like a microphone that died after exactly one pipe buffer while the
    // child is alive and silent about it. Keep the first bounded chunk for the operator and
    // discard the rest - diagnostics must never be able to stall the data path.
    if (child_stderr_fd_ < 0) return;
    char buffer[1024];
    for (;;) {
        const ssize_t n = ::read(child_stderr_fd_, buffer, sizeof(buffer));
        if (n > 0) {
            if (child_stderr_.size() < kMaxChildDiagnosticChars) {
                const size_t room = kMaxChildDiagnosticChars - child_stderr_.size();
                child_stderr_.append(buffer, static_cast<size_t>(n) < room ? static_cast<size_t>(n)
                                                                           : room);
            }
            stats_.child_stderr_bytes += static_cast<uint64_t>(n);
            continue;
        }
        if (n == 0) return;  // the child closed it
        if (errno == EINTR) continue;
        return;  // EAGAIN / EWOULDBLOCK / anything else: nothing more to take right now
    }
}

size_t PwAudioCaptureDevice::read_frames(int16_t* out, size_t frames_requested) {
    if (out == nullptr || frames_requested == 0) return 0;
    if (frames_requested > kMaxFramesPerRead) frames_requested = kMaxFramesPerRead;
    // unique_lock, not lock_guard: the bounded wait below must be able to release the mutex
    // while it waits, and only unique_lock can do that.
    std::unique_lock<std::mutex> lock(mutex_);
    if (state_ != DeviceState::Capturing) return 0;

    // BOUNDED WAIT on the ring. The READER THREAD owns the pipes, so this function never
    // touches a descriptor and never sleeps in a poll loop - it waits for data or for the
    // reader to finish. The first read after start() gets a longer grace period because a
    // capture tool needs time to deliver its first buffer.
    if (!config_.dry_run && ring_.empty() && !reader_done_.load()) {
        const int wait_budget_ms = first_read_ ? config_.first_read_grace_ms : config_.read_wait_ms;
        ring_cv_.wait_for(lock,
                          std::chrono::milliseconds(wait_budget_ms > 0 ? wait_budget_ms : 200),
                          [this] { return !ring_.empty() || reader_done_.load(); });
    }
    first_read_ = false;
    if (state_ == DeviceState::Capturing && reader_done_.load() && !config_.dry_run &&
        ring_.empty()) {
        // The reader finished and nothing is buffered: report the device loss rather than
        // returning silence that would look like a quiet room.
        if (last_error_.empty()) {
            last_error_ = "the capture device stopped delivering samples (disconnected or released)";
        }
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

void PwAudioCaptureDevice::stop_reader() noexcept {
    // Signal first, then join. The join must NOT happen while the caller holds mutex_, or a
    // reader waiting for that mutex could deadlock the joiner.
    reader_stop_.store(true);
    ring_cv_.notify_all();
    if (reader_.joinable()) reader_.join();
}

void PwAudioCaptureDevice::stop() noexcept {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        terminate_child();  // kills the child, so a blocking read returns EOF
        if (state_ == DeviceState::Capturing) state_ = DeviceState::Stopped;
    }
    stop_reader();
}

void PwAudioCaptureDevice::close() noexcept {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        terminate_child();
        ring_.clear();
        state_ = DeviceState::Idle;
    }
    stop_reader();
}

void PwAudioCaptureDevice::fail() noexcept {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        terminate_child();
        if (last_error_.empty()) last_error_ = "device failure was injected";
        state_ = DeviceState::Error;
    }
    stop_reader();
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
