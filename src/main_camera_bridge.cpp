// hg_camera_bridge — the REAL physical camera path on Ubuntu, with an operator gate.
//
// WHAT THIS DOES
//   --probe    : enumerate the host's V4L2 camera nodes and report what they can do. This
//                opens nodes to QUERY their capabilities and captures NOTHING.
//   --capture  : capture a bounded number of real frames from a real camera, reduce each
//                frame to a luma plane, derive bounded motion scalars, run the EXISTING
//                activity detector, and print one finding per frame plus the resulting
//                Observation with `provenance: "real"`.
//
// THE GATE
//   `--capture` requires `--confirm`. Without it the program refuses, so no shell history
//   accident, no script and no test can switch a camera on by running this binary.
//
// WHAT IT DOES NOT DO
//   No recording, no file, no stream, no socket, no colour conversion, no codec. A frame
//   exists only long enough to be reduced to scalars. No person, animal, species, face,
//   gesture, fall or emergency claim is produced, and none is possible: motion is motion.
//
// Missing dependency note: unlike the audio path, this needs NO library at all. V4L2 is the
// kernel UAPI already present at /usr/include/linux/videodev2.h.

#include "backend/linux/V4l2CameraDevice.h"
#include "core/CameraActivitySession.h"
#include "core/Observation.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <linux/videodev2.h>

using namespace homeguardian;

namespace {

void print_usage(const char* argv0) {
    std::printf(
        "usage:\n"
        "  %s --probe [--device <node>]                     enumerate camera nodes (captures nothing)\n"
        "  %s --capture --confirm --frames <n> [--device <node>] [--width w] [--height h]\n"
        "                                                   capture n frames and reduce them to scalars\n"
        "\n"
        "Capture REQUIRES --confirm. This program derives bounded scalars only: it writes no\n"
        "frame anywhere, opens no socket, and makes no claim about what is in view.\n",
        argv0, argv0);
}

int xioctl(int fd, unsigned long request, void* arg) {
    int rc = 0;
    do {
        rc = ::ioctl(fd, request, arg);
    } while (rc == -1 && errno == EINTR);
    return rc;
}

std::string fourcc_to_string(uint32_t f) {
    std::string s(4, ' ');
    s[0] = static_cast<char>(f & 0xFF);
    s[1] = static_cast<char>((f >> 8) & 0xFF);
    s[2] = static_cast<char>((f >> 16) & 0xFF);
    s[3] = static_cast<char>((f >> 24) & 0xFF);
    return s;
}

// Metadata only: capability query, no streaming, no buffer, no frame.
int probe_node(const std::string& node) {
    const int fd = ::open(node.c_str(), O_RDWR | O_NONBLOCK);
    if (fd < 0) {
        std::printf("  %-14s cannot open (%s)\n", node.c_str(), std::strerror(errno));
        return 0;
    }
    v4l2_capability cap{};
    if (xioctl(fd, VIDIOC_QUERYCAP, &cap) < 0) {
        std::printf("  %-14s not a V4L2 device\n", node.c_str());
        ::close(fd);
        return 0;
    }
    const uint32_t caps = (cap.capabilities & V4L2_CAP_DEVICE_CAPS) ? cap.device_caps
                                                                    : cap.capabilities;
    const bool capture = (caps & V4L2_CAP_VIDEO_CAPTURE) != 0;
    const bool streaming = (caps & V4L2_CAP_STREAMING) != 0;
    std::printf("  %-14s driver=%s card=%s\n", node.c_str(),
                reinterpret_cast<const char*>(cap.driver),
                reinterpret_cast<const char*>(cap.card));
    std::printf("  %-14s %s%s%s\n", "", capture ? "VIDEO_CAPTURE " : "",
                streaming ? "STREAMING " : "",
                (caps & V4L2_CAP_META_CAPTURE) ? "META_CAPTURE " : "");
    std::printf("  %-14s us usable=%s\n", "",
                (capture && streaming) ? "yes" : "NO (capture requires both flags)");
    if (capture) {
        v4l2_fmtdesc f{};
        f.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        std::printf("  %-14s formats:", "");
        int count = 0;
        for (f.index = 0; xioctl(fd, VIDIOC_ENUM_FMT, &f) == 0 && count < 12; ++f.index, ++count) {
            std::printf(" %s", fourcc_to_string(f.pixelformat).c_str());
        }
        std::printf("%s\n", count == 0 ? " (none advertised)" : "");
    }
    ::close(fd);
    return (capture && streaming) ? 1 : 0;
}

}  // namespace

