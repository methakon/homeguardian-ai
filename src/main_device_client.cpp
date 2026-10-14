// hg_device_client: a mock Android transport client for the D-03 ingestion boundary.
//
// WHAT IT IS: the test harness that speaks the wire protocol the Android node will
// speak - mutually authenticated TLS, the framed versioned message format, hello
// then observations with sequence numbers and acquisition timestamps. It exists so
// the boundary can be exercised end to end on a host or inside a container without
// a phone, and so the acceptance script can drive ADVERSARIAL cases (replay,
// future timestamps, unlisted source types, oversized frames, wrong-device
// credentials) that a cooperative client would never produce.
//
// WHAT IT IS NOT: it is not the Android implementation and does not claim to be.
// The Android side (Java, APK) is a separate, compile-verified artifact; physical
// phone acceptance remains a separate gate.
//
// Every mode exits non-zero if the hub's answer is not the one the mode expects,
// so the acceptance script can assert behaviour rather than eyeball output.

#include "net/DeviceProtocol.h"
#include "net/TlsTransport.h"

#include <chrono>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>

using namespace homeguardian::net;

namespace {

int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

struct Options {
    std::string host = "127.0.0.1";
    uint16_t port = 8443;
    std::string ca_path;
    std::string cert_path;
    std::string key_path;
    std::string device_id;
    std::string source_type = "motion";
    std::string observation_type = "motion-changed-fraction";
    std::string mode = "normal";   // normal | replay | future | ancient | unknown-source |
                                   // oversized | claim-real | no-hello | goodbye
    int count = 3;
    int interval_ms = 0;
    uint64_t sequence_start = 1;
    double value = 0.25;
    bool expect_all_accepted = true;
};

void usage() {
    std::cerr << "usage: hg_device_client --ca <ca.pem> --cert <client.pem> --key <client.key>\n"
                 "                        --device-id <id> [--host 127.0.0.1] [--port 8443]\n"
                 "                        [--count N] [--mode normal|replay|future|ancient|\n"
                 "                         unknown-source|oversized|claim-real|no-hello|goodbye]\n"
                 "                        [--source-type T] [--observation-type T]\n"
                 "                        [--sequence-start N] [--value V] [--expect-reject]\n";
}

bool parse(int argc, char** argv, Options* options) {
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto next = [&](std::string* out) {
            if (i + 1 >= argc) return false;
            *out = argv[++i];
            return true;
        };
        std::string value;
        if (arg == "--ca") { if (!next(&options->ca_path)) return false; }
        else if (arg == "--cert") { if (!next(&options->cert_path)) return false; }
        else if (arg == "--key") { if (!next(&options->key_path)) return false; }
        else if (arg == "--device-id") { if (!next(&options->device_id)) return false; }
        else if (arg == "--host") { if (!next(&options->host)) return false; }
        else if (arg == "--source-type") { if (!next(&options->source_type)) return false; }
        else if (arg == "--observation-type") { if (!next(&options->observation_type)) return false; }
        else if (arg == "--mode") { if (!next(&options->mode)) return false; }
        else if (arg == "--port") { if (!next(&value)) return false; options->port = static_cast<uint16_t>(std::stoi(value)); }
        else if (arg == "--count") { if (!next(&value)) return false; options->count = std::stoi(value); }
        else if (arg == "--interval-ms") { if (!next(&value)) return false; options->interval_ms = std::stoi(value); }
        else if (arg == "--sequence-start") { if (!next(&value)) return false; options->sequence_start = std::stoull(value); }
        else if (arg == "--value") { if (!next(&value)) return false; options->value = std::stod(value); }
        else if (arg == "--expect-reject") { options->expect_all_accepted = false; }
        else if (arg == "--help" || arg == "-h") { usage(); return false; }
        else {
            std::cerr << "unknown argument: " << arg << "\n";
            return false;
        }
    }
    if (options->ca_path.empty() || options->cert_path.empty() || options->key_path.empty() ||
        options->device_id.empty()) {
        usage();
        return false;
    }
    return true;
}

DeviceMessage make_hello(const Options& options) {
    DeviceMessage hello;
    hello.protocol_version_major = kProtocolVersionMajor;
    hello.protocol_version_minor = kProtocolVersionMinor;
    hello.kind = MessageKind::hello;
    hello.device_id_claim = options.device_id;
    hello.display_name_claim = "mock android node";
    hello.capabilities = {"camera", "microphone"};
    return hello;
}

