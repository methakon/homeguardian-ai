#pragma once

// Device discovery for the Ubuntu (Linux) sensor backend.
//
// Pure metadata enumeration: it lists available camera (V4L2) and audio
// (ALSA/PipeWire) devices WITHOUT opening or capturing from them. This is the
// non-capture discovery step that lets the operator see what exists before any
// explicit activation. No device file is opened here beyond reading sysfs and
// querying the sound server's device list; no frames or samples are read.

#include <string>
#include <vector>

namespace homeguardian {

enum class SensorKind {
    camera,
    microphone,
    speaker
};

struct SensorDeviceInfo {
    SensorKind kind;
    std::string id;          // stable identifier (e.g. "/dev/video0", alsa name)
    std::string name;        // human-readable label
    std::string driver;      // e.g. "v4l2", "alsa", "pipewire"
};

class ISensorDiscovery {
public:
    virtual ~ISensorDiscovery() = default;
    virtual std::vector<SensorDeviceInfo> list_cameras() = 0;
    virtual std::vector<SensorDeviceInfo> list_microphones() = 0;
    virtual std::vector<SensorDeviceInfo> list_speakers() = 0;
};

// Linux implementation. Enumerates:
//  - cameras via /dev/video* + /sys/class/video4linux/*/name (V4L2 nodes)
//  - audio via the PipeWire/PulseAudio server through `pactl` (metadata only)
// It does NOT open any device for capture.
class LinuxSensorDiscovery : public ISensorDiscovery {
public:
    std::vector<SensorDeviceInfo> list_cameras() override;
    std::vector<SensorDeviceInfo> list_microphones() override;
    std::vector<SensorDeviceInfo> list_speakers() override;
};

} // namespace homeguardian