int main(int argc, char** argv) {
    bool probe = false;
    bool capture = false;
    bool confirmed = false;
    int frames = 30;
    std::string device = "/dev/video0";
    uint32_t width = 640;
    uint32_t height = 480;

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--probe") probe = true;
        else if (a == "--capture") capture = true;
        else if (a == "--confirm") confirmed = true;
        else if (a == "--frames" && i + 1 < argc) frames = std::atoi(argv[++i]);
        else if (a == "--device" && i + 1 < argc) device = argv[++i];
        else if (a == "--width" && i + 1 < argc) width = static_cast<uint32_t>(std::atoi(argv[++i]));
        else if (a == "--height" && i + 1 < argc) height = static_cast<uint32_t>(std::atoi(argv[++i]));
        else if (a == "--help" || a == "-h") { print_usage(argv[0]); return 0; }
        else { std::printf("unknown argument: %s\n", a.c_str()); print_usage(argv[0]); return 2; }
    }

    if (!probe && !capture) { print_usage(argv[0]); return 2; }
    if (frames < 1) frames = 1;
    if (frames > 600) frames = 600;   // bounded: this is a spot check, not a surveillance run

    if (probe) {
        std::printf("=====================================================================\n");
        std::printf(" Host camera inventory (metadata only; nothing is captured)\n");
        std::printf("=====================================================================\n");
        int usable = 0;
        for (int i = 0; i < 8; ++i) {
            const std::string node = "/dev/video" + std::to_string(i);
            if (::access(node.c_str(), F_OK) != 0) continue;
            usable += probe_node(node);
        }
        std::printf("\n  capturable nodes: %d\n", usable);
        std::printf("  (a node with META_CAPTURE only is the UVC metadata channel, not a camera)\n");
        if (!capture) return 0;
    }

    if (!capture) return 0;

    if (!confirmed) {
        std::printf("REFUSED: capture requires --confirm. Nothing was opened.\n");
        return 3;
    }

    std::printf("=====================================================================\n");
    std::printf(" REAL camera capture -> bounded scalars -> one finding per frame\n");
    std::printf(" device=%s  requested=%ux%u  frames=%d\n", device.c_str(), width, height, frames);
    std::printf(" Nothing is written, streamed or transmitted. No image leaves this process.\n");
    std::printf("=====================================================================\n");

    V4l2CameraDevice::Config cfg;
    cfg.device = device;
    cfg.width = width;
    cfg.height = height;
    V4l2CameraDevice cam(cfg);
    CameraActivitySession::Config scfg;
    CameraActivitySession session(scfg);

    try {
        cam.authorize_capture(true);   // the operator's flag, translated into the gate
        cam.initialize();
        std::printf("\nnegotiated: %s\n", cam.negotiated_summary().c_str());
        cam.start();
    } catch (const std::exception& e) {
        std::printf("FAILED to start capture: %s\n", e.what());
        return 4;
    }

    const auto t0 = std::chrono::steady_clock::now();
    size_t captured = 0;
    size_t stalls = 0;
    size_t findings = 0;
    CameraFinding last_valid{};
    std::string last_reason;

    for (int i = 0; i < frames; ++i) {
        LumaFrame frame;
        std::string why;
        if (!cam.next_luma(&frame, &why)) {
            ++stalls;
            const CameraFinding stall = session.note_stalled_window(why);
            std::printf("  [%3d] %-22s %s\n", i, camera_finding_kind_to_string(stall.kind),
                        stall.reason.c_str());
            if (++findings >= 8 && stalls > 0) break;   // do not spin on a dead device
            continue;
        }
        ++captured;
        const CameraFinding f = session.ingest(frame.luma.data(), frame.luma.size(),
                                               Provenance::real);
        if (f.kind != CameraFindingKind::warming_up || findings < 3) {
            std::printf("  [%3d] seq=%-6u motion=%.4f mean_delta=%.4f baseline=%.4f ratio=%.2f  %s\n",
                        i, frame.sequence, f.motion_level, f.mean_delta, f.baseline, f.ratio,
                        camera_finding_kind_to_string(f.kind));
        }
        ++findings;
        if (f.valid) last_valid = f;
    }

    const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now() - t0).count();

    cam.stop();
    cam.close();

    std::printf("\n== RESULTS ==\n");
    std::printf("  frames captured      : %zu of %d requested\n", captured, frames);
    std::printf("  capture stalls       : %zu\n", stalls);
    std::printf("  driver-dropped frames: %llu\n",
                static_cast<unsigned long long>(cam.dropped_frames()));
    std::printf("  elapsed              : %lld ms\n", static_cast<long long>(elapsed_ms));
    std::printf("  pixels per frame     : %ux%u (luma plane reduced in place)\n", width, height);
    std::printf("  final finding        : %s\n", camera_finding_kind_to_string(last_valid.kind));
    std::printf("    sentence           : %s\n", last_valid.sentence.c_str());
    std::printf("    motion_level       : %.4f   mean_delta %.4f   baseline %.4f\n",
                last_valid.motion_level, last_valid.mean_delta, last_valid.baseline);
    std::printf("    provenance         : %s\n",
                last_valid.real_source ? "real (a physical device frame)" : "synthetic");
    std::printf("    NOT claimed        : %s\n", last_valid.uncertainty.c_str());

    if (last_valid.kind != CameraFindingKind::frames_stalled) {
        const std::string obs_id =
            "cam-activity-" + std::to_string(static_cast<long long>(elapsed_ms));
        const Observation obs = session.observation_for(obs_id, device);
        const Event ev = obs.to_event();
        std::printf("\n  observation (through the existing event model):\n%s\n",
                    ev.to_json().dump(2).c_str());
    } else {
        std::printf("\n  no motion observation is built for a stalled device: nothing was "
                    "measured.\n");
    }

    if (captured == 0) {
        std::printf("\nRESULT: NO FRAMES (the device did not deliver)\n");
        return 5;
    }
    std::printf("\nRESULT: %zu real frame(s) reduced to scalars; nothing retained.\n", captured);
    return 0;
}
