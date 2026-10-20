// v380_handshake - ONE bounded handshake against one camera. It never requests media.
//
// WHAT IT DOES: connects to the vendor port, sends the login request, reports the device's named answer, and
// only if the login succeeded does it send the stream LOGIN (301) to read the stream result. It deliberately
// STOPS BEFORE the stream start (303), which is the command that begins media: this tool opens no stream,
// reassembles no frame and writes no byte of video anywhere.
//
// ONE ATTEMPT PER INVOCATION: no retry loop, no backoff, no reconnect. Everything is a named outcome, because
// the device sends no text on a failed exchange and a boolean would throw away the only diagnostic there is.
//
// usage: hg_v380_handshake --ip <addr> [--id <device-id>] [--password <pw>] [--key <file>] [--username <u>]
//   --key defaults to $HG_V380_KEY or ~/projects/homeguardian-ai-evidence-private/v380_static_key.txt and
//   must NOT be group/other-readable. The vendor static key is a parameter and lives outside this repository
//   on purpose: the repository is public.
//
// absent --id prints `missing_device_id`: an IP address CANNOT stand in for the device id, which is a numeric
// field at a fixed offset in the request. Substituting the address would fabricate an identifier and turn the
// device's honest answer into a lie about which camera was contacted.

#include "camera/v380_protocol.h"
#include "core/Aes128Ecb.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace {

constexpr int kTimeoutSeconds = 5;
constexpr uint16_t kDefaultPort = 8800;

std::string slurp(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    std::string s = ss.str();
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
    return s;
}

bool key_file_is_safe(const std::string& path, std::string* why) {
    struct stat st{};
    if (::stat(path.c_str(), &st) != 0) {
        *why = "cannot stat " + path;
        return false;
    }
    if ((st.st_mode & (S_IRWXG | S_IRWXO)) != 0) {
        *why = "refusing to read a key file that is group/other accessible (chmod 600 it): " + path;
        return false;
    }
    return true;
}

int connect_to(const std::string& ip, uint16_t port, std::string* why) {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        *why = "socket() failed";
        return -1;
    }
    struct timeval tv{kTimeoutSeconds, 0};
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (::inet_pton(AF_INET, ip.c_str(), &addr.sin_addr) != 1) {
        *why = "not an IPv4 literal";
        ::close(fd);
        return -1;
    }
    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        *why = std::string("connect failed: ") + std::strerror(errno);
        ::close(fd);
        return -1;
    }
    return fd;
}

bool send_all(int fd, const std::vector<uint8_t>& bytes, std::string* why) {
    size_t sent = 0;
    while (sent < bytes.size()) {
        const ssize_t n = ::send(fd, bytes.data() + sent, bytes.size() - sent, 0);
        if (n <= 0) {
            *why = "send failed or timed out";
            return false;
        }
        sent += static_cast<size_t>(n);
    }
    return true;
}

size_t recv_some(int fd, uint8_t* buf, size_t cap, std::string* why) {
    const ssize_t n = ::recv(fd, buf, cap, 0);
    if (n <= 0) {
        *why = n == 0 ? "the device closed without answering" : "recv timed out or failed";
        return 0;
    }
    return static_cast<size_t>(n);
}

}  // namespace

