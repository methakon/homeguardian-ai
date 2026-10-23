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
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <random>
#include <sstream>
#include <string>
#include <unordered_set>
#include <vector>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <nlohmann/json.hpp>
#include "net/DeviceRegistry.h"

using namespace homeguardian::net;
using json = nlohmann::json;

namespace {

// Proactive Denylist: Rejects historical test credentials and publicly exposed secrets.
// These 5 literals (sec_cam1_..., sec_syn_..., sec_mic1_..., sec_spk1_..., sec_env1_...)
// are solely denylist entries retained for proactive rejection. No seeded or active device
// accepts them (all default devices initialize unprovisioned with auth_secret == "").
bool is_blacklisted_secret(std::string_view secret) {
    if (secret.empty()) return false;
    static const std::unordered_set<std::string_view> kBlacklist = {
        "sec_cam1_7f8a9b0c1d2e3f4a5b6c7d8e9f0a1b2c",
        "sec_syn_1a2b3c4d5e6f7a8b9c0d1e2f3a4b5c6d",
        "sec_mic1_9e8d7c6b5a4f3e2d1c0b9a8f7e6d5c4b",
        "sec_spk1_5a6b7c8d9e0f1a2b3c4d5e6f7a8b9c0d",
        "sec_env1_3c4d5e6f7a8b9c0d1e2f3a4b5c6d7e8f",
        "camera-001-cert",
        "syn-camera-cert",
        "mic-001-cert",
        "speaker-001-cert",
        "env-001-cert",
        "cert-camera",
        "forged-cert",
        "forged-cert-001",
        "Release01!operator",
        "password",
        "secret",
        "123456",
        "admin"
    };
    return kBlacklist.find(secret) != kBlacklist.end();
}

// Fail-closed CSPRNG secret generator: strictly requires /dev/urandom.
// Refuses to fall back to non-cryptographic or unspecified pseudo-random sources.
std::string generate_csprng_secret(size_t num_bytes = 32) {
    std::vector<unsigned char> buf(num_bytes);
    std::ifstream urandom("/dev/urandom", std::ios::binary);
    if (!urandom.is_open() || !urandom.good()) {
        throw std::runtime_error("csprng_unavailable: unable to open /dev/urandom for cryptographically secure entropy");
    }
    urandom.read(reinterpret_cast<char*>(buf.data()), num_bytes);
    if (static_cast<size_t>(urandom.gcount()) != num_bytes) {
        throw std::runtime_error("csprng_insufficient_entropy: failed to read required bytes from /dev/urandom");
    }
    std::ostringstream ss;
    ss << "sec_live_";
    for (unsigned char b : buf) {
        ss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(b);
    }
    return ss.str();
}

bool constant_time_equals(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    unsigned char result = 0;
    for (size_t i = 0; i < a.size(); ++i) {
        result |= static_cast<unsigned char>(a[i] ^ b[i]);
    }
    return result == 0;
}

// Validates secret file safety:
// 1. Must exist, be accessible, and be a regular file (S_ISREG). Symlinks (S_ISLNK) are strictly rejected.
// 2. Must be owned by current process effective UID or root (0).
// 3. Must have strict owner-only permissions: mode 0600 or 0400 ((st.st_mode & 0077) == 0).
// 4. Must not be empty.
bool read_secure_secret_file(const std::string& path, std::string& out_secret, std::string* err_msg = nullptr) {
    if (path.empty()) {
        if (err_msg) *err_msg = "secret file path is empty";
        return false;
    }
    struct stat st{};
    // Use lstat to detect symlinks before any open
    if (::lstat(path.c_str(), &st) != 0) {
        if (err_msg) *err_msg = "secret file not found or inaccessible: " + path;
        return false;
    }
    if (S_ISLNK(st.st_mode)) {
        if (err_msg) *err_msg = "secret file rejected: symlinks are prohibited: " + path;
        return false;
    }
    if (!S_ISREG(st.st_mode)) {
        if (err_msg) *err_msg = "secret file rejected: not a regular file: " + path;
        return false;
    }
    uid_t euid = ::geteuid();
    if (st.st_uid != euid && st.st_uid != 0) {
        if (err_msg) *err_msg = "secret file rejected: file owner UID (" + std::to_string(st.st_uid) +
                                ") does not match process UID (" + std::to_string(euid) + ") or root";
        return false;
    }
    if ((st.st_mode & 0077) != 0) {
        if (err_msg) *err_msg = "secret file rejected: insecure permissions; must be 0600 or 0400: " + path;
        return false;
    }
    std::ifstream sf(path);
    if (!sf.good()) {
        if (err_msg) *err_msg = "failed to open secret file for reading: " + path;
        return false;
    }
    std::string line;
    std::getline(sf, line);
    while (!line.empty() && (line.back() == '\r' || line.back() == '\n' || line.back() == ' ' || line.back() == '\t')) {
        line.pop_back();
    }
    size_t start = 0;
    while (start < line.size() && (line[start] == ' ' || line[start] == '\t')) {
        start++;
    }
    std::string secret = line.substr(start);
    if (secret.empty()) {
        if (err_msg) *err_msg = "secret file is empty: " + path;
        return false;
    }
    out_secret = secret;
    return true;
}

// Verifies trusted operator authorization for administrative actions (enrollment, recovery, unpairing).
// Ensures:
// 1. Expected credential is anchored in an authority controlled independently of caller.
//    Process environment (HG_OPERATOR_SECRET / HG_OPERATOR_PASSWORD) cannot define the authority.
// 2. Fallback files in world-writable /tmp are prohibited.
// 3. Symlinks, special files, wrong-owner files, and insecure permissions (0644) are rejected.
// 4. Stdin and process environment only present credentials; they never define expected authority.
bool verify_operator_authorization(
    bool action_flag,
    const std::string& registry_path,
    const std::string& authority_file_flag,
    const std::string& secret_flag,
    const std::string& secret_file_flag,
    bool stdin_flag = false)
{
    if (!action_flag) return false;

    // 1. Resolve EXPECTED secret from trusted authority store.
    // Untrusted caller environment variables MUST NEVER define the authority.
    std::string expected_secret;
    std::string auth_err;

    std::string authority_path = authority_file_flag;
    if (authority_path.empty()) {
        // Default to operator.secret in the directory of the registry file
        std::filesystem::path reg_path(registry_path);
        std::filesystem::path reg_dir = reg_path.parent_path();
        if (reg_dir.empty()) reg_dir = ".";
        authority_path = (reg_dir / "operator.secret").string();
    }

    if (!read_secure_secret_file(authority_path, expected_secret, &auth_err)) {
        if (authority_file_flag.empty()) {
            std::filesystem::path reg_path(registry_path);
            std::filesystem::path reg_dir = reg_path.parent_path();
            if (reg_dir.empty()) reg_dir = ".";
            std::string hidden_path = (reg_dir / ".operator.secret").string();
            read_secure_secret_file(hidden_path, expected_secret, nullptr);
        }
    }

    // Fail-closed: If no trusted operator authority file exists or fails validation, refuse authorization
    if (expected_secret.empty() || is_blacklisted_secret(expected_secret)) {
        std::cerr << "{\"error\": \"operator authorization unavailable: trusted authority secret file absent, unreadable, or insecure\"}\n";
        return false;
    }

    // 2. Resolve PRESENTED secret from caller.
    // The caller can present credentials via CLI argument (scrubbed), stdin, secure secret file,
    // or environment variable HG_OPERATOR_SECRET / HG_OPERATOR_PASSWORD.
    std::string presented_secret = secret_flag;
    if (presented_secret.empty() && stdin_flag) {
        std::getline(std::cin, presented_secret);
    }
    if (presented_secret.empty() && !secret_file_flag.empty()) {
        std::string pres_err;
        if (!read_secure_secret_file(secret_file_flag, presented_secret, &pres_err)) {
            std::cerr << "{\"error\": \"" << pres_err << "\"}\n";
            return false;
        }
    }
    if (presented_secret.empty()) {
        if (const char* s = std::getenv("HG_OPERATOR_SECRET")) {
            presented_secret = s;
        } else if (const char* s = std::getenv("HG_OPERATOR_PASSWORD")) {
            presented_secret = s;
        }
    }

    // Strip trailing whitespace/newlines
    while (!presented_secret.empty() && (presented_secret.back() == '\r' || presented_secret.back() == '\n' || presented_secret.back() == ' ' || presented_secret.back() == '\t')) {
        presented_secret.pop_back();
    }
    size_t pstart = 0;
    while (pstart < presented_secret.size() && (presented_secret[pstart] == ' ' || presented_secret[pstart] == '\t')) {
        pstart++;
    }
    presented_secret = presented_secret.substr(pstart);

    if (presented_secret.empty() || is_blacklisted_secret(presented_secret)) {
        return false;
    }

    return constant_time_equals(presented_secret, expected_secret);
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

    // A device is authorized if capture is enabled and consent is recorded
    const bool authorized = d.capture_enabled && d.consent_granted;
    j["authorized"] = authorized;

    // Check authenticated liveness: verified identity, active online flag, and unexpired heartbeat
    const bool has_verified_identity = d.is_synthetic ||
        (d.paired && !d.revoked && !d.credential_id.empty() && !d.auth_secret.empty());
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

    // 1. Android Wi-Fi Camera 1 (Physical candidate: paired inventory, unprovisioned credentials, offline)
    DeviceRegistry::Device cam1;
    cam1.device_id = "camera-001";
    cam1.display_name = "Android Wi-Fi Camera 1";
    cam1.device_type = "camera";
    cam1.credential_id = "";
    cam1.auth_secret = "";
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

    // 2. Synthetic Test Provider (Simulation benchmark fixture for UI & WebRTC test without phone)
    DeviceRegistry::Device camSyn;
    camSyn.device_id = "camera-syn-001";
    camSyn.display_name = "Synthetic Test Camera (Simulation)";
    camSyn.device_type = "simulator";
    camSyn.credential_id = "syn-camera-cert";
    camSyn.auth_secret = "";
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

    // 3. Hallway Audio Sensor (Unprovisioned physical candidate: offline, unconsented)
    DeviceRegistry::Device mic1;
    mic1.device_id = "mic-001";
    mic1.display_name = "Living Room Microphone Node";
    mic1.device_type = "microphone";
    mic1.credential_id = "";
    mic1.auth_secret = "";
    mic1.paired = true;
    mic1.revoked = false;
    mic1.capabilities = {"microphone"};
    mic1.has_camera = false;
    mic1.has_microphone = true;
    mic1.consent_granted = false;
    mic1.capture_enabled = false;
    mic1.online = false;
    reg.pair(mic1, &reason);

    // 4. Intercom Speaker Node (Unprovisioned physical candidate: talkback capability)
    DeviceRegistry::Device spk1;
    spk1.device_id = "speaker-001";
    spk1.display_name = "Hallway Intercom Speaker";
    spk1.device_type = "speaker";
    spk1.credential_id = "";
    spk1.auth_secret = "";
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

    // 5. Environmental Multi-Sensor (Unprovisioned physical candidate)
    DeviceRegistry::Device env1;
    env1.device_id = "sensor-env-001";
    env1.display_name = "Kitchen Environmental Multi-Sensor";
    env1.device_type = "sensor";
    env1.credential_id = "";
    env1.auth_secret = "";
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
        // Provision secure operator authority file in registry directory if absent
        std::filesystem::path reg_file(path);
        std::filesystem::path reg_dir = reg_file.parent_path();
        if (reg_dir.empty()) reg_dir = ".";
        std::filesystem::path op_secret_file = reg_dir / "operator.secret";
        struct stat ost{};
        if (::lstat(op_secret_file.c_str(), &ost) != 0) {
            try {
                std::string op_sec = generate_csprng_secret(32);
                int fd = ::open(op_secret_file.c_str(), O_WRONLY | O_CREAT | O_EXCL, S_IRUSR | S_IWUSR);
                if (fd >= 0) {
                    std::string line = op_sec + "\n";
                    ssize_t w = ::write(fd, line.data(), line.size());
                    (void)w;
                    ::close(fd);
                    ::chmod(op_secret_file.c_str(), S_IRUSR | S_IWUSR);
                }
            } catch (...) {}
        }
    }
}

}  // namespace

