#include "backend/linux/AlsaCommandAudioSource.h"

#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <sstream>

namespace homeguardian {

CommandResult run_command_piped(const std::vector<std::string>& argv, size_t max_bytes) {
    CommandResult result;
    if (argv.empty()) {
        result.error = "empty command";
        return result;
    }

    int fds[2];
    if (pipe(fds) != 0) {
        result.error = std::string("pipe: ") + std::strerror(errno);
        return result;
    }
    const pid_t pid = fork();
    if (pid < 0) {
        close(fds[0]);
        close(fds[1]);
        result.error = std::string("fork: ") + std::strerror(errno);
        return result;
    }
    if (pid == 0) {
        dup2(fds[1], STDOUT_FILENO);          // stdout only; stderr stays for the caller to see
        close(fds[0]);
        close(fds[1]);
        std::vector<char*> args;
        args.reserve(argv.size() + 1);
        for (const auto& a : argv) args.push_back(const_cast<char*>(a.c_str()));
        args.push_back(nullptr);
        execvp(args[0], args.data());
        _exit(127);                            // 127 = "command not found", as a shell would report
    }

    close(fds[1]);
    std::string out;
    char buffer[65536];
    ssize_t n = 0;
    while ((n = read(fds[0], buffer, sizeof(buffer))) > 0) {
        out.append(buffer, static_cast<size_t>(n));
        if (out.size() > max_bytes) {          // a stream that never ends is cut off, not accumulated
            result.truncated = true;
            break;
        }
    }
    close(fds[0]);

    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
    result.exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    result.output = std::move(out);
    return result;
}

AlsaCommandAudioSource::AlsaCommandAudioSource(Config cfg) : cfg_(std::move(cfg)) {}

bool AlsaCommandAudioSource::open(std::string* error) {
    // `arecord -l` is read-only and lists capture hardware; a machine with no capture device is a
    // documented blocker rather than a silent failure at read time.
    const CommandResult listed = cfg_.runner({cfg_.command, "-l"}, 64u * 1024u);
    if (listed.exit_code != 0) {
        if (error != nullptr) {
            *error = "the capture tool '" + cfg_.command + "' is unavailable (exit " +
                     std::to_string(listed.exit_code) + ")" +
                     (listed.truncated ? " [output truncated]" : "");
        }
        return false;
    }
    if (listed.output.find("card ") == std::string::npos) {
        if (error != nullptr) *error = "no capture hardware is present";
        return false;
    }
    if (cfg_.seconds == 0 || cfg_.channels == 0 || cfg_.sample_rate == 0) {
        if (error != nullptr) *error = "the window is unbounded or the PCM format is invalid";
        return false;
    }
    // Fail here rather than at read time, and name the bound: a window longer than the extractor accepts
    // would be refused downstream as "too many frames", which reads like a device fault.
    const size_t window_frames = static_cast<size_t>(cfg_.seconds) * cfg_.sample_rate;
    if (window_frames > FeatureBounds::kMaxAudioFrames) {
        if (error != nullptr) {
            *error = "a window of " + std::to_string(window_frames) + " frames exceeds the extractor's "
                     "bound of " + std::to_string(FeatureBounds::kMaxAudioFrames) +
                     " per call: use repeated windows";
        }
        return false;
    }
    opened_ = true;
    return true;
}

std::vector<std::string> AlsaCommandAudioSource::capture_argv() const {
    // -t raw with output to the pipe's stdout: there is no file argument anywhere in this command.
    return {cfg_.command,
            "-q",
            "-D",
            cfg_.device,
            "-f",
            "S16_LE",
            "-r",
            std::to_string(cfg_.sample_rate),
            "-c",
            std::to_string(cfg_.channels),
            "-d",
            std::to_string(cfg_.seconds),
            "-t",
            "raw",
            "-"};
}

bool AlsaCommandAudioSource::read_window(std::vector<int16_t>& pcm, size_t* frames, uint32_t* channels,
                                         uint32_t* sample_rate) {
    if (!opened_) return false;

    const CommandResult captured = cfg_.runner(capture_argv(), cfg_.max_bytes);
    last_read_short_ = false;
    if (captured.exit_code != 0) return false;
    if (captured.truncated) {
        // The bound was hit: the device streamed more than one window's worth. Refusing is the honest
        // answer - a truncated window would silently change the measurement's window length.
        return false;
    }
    const size_t bytes = captured.output.size();
    if (bytes % 2 != 0) return false;          // S16_LE cannot have an odd byte count
    if (bytes == 0) return false;

    const size_t expected_bytes =
        static_cast<size_t>(cfg_.seconds) * cfg_.sample_rate * cfg_.channels * 2u;
    last_read_short_ = bytes < expected_bytes;

    pcm.resize(bytes / 2);
    std::memcpy(pcm.data(), captured.output.data(), bytes);
    if (frames != nullptr) *frames = pcm.size() / cfg_.channels;
    if (channels != nullptr) *channels = cfg_.channels;
    if (sample_rate != nullptr) *sample_rate = cfg_.sample_rate;
    return true;
}

}  // namespace homeguardian
