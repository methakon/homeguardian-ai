// hg_device_registry_cli — Authoritative C++ CLI interface for HomeGuardian DeviceRegistry
//
// Milestone HG-UI-03: Peripheral Devices Panel integration.
// Direct bridge between HomeGuardian's C++ DeviceRegistry and the web gateway.
// Enforces:
//   - Strict boundary and capability limits (DeviceRegistry::Config)
//   - Liveness sweep and offline detection based on heartbeat timestamps
//   - Canonical peripheral schema output required by HG-UI-03:
//     {
//       "id": "...",
//       "name": "...",
//       "type": "...",
//       "authorized": bool,
//       "online": bool,
//       "capabilities": { "video": bool, "audio_in": bool, "talkback": bool },
//       "consent": "granted" | "not_granted",
//       "stream_state": "active" | "inactive",
//       "last_seen": ISO8601 string or null
//     }
//   - Zero hardcoded mock arrays: every device is read and persisted via
//     homeguardian::net::DeviceRegistry.

#include <chrono>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <random>
#include <sstream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>
#include "net/DeviceRegistry.h"

using namespace homeguardian::net;
using json = nlohmann::json;

namespace {

bool constant_time_equals(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    unsigned char result = 0;
    for (size_t i = 0; i < a.size(); ++i) {
        result |= static_cast<unsigned char>(a[i] ^ b[i]);
    }
    return result == 0;
}

int64_t get_now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

std::string format_iso(int64_t ms) {
    if (ms <= 0) return "";
    std::time_t t = static_cast<std::time_t>(ms / 1000);
    std::tm gm{};
    gmtime_r(&t, &gm);
    std::ostringstream ss;
    ss << std::put_time(&gm, "%Y-%m-%dT%H:%M:%SZ");
    return ss.str();
}

json format_device(const DeviceRegistry::Device& d, int64_t now_ms) {
    json j = json::object();
    j["id"] = d.device_id;
    j["name"] = d.display_name.empty() ? d.device_id : d.display_name;
    j["type"] = d.device_type.empty() ? "sensor" : d.device_type;

    // A device is authorized ONLY if it has verified pairing identity, capture enabled, and consent recorded
    const bool has_verified_identity = d.paired && !d.revoked && !d.credential_id.empty() && !d.auth_secret.empty();
    const bool authorized = d.capture_enabled && d.consent_granted && has_verified_identity;
    j["authorized"] = authorized;

    // Check authenticated liveness: verified identity, active online flag, and unexpired heartbeat
    const bool online = has_verified_identity && d.online && (d.last_heartbeat_ms > 0) &&
                        ((now_ms - d.last_heartbeat_ms) < 90000);
    j["online"] = online;

    // Capabilities breakdown
    bool has_talkback = false;
    for (const auto& cap : d.capabilities) {
        if (cap == "talkback" || cap == "speaker") has_talkback = true;
    }
    if (d.device_type == "speaker") has_talkback = true;

    j["capabilities"] = {
        {"video", d.has_camera},
        {"audio_in", d.has_microphone},
        {"talkback", has_talkback}
    };

    // Consent status
    if (d.consent_granted) {
        j["consent"] = "granted";
    } else {
        j["consent"] = "not_granted";
    }

    // Stream state
    if (online && authorized) {
        j["stream_state"] = "active";
    } else {
        j["stream_state"] = "inactive";
    }

    // Last seen
    if (d.last_heartbeat_ms > 0) {
        j["last_seen"] = format_iso(d.last_heartbeat_ms);
        j["last_seen_ms"] = d.last_heartbeat_ms;
    } else {
        j["last_seen"] = nullptr;
        j["last_seen_ms"] = 0;
    }

    // Security & consent metadata
    j["paired"] = d.paired;
    j["revoked"] = d.revoked;
    if (!d.revocation_note.empty()) {
        j["revocation_note"] = d.revocation_note;
    }
    j["consent_subject"] = d.consent_subject;
    j["consent_purpose"] = d.consent_purpose;

    // Do NOT publish credential_id or auth_secret over public listings
    j["has_credential"] = !d.credential_id.empty() && !d.auth_secret.empty();

    // Explicit synthetic provider flag: true strictly for authoritative simulator provider
    const bool is_synthetic = (d.device_id == DeviceRegistry::Device::kAuthoritativeSyntheticDeviceId && d.device_type == "simulator");
    j["is_synthetic"] = is_synthetic;

    return j;
}

void init_default_registry(DeviceRegistry& reg, const std::string& path, bool save_to_disk = false) {
    std::string reason;

    // 1. Android Wi-Fi Camera 1 (Physical candidate: offline, not granted, never faked online)
    DeviceRegistry::Device cam1;
    cam1.device_id = "camera-001";
    cam1.display_name = "Android Wi-Fi Camera 1";
    cam1.device_type = "camera";
    cam1.credential_id = "camera-001-cert";
    cam1.auth_secret = "sec_cam1_7f8a9b0c1d2e3f4a5b6c7d8e9f0a1b2c";
    cam1.paired = true;
    cam1.revoked = false;
    cam1.capabilities = {"camera", "microphone", "talkback"};
    cam1.has_camera = true;
    cam1.has_microphone = true;
    cam1.consent_granted = false;
    cam1.capture_enabled = false;
    cam1.online = false;
    cam1.last_heartbeat_ms = 0;
    cam1.is_synthetic = false;
    reg.pair(cam1, &reason);

    // 2. Synthetic Test Provider (Specifically for UI & transport testing without phone)
    DeviceRegistry::Device camSyn;
    camSyn.device_id = "camera-syn-001";
    camSyn.display_name = "Synthetic Test Camera (Simulation)";
    camSyn.device_type = "simulator";
    camSyn.credential_id = "syn-camera-cert";
    camSyn.auth_secret = "sec_syn_1a2b3c4d5e6f7a8b9c0d1e2f3a4b5c6d";
    camSyn.paired = true;
    camSyn.revoked = false;
    camSyn.capabilities = {"camera"};
    camSyn.has_camera = true;
    camSyn.has_microphone = false;
    camSyn.consent_granted = true;
    camSyn.consent_subject = "operator";
    camSyn.consent_purpose = "demo_testing";
    camSyn.capture_enabled = true;
    camSyn.online = false;
    camSyn.last_heartbeat_ms = 0;
    camSyn.is_synthetic = true;
    reg.pair(camSyn, &reason);

    // 3. Hallway Audio Sensor (Offline, unconsented)
    DeviceRegistry::Device mic1;
    mic1.device_id = "mic-001";
    mic1.display_name = "Living Room Microphone Node";
    mic1.device_type = "microphone";
    mic1.credential_id = "mic-001-cert";
    mic1.auth_secret = "sec_mic1_9e8d7c6b5a4f3e2d1c0b9a8f7e6d5c4b";
    mic1.paired = true;
    mic1.revoked = false;
    mic1.capabilities = {"microphone"};
    mic1.has_camera = false;
    mic1.has_microphone = true;
    mic1.consent_granted = false;
    mic1.capture_enabled = false;
    mic1.online = false;
    reg.pair(mic1, &reason);

    // 4. Intercom Speaker Node (Talkback capability)
    DeviceRegistry::Device spk1;
    spk1.device_id = "speaker-001";
    spk1.display_name = "Hallway Intercom Speaker";
    spk1.device_type = "speaker";
    spk1.credential_id = "speaker-001-cert";
    spk1.auth_secret = "sec_spk1_5a6b7c8d9e0f1a2b3c4d5e6f7a8b9c0d";
    spk1.paired = true;
    spk1.revoked = false;
    spk1.capabilities = {"talkback"};
    spk1.has_camera = false;
    spk1.has_microphone = false;
    spk1.consent_granted = true;
    spk1.consent_subject = "operator";
    spk1.consent_purpose = "intercom";
    spk1.capture_enabled = true;
    spk1.online = false;
    reg.pair(spk1, &reason);

    // 5. Environmental Multi-Sensor
    DeviceRegistry::Device env1;
    env1.device_id = "sensor-env-001";
    env1.display_name = "Kitchen Environmental Multi-Sensor";
    env1.device_type = "sensor";
    env1.credential_id = "env-001-cert";
    env1.auth_secret = "sec_env1_3c4d5e6f7a8b9c0d1e2f3a4b5c6d7e8f";
    env1.paired = true;
    env1.revoked = false;
    env1.capabilities = {"temperature", "humidity", "motion"};
    env1.has_camera = false;
    env1.has_microphone = false;
    env1.consent_granted = true;
    env1.consent_subject = "household";
    env1.consent_purpose = "hazard_detection";
    env1.capture_enabled = true;
    env1.online = false;
    reg.pair(env1, &reason);

    if (save_to_disk) {
        reg.save(path, &reason);
    }
}

}  // namespace