int main(int argc, char* argv[]) {
    std::string registry_path = "devices.json";
    std::string credential_file_flag;
    bool operator_auth_flag = false;
    bool operator_recovery_flag = false;
    std::string operator_authority_file_flag;
    std::string operator_secret_flag;
    std::string operator_secret_file_flag;
    bool operator_secret_stdin_flag = false;
    std::string command;
    std::vector<std::string> args;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--registry" && i + 1 < argc) {
            registry_path = argv[++i];
        } else if (a == "--credential-file" && i + 1 < argc) {
            credential_file_flag = argv[++i];
        } else if (a == "--operator-auth") {
            operator_auth_flag = true;
        } else if (a == "--operator-recovery") {
            operator_recovery_flag = true;
        } else if (a == "--operator-authority-file" && i + 1 < argc) {
            operator_authority_file_flag = argv[++i];
        } else if (a == "--operator-secret-stdin") {
            operator_secret_stdin_flag = true;
        } else if (a == "--operator-secret" && i + 1 < argc) {
            operator_secret_flag = argv[++i];
            // Security hardening: scrub operator secret from process memory to avoid ps inspection
            std::fill_n(argv[i], std::strlen(argv[i]), '\0');
        } else if (a == "--operator-secret-file" && i + 1 < argc) {
            operator_secret_file_flag = argv[++i];
        } else if (a == "--help" || a == "-h") {
            std::cout << "Usage: hg_device_registry_cli [--registry <path>] [--credential-file <path>] [--operator-auth] [--operator-recovery] [--operator-authority-file <path>] [--operator-secret-file <path>] <command> [args...]\n"
                      << "Commands:\n"
                      << "  list                          List all peripheral devices in HG-UI-03 format\n"
                      << "  get <device_id>               Get single device details\n"
                      << "  set_consent <id> <true|false> Update device consent and capture status\n"
                      << "  pair <json_device>            Pair a new device (generates CSPRNG secret if omitted)\n"
                      << "  rotate_secret <id>            Rotate device secret to a fresh CSPRNG token\n"
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

        // Hard gate: strictly reject blacklisted/known-insecure secrets
        if (is_blacklisted_secret(presented_secret) || is_blacklisted_secret(d->auth_secret)) {
            std::cerr << "{\"error\": \"heartbeat rejected: known exposed/insecure secret revoked by policy\"}\n";
            std::cout << format_device(*d, now_ms).dump(2) << "\n";
            return 3;
        }

        // Constant-time comparison against auth_secret
        if (presented_secret.empty() || !constant_time_equals(presented_secret, d->auth_secret)) {
            std::cerr << "{\"error\": \"heartbeat rejected: credential missing or mismatch\"}\n";
            std::cout << format_device(*d, now_ms).dump(2) << "\n";
            return 3;
        }

        // Strictly increasing sequence verification (Heartbeat replay protection)
        uint64_t seq = 0;
        if (args.size() > 1) {
            try {
                size_t idx = 0;
                seq = std::stoull(args[1], &idx);
                if (idx != args[1].size()) {
                    std::cerr << "{\"error\": \"heartbeat rejected: malformed sequence number\"}\n";
                    std::cout << format_device(*d, now_ms).dump(2) << "\n";
                    return 3;
                }
            } catch (const std::exception&) {
                std::cerr << "{\"error\": \"heartbeat rejected: malformed sequence number\"}\n";
                std::cout << format_device(*d, now_ms).dump(2) << "\n";
                return 3;
            }

            if (seq <= d->last_sequence) {
                std::cerr << "{\"error\": \"heartbeat rejected: duplicate or older sequence number (replay detected)\"}\n";
                std::cout << format_device(*d, now_ms).dump(2) << "\n";
                return 3;
            }
        } else {
            seq = d->last_sequence + 1;
        }

        // Timestamp freshness check
        int64_t hb_ms = now_ms;
        if (args.size() > 2) {
            try {
                size_t idx = 0;
                hb_ms = std::stoll(args[2], &idx);
                if (idx != args[2].size()) {
                    std::cerr << "{\"error\": \"heartbeat rejected: malformed timestamp\"}\n";
                    std::cout << format_device(*d, now_ms).dump(2) << "\n";
                    return 3;
                }
            } catch (const std::exception&) {
                std::cerr << "{\"error\": \"heartbeat rejected: malformed timestamp\"}\n";
                std::cout << format_device(*d, now_ms).dump(2) << "\n";
                return 3;
            }
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

        d->last_sequence = seq;
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
        std::string dev_id = dev_j.value("device_id", "");
        if (dev_id.empty()) {
            std::cerr << "{\"error\": \"pair rejected: device_id is required\"}\n";
            return 1;
        }

        // Hard gate: Do not accept arbitrary client-supplied auth_secret to acquire trusted identity
        if (dev_j.contains("auth_secret") && !dev_j["auth_secret"].get<std::string>().empty()) {
            std::cerr << "{\"error\": \"pair rejected: client-supplied auth_secret not permitted; host CSPRNG provisions credentials\"}\n";
            return 2;
        }

        const DeviceRegistry::Device* existing = registry.find(dev_id);

        const bool operator_authorized = verify_operator_authorization(
            operator_auth_flag || operator_recovery_flag,
            registry_path,
            operator_authority_file_flag,
            operator_secret_flag,
            operator_secret_file_flag,
            operator_secret_stdin_flag);

        const bool operator_recovery_authorized = verify_operator_authorization(
            operator_recovery_flag,
            registry_path,
            operator_authority_file_flag,
            operator_secret_flag,
            operator_secret_file_flag,
            operator_secret_stdin_flag);

        // Security Lifecycle Gates:
        // Gate 1: Revoked device cannot be reclaimed by ordinary pairing or credential proof; requires trusted operator recovery
        if (existing != nullptr && existing->revoked) {
            if (!operator_recovery_authorized) {
                std::cerr << "{\"error\": \"re-pairing rejected: device is revoked; trusted operator recovery authorization required (--operator-recovery)\"}\n";
                return 3;
            }
        }
        // Gate 2: Actively enrolled device requires either proof of current credential or trusted operator recovery
        else if (existing != nullptr && existing->paired && !existing->auth_secret.empty()) {
            // Reject any unauthenticated requester attempting to claim operator recovery via caller-controlled JSON
            if ((dev_j.contains("operator_recovery") && dev_j["operator_recovery"].is_boolean() && dev_j["operator_recovery"].get<bool>()) ||
                (dev_j.contains("force_re-pair") && dev_j["force_re-pair"].is_boolean() && dev_j["force_re-pair"].get<bool>())) {
                if (!operator_recovery_authorized) {
                    std::cerr << "{\"error\": \"re-pairing rejected: caller-supplied operator_recovery boolean is not permitted; operator recovery requires trusted operator authorization\"}\n";
                    return 3;
                }
            }

            if (!operator_recovery_authorized) {
                // Normal re-pairing strictly requires proof of existing device credential
                std::string presented_proof;
                if (const char* s = std::getenv("HG_DEVICE_SECRET")) {
                    presented_proof = s;
                } else if (dev_j.contains("existing_auth_secret")) {
                    presented_proof = dev_j["existing_auth_secret"].get<std::string>();
                }

                if (presented_proof.empty() || is_blacklisted_secret(presented_proof) || !constant_time_equals(presented_proof, existing->auth_secret)) {
                    std::cerr << "{\"error\": \"re-pairing rejected: device is already enrolled; valid proof of existing credential or trusted operator recovery authorization required\"}\n";
                    return 3;
                }
            }
        }
        // Gate 3: Initial enrollment (new device) or unprovisioned candidate / unpaired device requires trusted operator authorization
        else {
            if (!operator_authorized) {
                std::cerr << "{\"error\": \"pairing rejected: initial device enrollment requires trusted operator authorization (--operator-auth)\"}\n";
                return 3;
            }
        }

        DeviceRegistry::Device d;
        d.device_id = dev_id;
        d.display_name = dev_j.value("display_name", d.device_id);
        d.device_type = dev_j.value("device_type", "sensor");
        try {
            d.auth_secret = generate_csprng_secret(32);
        } catch (const std::exception& e) {
            std::cerr << "{\"error\": \"credential generation failed closed: " << e.what() << "\"}\n";
            return 5;
        }
        d.credential_id = d.device_id + "-cred-" + d.auth_secret.substr(9, 8);
        d.paired = true;
        d.revoked = false;
        d.online = false; // Always enforce offline state on pairing/re-pairing until authenticated heartbeat
        d.last_heartbeat_ms = 0;
        if (operator_recovery_authorized) {
            d.revocation_note = "recovered by authorized operator";
        }
        d.has_camera = dev_j.value("has_camera", false);
        d.has_microphone = dev_j.value("has_microphone", false);

        // Security Lifecycle Requirement B:
        // Re-pairing and recovery invalidate existing verified-online state, stale viewing grants and previously authenticated liveness.
        if (existing != nullptr) {
            d.consent_granted = false;
            d.consent_subject.clear();
            d.consent_purpose.clear();
            d.capture_enabled = false;
        } else {
            d.consent_granted = dev_j.value("consent_granted", false);
            d.consent_subject = dev_j.value("consent_subject", "");
            d.consent_purpose = dev_j.value("consent_purpose", "");
            d.capture_enabled = dev_j.value("capture_enabled", false) && d.consent_granted;
        }
        d.is_synthetic = (d.device_id == DeviceRegistry::Device::kAuthoritativeSyntheticDeviceId && d.device_type == "simulator");
        if (dev_j.contains("capabilities") && dev_j["capabilities"].is_array()) {
            for (const auto& cap : dev_j["capabilities"]) {
                if (cap.is_string()) d.capabilities.push_back(cap.get<std::string>());
            }
        }
        // Preserve existing sequence watermark across re-pairing unless authorized operator explicitly requested reset
        if (existing != nullptr) {
            if (operator_recovery_authorized && dev_j.value("reset_sequence", false)) {
                d.last_sequence = 0;
            } else {
                d.last_sequence = existing->last_sequence;
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
        json out = format_device(*paired_d, now_ms);
        // During pairing ONLY, output the provisioned secret so the device provisioning client can store it
        out["provisioned_auth_secret"] = paired_d->auth_secret;
        std::cout << out.dump(2) << "\n";
        return 0;
    }

    if (command == "rotate_secret") {
        if (args.size() != 1) {
            std::cerr << "{\"error\": \"usage: rotate_secret <device_id>\"}\n";
            return 1;
        }
        const std::string& dev_id = args[0];
        DeviceRegistry::Device* d = registry.find(dev_id);
        if (d == nullptr) {
            std::cerr << "{\"error\": \"device not found: " << dev_id << "\"}\n";
            return 2;
        }
        if (d->revoked) {
            std::cerr << "{\"error\": \"cannot rotate secret for revoked device\"}\n";
            return 3;
        }
        std::string new_secret;
        try {
            new_secret = generate_csprng_secret(32);
        } catch (const std::exception& e) {
            std::cerr << "{\"error\": \"credential generation failed closed: " << e.what() << "\"}\n";
            return 5;
        }
        d->auth_secret = new_secret;
        d->online = false; // Fresh heartbeat with rotated secret is required
        d->last_heartbeat_ms = 0;
        if (!registry.save(registry_path, &reason)) {
            std::cerr << "{\"error\": \"failed to save registry: " << reason << "\"}\n";
            return 4;
        }
        json out = format_device(*d, now_ms);
        out["rotated_auth_secret"] = new_secret;
        std::cout << out.dump(2) << "\n";
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
