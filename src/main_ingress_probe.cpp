// hg_ingress_probe - service-specific health probe for the hg_device_ingress container.
//
//   hg_ingress_probe --ca /etc/homeguardian/certs/ca.crt --verify-name localhost [--port 8443]
//
// Exit codes: 0 = the ingress is up and its certificate verified for the expected name/address;
//             1 = not healthy (unreachable, not TLS, wrong/untrusted certificate, or no CA given).
//             2 = usage error.
//
// It reports exactly one line, including what it did NOT do (device_auth=NOT_PERFORMED), so a log
// reader cannot mistake a healthy ingress for an authenticated device.

#include "net/IngressHealthProbe.h"

#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>

namespace {

void print_usage() {
    std::cerr << "usage: hg_ingress_probe --ca <ca.crt> (--verify-name <dns-name> | --verify-ip <addr>)\n"
                 "                        [--host 127.0.0.1] [--port 8443] [--timeout-ms 4000]\n";
}

bool next_value(int argc, char** argv, int& i, std::string* out) {
    if (i + 1 >= argc) return false;
    *out = argv[++i];
    return true;
}

} // namespace

int main(int argc, char** argv) {
    homeguardian::net::IngressProbeOptions options;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            print_usage();
            return 2;
        } else if (arg == "--ca") {
            if (!next_value(argc, argv, i, &options.ca_file)) return 2;
        } else if (arg == "--verify-name") {
            if (!next_value(argc, argv, i, &options.verify_name)) return 2;
        } else if (arg == "--verify-ip") {
            if (!next_value(argc, argv, i, &options.verify_ip)) return 2;
        } else if (arg == "--host") {
            if (!next_value(argc, argv, i, &options.connect_host)) return 2;
        } else if (arg == "--port") {
            std::string value;
            if (!next_value(argc, argv, i, &value)) return 2;
            options.port = std::atoi(value.c_str());
            if (options.port <= 0 || options.port > 65535) return 2;
        } else if (arg == "--timeout-ms") {
            std::string value;
            if (!next_value(argc, argv, i, &value)) return 2;
            options.timeout_ms = std::atoi(value.c_str());
            if (options.timeout_ms < 100) return 2;
        } else {
            std::cerr << "hg_ingress_probe: unknown argument: " << arg << "\n";
            print_usage();
            return 2;
        }
    }

    if (options.ca_file.empty() ||
        (options.verify_name.empty() && options.verify_ip.empty())) {
        print_usage();
        return 2;
    }

    const auto result = homeguardian::net::probe_ingress_endpoint(options);
    std::cout << result.summary() << std::endl;
    return result.ok ? 0 : 1;
}