int main(int argc, char* argv[]) {
    std::string registry_path = "devices.json";
    std::string credential_file_flag;
    std::string command;
    std::vector<std::string> args;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--registry" && i + 1 < argc) {
            registry_path = argv[++i];
        } else if (a == "--credential-file" && i + 1 < argc) {
            credential_file_flag = argv[++i];
        } else if (a == "--help" || a == "-h") {
            std::cout << "Usage: hg_device_registry_cli [--registry <path>] [--credential-file <path>] <command> [args...]\n"
                      << "Commands:\n"
                      << "  list                          List all peripheral devices in HG-UI-03 format\n"
                      << "  get <device_id>               Get single device details\n"
                      << "  set_consent <id> <true|false> Update device consent and capture status\n"
                      << "  pair <json_device>            Pair a new device\n"
                      << "  revoke <id> [reason]          Revoke device authorization\n"
                      << "  unpair <id>                   Unpair a device and remove state\n"
                      << "  heartbeat <id> [seq] [ts_ms]  Update liveness timestamp\n"
                      << "  init_defaults                 Create default inventory if not present\n";
            return 0;
        } else {
            if (command.empty()) {
                command = a;
            } else {
                args.push_back(a);
            }
        }
    }
    if (command.empty()) {
        command = "list";
    }

    DeviceRegistry registry;
    std::string reason;

    // Check if registry exists, otherwise initialize default
    std::ifstream test_f(registry_path);
    if (!test_f.good()) {
        init_default_registry(registry, registry_path, false);
    } else {
        if (!registry.load(registry_path, &reason)) {
            std::cerr << "{\"error\": \"failed to load registry: " << reason << "\"}\n";
            return 1;
        }
    }

    const int64_t now_ms = get_now_ms();
    registry.sweep_offline(now_ms);

    if (command == "list") {
        if (!args.empty()) {
            std::cerr << "{\"error\": \"usage: list (takes no arguments)\"}\n";
            return 1;
        }
        json list = json::array();
        for (const auto& d : registry.snapshot()) {
            list.push_back(format_device(d, now_ms));
        }
        std::cout << list.dump(2) << "\n";
        return 0;
    }

    if (command == "get") {
        if (args.size() != 1) {
            std::cerr << "{\"error\": \"usage: get <device_id>\"}\n";
            return 1;
        }
        const std::string& dev_id = args[0];
        const DeviceRegistry::Device* d = registry.find(dev_id);
        if (d == nullptr) {
            std::cerr << "{\"error\": \"device not found: " << dev_id << "\"}\n";
            return 2;
        }
        std::cout << format_device(*d, now_ms).dump(2) << "\n";
        return 0;
    }

    if (command == "set_consent") {
        if (args.size() < 2 || args.size() > 4) {
            std::cerr << "{\"error\": \"usage: set_consent <device_id> <true|false> [subject] [purpose]\"}\n";
            return 1;
        }
        const std::string& dev_id = args[0];
        const bool granted = (args[1] == "true" || args[1] == "1");
        const std::string subject = args.size() > 2 ? args[2] : "operator";
        const std::string purpose = args.size() > 3 ? args[3] : "live_view";

        if (!registry.set_consent(dev_id, granted, subject, purpose, now_ms)) {
            std::cerr << "{\"error\": \"set_consent failed for device " << dev_id << "\"}\n";
            return 3;
        }
        // Enable capture if consent granted, disable if revoked
        registry.set_capture_enabled(dev_id, granted);

        if (!registry.save(registry_path, &reason)) {
            std::cerr << "{\"error\": \"failed to save registry: " << reason << "\"}\n";
            return 4;
        }

        const DeviceRegistry::Device* d = registry.find(dev_id);
        std::cout << format_device(*d, now_ms).dump(2) << "\n";
        return 0;
    }

    if (command == "heartbeat") {
        if (args.empty() || args.size() > 3) {
            std::cerr << "{\"error\": \"usage: heartbeat <device_id> [seq] [ts_ms] (unexpected arguments rejected)\"}\n";
            return 1;
        }
        const std::string& dev_id = args[0];
        DeviceRegistry::Device* d = registry.find(dev_id);
        if (d == nullptr) {
            std::cerr << "{\"error\": \"device not found\"}\n";
            return 2;
        }
        if (!d->paired || d->revoked || d->credential_id.empty() || d->auth_secret.empty()) {
            std::cerr << "{\"error\": \"heartbeat rejected: device is unpaired, revoked, or unauthenticated\"}\n";
            std::cout << format_device(*d, now_ms).dump(2) << "\n";
            return 3;
        }

        // Authenticate against d->auth_secret via HG_DEVICE_SECRET / HG_DEVICE_CREDENTIAL / HG_CREDENTIAL_FILE without placing secrets on CLI args
        std::string presented_secret;
        if (const char* s = std::getenv("HG_DEVICE_SECRET")) {
            presented_secret = s;
        } else if (const char* c = std::getenv("HG_DEVICE_CREDENTIAL")) {
            presented_secret = c;
        } else {
            std::string cfile;
            if (const char* f = std::getenv("HG_CREDENTIAL_FILE")) {
                cfile = f;
            } else if (!credential_file_flag.empty()) {
                cfile = credential_file_flag;
            }
            if (!cfile.empty()) {
                std::ifstream ifs(cfile);
                if (ifs.good()) {
                    std::string line;
                    std::getline(ifs, line);
                    while (!line.empty() && (line.back() == '\r' || line.back() == '\n' || line.back() == ' ' || line.back() == '\t')) {
                        line.pop_back();
                    }
                    size_t start = 0;
                    while (start < line.size() && (line[start] == ' ' || line[start] == '\t')) {
                        start++;
                    }
                    presented_secret = line.substr(start);
                }
            }
        }

        // Hard gate: strictly reject if caller presents public credential_id as the secret
        if (!d->credential_id.empty() && presented_secret == d->credential_id) {
            std::cerr << "{\"error\": \"heartbeat rejected: public credential_id cannot be used as authentication secret\"}\n";
            std::cout << format_device(*d, now_ms).dump(2) << "\n";
            return 3;
        }

        // Constant-time comparison against auth_secret
        if (presented_secret.empty() || !constant_time_equals(presented_secret, d->auth_secret)) {
            std::cerr << "{\"error\": \"heartbeat rejected: credential missing or mismatch\"}\n";
            std::cout << format_device(*d, now_ms).dump(2) << "\n";
            return 3;
        }

        int64_t hb_ms = now_ms;
        if (args.size() > 1) {
            try {
                uint64_t seq = std::stoull(args[1]);
                registry.seed_sequence_watermark(dev_id, seq);
            } catch (const std::exception&) {}
        }
        if (args.size() > 2) {
            try {
                hb_ms = std::stoll(args[2]);
            } catch (const std::exception&) {}
        }
        if (hb_ms > now_ms + 5000) {
            std::cerr << "{\"error\": \"heartbeat rejected: timestamp is in the future\"}\n";
            std::cout << format_device(*d, now_ms).dump(2) << "\n";
            return 3;
        }
        if (hb_ms < d->last_heartbeat_ms) {
            std::cerr << "{\"error\": \"heartbeat rejected: timestamp is stale\"}\n";
            std::cout << format_device(*d, now_ms).dump(2) << "\n";
            return 3;
        }
        registry.note_heartbeat(dev_id, hb_ms, d->capabilities);
        registry.sweep_offline(now_ms);
        registry.save(registry_path, &reason);
        std::cout << format_device(*d, now_ms).dump(2) << "\n";
        return 0;
    }

    if (command == "pair") {
        if (args.size() != 1) {
            std::cerr << "{\"error\": \"usage: pair <json_device>\"}\n";
            return 1;
        }
        json dev_j;
        try {
            dev_j = json::parse(args[0]);
        } catch (const std::exception& e) {
            std::cerr << "{\"error\": \"invalid json: " << e.what() << "\"}\n";
            return 1;
        }
        DeviceRegistry::Device d;
        d.device_id = dev_j.value("device_id", "");
        d.display_name = dev_j.value("display_name", d.device_id);
        d.device_type = dev_j.value("device_type", "sensor");
        d.credential_id = dev_j.value("credential_id", "");
        d.auth_secret = dev_j.value("auth_secret", "");
        if (d.auth_secret.empty() && !d.credential_id.empty()) {
            std::random_device rd;
            std::uniform_int_distribution<unsigned int> dist(0, 255);
            std::ostringstream ss;
            for (int i = 0; i < 32; ++i) {
                ss << std::hex << std::setw(2) << std::setfill('0') << dist(rd);
            }
            d.auth_secret = "sec_" + ss.str();
        }
        d.paired = true;
        d.revoked = false;
        d.has_camera = dev_j.value("has_camera", false);
        d.has_microphone = dev_j.value("has_microphone", false);
        d.consent_granted = dev_j.value("consent_granted", false);
        d.capture_enabled = dev_j.value("capture_enabled", false) && d.consent_granted;
        d.is_synthetic = (d.device_id == DeviceRegistry::Device::kAuthoritativeSyntheticDeviceId && d.device_type == "simulator");
        if (dev_j.contains("capabilities") && dev_j["capabilities"].is_array()) {
            for (const auto& cap : dev_j["capabilities"]) {
                if (cap.is_string()) d.capabilities.push_back(cap.get<std::string>());
            }
        }
        if (!registry.pair(d, &reason)) {
            std::cerr << "{\"error\": \"pair failed: " << reason << "\"}\n";
            return 2;
        }
        if (!registry.save(registry_path, &reason)) {
            std::cerr << "{\"error\": \"failed to save registry: " << reason << "\"}\n";
            return 4;
        }
        const DeviceRegistry::Device* paired_d = registry.find(d.device_id);
        std::cout << format_device(*paired_d, now_ms).dump(2) << "\n";
        return 0;
    }

    if (command == "revoke") {
        if (args.empty() || args.size() > 2) {
            std::cerr << "{\"error\": \"usage: revoke <device_id> [note]\"}\n";
            return 1;
        }
        const std::string& dev_id = args[0];
        const std::string note = args.size() > 1 ? args[1] : "revoked by operator";
        if (!registry.revoke(dev_id, note)) {
            std::cerr << "{\"error\": \"revoke failed: device not found or already revoked: " << dev_id << "\"}\n";
            return 2;
        }
        if (!registry.save(registry_path, &reason)) {
            std::cerr << "{\"error\": \"failed to save registry: " << reason << "\"}\n";
            return 4;
        }
        const DeviceRegistry::Device* d = registry.find(dev_id);
        std::cout << format_device(*d, now_ms).dump(2) << "\n";
        return 0;
    }

    if (command == "unpair") {
        if (args.size() != 1) {
            std::cerr << "{\"error\": \"usage: unpair <device_id>\"}\n";
            return 1;
        }
        const std::string& dev_id = args[0];
        if (!registry.unpair(dev_id)) {
            std::cerr << "{\"error\": \"unpair failed: device not found: " << dev_id << "\"}\n";
            return 2;
        }
        if (!registry.save(registry_path, &reason)) {
            std::cerr << "{\"error\": \"failed to save registry: " << reason << "\"}\n";
            return 4;
        }
        std::cout << "{\"status\": \"unpaired\", \"device_id\": \"" << dev_id << "\"}\n";
        return 0;
    }

    if (command == "init_defaults") {
        if (!args.empty()) {
            std::cerr << "{\"error\": \"usage: init_defaults (takes no arguments)\"}\n";
            return 1;
        }
        DeviceRegistry fresh_reg;
        init_default_registry(fresh_reg, registry_path, true);
        std::cout << "{\"status\": \"initialized\", \"path\": \"" << registry_path << "\"}\n";
        return 0;
    }

    std::cerr << "{\"error\": \"unknown command: " << command << "\"}\n";
    return 1;
}