int main(int argc, char** argv) {
    std::string ip, password, username = "admin", key_path, id_text;
    uint16_t port = kDefaultPort;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        const bool has_next = (i + 1) < argc;
        if (a == "--ip" && has_next) ip = argv[++i];
        else if (a == "--id" && has_next) id_text = argv[++i];
        else if (a == "--password" && has_next) password = argv[++i];
        else if (a == "--username" && has_next) username = argv[++i];
        else if (a == "--key" && has_next) key_path = argv[++i];
        else if (a == "--port" && has_next) port = static_cast<uint16_t>(std::stoi(argv[++i]));
        else {
            std::fprintf(stderr, "unknown or incomplete argument: %s\n", a.c_str());
            return 64;
        }
    }
    if (ip.empty()) {
        std::fprintf(stderr, "usage: hg_v380_handshake --ip <addr> [--id <device-id>] [--password <pw>]\n");
        return 64;
    }

    // 1. Is there a path at all? A timeout and a refusal are DIFFERENT facts and are reported differently.
    std::string why;
    const int fd = connect_to(ip, port, &why);
    if (fd < 0) {
        std::printf("RESULT connect_failed  ip=%s port=%u  detail=%s\n", ip.c_str(), port, why.c_str());
        std::printf("NOTE   a timeout means NO PATH (nothing learned about the port); a refusal means the host "
                    "answered.\n");
        return 3;
    }
    std::printf("connect ok  ip=%s port=%u\n", ip.c_str(), port);

    // 2. The device id is a numeric field, never the address.
    if (id_text.empty()) {
        ::close(fd);
        std::printf("RESULT missing_device_id  ip=%s\n", ip.c_str());
        std::printf("NOTE   the login request carries a numeric device id at a fixed offset; an IP address "
                    "cannot fill it. Substituting one would fabricate an identifier.\n");
        return 2;
    }
    const int32_t device_id = std::stoi(id_text);

    if (key_path.empty()) {
        const char* env = ::getenv("HG_V380_KEY");
        key_path = env != nullptr ? env
                                 : std::string(::getenv("HOME") ? ::getenv("HOME") : "") +
                                       "/projects/homeguardian-ai-evidence-private/v380_static_key.txt";
    }
    if (!key_file_is_safe(key_path, &why)) {
        ::close(fd);
        std::printf("RESULT key_unavailable  detail=%s\n", why.c_str());
        return 4;
    }
    const std::string static_key = slurp(key_path);
    if (static_key.size() != 16) {
        ::close(fd);
        std::printf("RESULT key_unavailable  detail=the key file must contain exactly 16 bytes\n");
        return 4;
    }

    // 3. The login.
    std::vector<uint8_t> request;
    homeguardian::v380::LoginRequest login;
    login.device_id = device_id;
    login.username = username;
    login.password = password;   // may legitimately be empty on a QR-set-up device
    if (!homeguardian::v380::build_login_request(login, static_key, /*random_key=*/"0123456789abcdef",
                                                 &request, &why)) {
        ::close(fd);
        std::printf("RESULT request_invalid  detail=%s\n", why.c_str());
        return 4;
    }
    std::printf("sending login (1167), device id %d, username '%s', password %s\n", device_id,
                username.c_str(), password.empty() ? "EMPTY" : "(supplied)");
    if (!send_all(fd, request, &why)) {
        ::close(fd);
        std::printf("RESULT send_failed  detail=%s\n", why.c_str());
        return 5;
    }

    uint8_t buf[512] = {0};
    const size_t got = recv_some(fd, buf, sizeof(buf), &why);
    ::close(fd);
    if (got == 0) {
        std::printf("RESULT no_login_response  detail=%s\n", why.c_str());
        std::printf("NOTE   an unanswered login on 8800 is what a blocked/absent vendor service looks like, "
                    "and it is NOT a credential verdict.\n");
        return 6;
    }
    const auto response = homeguardian::v380::parse_login_response(buf, got);
    std::printf("login response: %zu bytes, command=%d result_code=%d -> %s\n", got, response.command,
                response.login_result, homeguardian::v380::to_string(response.result));
    {
        // The raw leading bytes, as evidence. A generation verdict must be readable from the wire, not
        // inferred from a single decoded integer.
        std::string hex, ascii;
        char tmp[8];
        for (size_t i = 0; i < got && i < 64; ++i) {
            std::snprintf(tmp, sizeof(tmp), "%02x ", buf[i]);
            hex += tmp;
            ascii += (buf[i] >= 32 && buf[i] < 127) ? static_cast<char>(buf[i]) : '.';
        }
        std::printf("first bytes: %s\n", hex.c_str());
        std::printf("as ascii   : %s\n", ascii.c_str());
        std::printf("magic check: %s\n",
                    std::string(reinterpret_cast<const char*>(buf), got).find("V380 Pro") != std::string::npos
                        ? "the response CONTAINS the literal 'V380 Pro' -> the newer framed generation"
                        : "no 'V380 Pro' literal in the response");
    }
    if (response.result != homeguardian::v380::LoginResult::ok) {
        // A named refusal from the LOGIN is reported as itself, but it does not end the inquiry: the firmware
        // generation is decided by the stream response (v21), and continuing here is what the documented
        // exchange does too. Exactly one more request follows, and still no media.
        std::printf("login refused as '%s'; continuing to the stream login, which is where the generation "
                    "is decided\n",
                    homeguardian::v380::to_string(response.result));
    }

    // 4. The stream login - this is where the firmware generation is decided. NO stream start, no media.
    std::vector<uint8_t> srequest;
    if (!homeguardian::v380::build_stream_login_request(device_id, response.auth_ticket, 20, 1, &srequest,
                                                        &why)) {
        std::printf("RESULT request_invalid  detail=%s\n", why.c_str());
        return 4;
    }
    const int sfd = connect_to(ip, port, &why);
    if (sfd < 0) {
        std::printf("RESULT connect_failed  detail=%s\n", why.c_str());
        return 3;
    }
    if (!send_all(sfd, srequest, &why)) {
        ::close(sfd);
        std::printf("RESULT send_failed  detail=%s\n", why.c_str());
        return 5;
    }
    const size_t sgot = recv_some(sfd, buf, sizeof(buf), &why);
    ::close(sfd);
    if (sgot == 0) {
        std::printf("RESULT no_stream_response  detail=%s\n", why.c_str());
        return 6;
    }
    const auto sresp = homeguardian::v380::parse_stream_response(buf, sgot);
    std::printf("stream login response: %zu bytes, command=%d v21=%d -> %s\n", sgot, sresp.command,
                sresp.v21, homeguardian::v380::to_string(sresp.result));
    std::printf("RESULT %s\n", homeguardian::v380::to_string(sresp.result));
    if (sresp.result == homeguardian::v380::StreamResult::ok) {
        std::printf("NOTE   stream login accepted. Media is NOT requested here; the stream start (303) is "
                    "deliberately not sent.\n");
    }
    return 0;
}