DeviceMessage make_observation(const Options& options, uint64_t sequence, const std::string& id) {
    DeviceMessage m;
    m.protocol_version_major = kProtocolVersionMajor;
    m.protocol_version_minor = kProtocolVersionMinor;
    m.kind = MessageKind::observation;
    m.observation_id = id;
    m.device_id_claim = options.device_id;
    m.source_type = options.source_type;
    m.observation_type = options.observation_type;
    m.acquisition_mode = "live";
    m.acquired_at_ms = now_ms() - 500;
    m.sequence = sequence;
    m.features = nlohmann::json::object();
    m.features["motion-changed-fraction"] = options.value;
    m.features["state"] = "on";
    m.provenance_claim = "real";
    m.confidence = 0.9;
    m.quality = 0.9;
    m.unit = "fraction";

    if (options.mode == "future") m.acquired_at_ms = now_ms() + 3'600'000;
    if (options.mode == "ancient") m.acquired_at_ms = now_ms() - (2LL * 24 * 3600 * 1000);
    if (options.mode == "unknown-source") {
        m.source_type = "emotion";
        m.observation_type = "facial-expression";
    }
    if (options.mode == "claim-real") m.acquisition_mode = "synthetic";  // claim real, not live
    if (options.mode == "oversized") {
        // Exceeds the hub's frame cap: the boundary must refuse before allocating.
        for (int i = 0; i < 400; ++i) m.features["pad" + std::to_string(i)] = std::string(32, 'x');
    }
    return m;
}

}  // namespace

int main(int argc, char** argv) {
    Options options;
    if (!parse(argc, argv, &options)) return 2;

    TlsClient client;
    std::string error;
    if (!client.connect(options.host, options.port, options.ca_path, options.cert_path,
                        options.key_path, 5000, &error)) {
        std::cerr << "client: TLS connect failed: " << error << "\n";
        return 5;
    }
    std::cout << "client: connected, hub CN=" << client.peer().device_id
              << " fingerprint=" << client.peer().credential_id.substr(0, 16) << "...\n";

    if (options.mode == "no-hello") {
        // Deliberately skip the handshake message: the hub must refuse.
        DeviceMessage observation = make_observation(options, options.sequence_start, "obs-nohello");
        client.send(observation, &error);
        DeviceMessage reply;
        if (client.receive(&reply, 3000, &error) && reply.kind == MessageKind::error) {
            std::cout << "client: hub refused a stream without hello ("
                      << reject_reason_to_string(reply.reason) << ")\n";
            return 0;
        }
        std::cerr << "client: hub did NOT refuse a stream without hello\n";
        return 6;
    }

    if (!client.send(make_hello(options), &error)) {
        std::cerr << "client: hello send failed: " << error << "\n";
        return 5;
    }
    DeviceMessage welcome;
    if (!client.receive(&welcome, 5000, &error) || welcome.kind != MessageKind::welcome) {
        // The hub refuses a hello with an error frame carrying a reason; printing
        // only the transport error made a refusal look like an unexplained hang.
        if (welcome.kind == MessageKind::error) {
            std::cerr << "client: hub refused the hello: reason="
                      << reject_reason_to_string(welcome.reason) << " detail=" << error << "\n";
        } else {
            std::cerr << "client: no welcome: " << error << "\n";
        }
        return 6;
    }
    std::cout << "client: welcome for device " << welcome.device_id_claim << "\n";

    if (options.mode == "goodbye") {
        DeviceMessage bye;
        bye.protocol_version_major = kProtocolVersionMajor;
        bye.kind = MessageKind::goodbye;
        client.send(bye, &error);
        std::cout << "client: sent goodbye\n";
        return 0;
    }

    int accepted = 0;
    int rejected = 0;
    for (int i = 0; i < options.count; ++i) {
        const uint64_t sequence = options.sequence_start + static_cast<uint64_t>(i);
        // Replay mode re-sends the SAME sequence twice.
        const uint64_t send_sequence =
            options.mode == "replay" && i % 2 == 1 ? options.sequence_start : sequence;
        const std::string id = "mock-" + options.device_id + "-" + std::to_string(send_sequence) +
                               "-" + std::to_string(i);
        const DeviceMessage observation = make_observation(options, send_sequence, id);
        if (!client.send(observation, &error)) {
            std::cerr << "client: send failed: " << error << "\n";
            return 5;
        }
        DeviceMessage ack;
        if (!client.receive(&ack, 5000, &error)) {
            std::cerr << "client: no ack: " << error << "\n";
            return 6;
        }
        if (ack.kind != MessageKind::acknowledgement) {
            std::cerr << "client: unexpected reply kind " << message_kind_to_string(ack.kind)
                      << " reason=" << reject_reason_to_string(ack.reason) << "\n";
            return 6;
        }
        const std::string reason = reject_reason_to_string(ack.reason);
        std::cout << "client: seq=" << send_sequence << " accepted=" << (ack.reason == RejectReason::none)
                  << " reason=" << reason << "\n";
        if (ack.reason == RejectReason::none) {
            ++accepted;
        } else {
            ++rejected;
        }
        if (options.interval_ms > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(options.interval_ms));
        }
    }

    std::cout << "client: done accepted=" << accepted << " rejected=" << rejected << "\n";

    if (options.mode == "replay" && rejected == 0) {
        std::cerr << "client: EXPECTED the hub to reject replayed sequences\n";
        return 7;
    }
    if (options.expect_all_accepted) {
        const int expected_rejects =
            (options.mode == "replay") ? (options.count / 2) : 0;
        if (rejected != expected_rejects) {
            std::cerr << "client: expected " << expected_rejects << " rejections, saw " << rejected
                      << "\n";
            return 7;
        }
    } else if (rejected == 0) {
        std::cerr << "client: expected at least one rejection\n";
        return 7;
    }
    return 0;
}
