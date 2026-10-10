#include "SensorDiscovery.h"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <array>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

namespace homeguardian {
namespace fs = std::filesystem;

// Run a command and capture its stdout (no shell injection concerns here: the
// arguments are fixed literals). Returns empty string on failure.
static std::string run_command(const std::string& cmd) {
    std::string result;
    std::array<char, 4096> buffer{};
    // Using popen with a fixed command string; cmd is always a constant we
    // control (never derived from untrusted input).
    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) return result;
    while (fgets(buffer.data(), static_cast<int>(buffer.size()), pipe) != nullptr) {
        result += buffer.data();
    }
    pclose(pipe);
    return result;
}

std::vector<SensorDeviceInfo> LinuxSensorDiscovery::list_cameras() {
    std::vector<SensorDeviceInfo> out;
    // Enumerate /dev/video* nodes. We only read the device-node path and its
    // sysfs name; we do NOT open the device (no VIDIOC_QUERYCAP, no capture).
    std::error_code ec;
    for (auto& entry : fs::directory_iterator("/dev", ec)) {
        if (ec) break;
        const std::string name = entry.path().filename().string();
        if (name.rfind("video", 0) != 0) continue;  // must start with "video"
        SensorDeviceInfo info;
        info.kind = SensorKind::camera;
        info.id = entry.path().string();
        info.driver = "v4l2";
        // Best-effort human-readable name from sysfs.
        std::string sysname = "/sys/class/video4linux/" + name + "/name";
        std::ifstream f(sysname);
        if (f.is_open()) {
            std::getline(f, info.name);
        }
        if (info.name.empty()) info.name = name;
        out.push_back(std::move(info));
    }
    return out;
}

// Parse `pactl list short sources` / `sinks` lines. Each non-comment line has
// tab-separated fields; field 1 (index) is the id and field 2 is the name.
static std::vector<SensorDeviceInfo> parse_pactl(const std::string& output, SensorKind kind) {
    std::vector<SensorDeviceInfo> out;
    std::istringstream iss(output);
    std::string line;
    while (std::getline(iss, line)) {
        if (line.empty() || line[0] == '#') continue;
        std::vector<std::string> fields;
        std::string field;
        std::istringstream ls(line);
        while (std::getline(ls, field, '\t')) fields.push_back(field);
        if (fields.size() < 2) continue;
        SensorDeviceInfo info;
        info.kind = kind;
        info.id = fields[1];       // pactl name
        info.name = fields[1];     // pactl has no separate label column here
        info.driver = "pipewire";  // pactl fronts PipeWire/PulseAudio
        out.push_back(std::move(info));
    }
    return out;
}

std::vector<SensorDeviceInfo> LinuxSensorDiscovery::list_microphones() {
    // `pactl list short sources` lists capture sources (inputs). Monitors
    // (".monitor") are loopback capture of sinks, not microphones; exclude them.
    std::string out = run_command("pactl list short sources 2>/dev/null");
    auto devices = parse_pactl(out, SensorKind::microphone);
    std::vector<SensorDeviceInfo> filtered;
    for (auto& d : devices) {
        if (d.id.find(".monitor") == std::string::npos) filtered.push_back(std::move(d));
    }
    return filtered;
}

std::vector<SensorDeviceInfo> LinuxSensorDiscovery::list_speakers() {
    std::string out = run_command("pactl list short sinks 2>/dev/null");
    return parse_pactl(out, SensorKind::speaker);
}

} // namespace homeguardian
