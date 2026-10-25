#pragma once

// Read-only kernel neighbor dump for ordinary Android app UIDs. No process,
// packet to a LAN host, credential, privilege request, or address adoption here.
#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <cerrno>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace homeguardian::portal::neighbor_netlink {
inline constexpr size_t kMaxBytes = 256u * 1024u;
inline constexpr size_t kMaxEntries = 1024;

// All messages must be complete and from the kernel, with the expected sequence.
// Nothing is exposed until NLMSG_DONE, so a truncated/conflicting partial dump
// cannot authorize credentials before a later datagram is inspected.
class DumpParser {
public:
    explicit DumpParser(uint32_t sequence = 1) : sequence_(sequence) {}
    bool append(const void* raw, size_t size, uint32_t sender) {
        if (failed_ || done_ || sender != 0 || !raw || !size || size > kMaxBytes - bytes_) return reject();
        bytes_ += size;
        const auto* data = static_cast<const unsigned char*>(raw);
        size_t offset = 0;
        while (offset < size) {
            if (done_ || size - offset < sizeof(nlmsghdr)) return reject();
            nlmsghdr h{}; std::memcpy(&h, data + offset, sizeof(h));
            if (h.nlmsg_len < NLMSG_HDRLEN || h.nlmsg_len > size - offset ||
                h.nlmsg_seq != sequence_ || (h.nlmsg_flags & NLM_F_DUMP_INTR)) return reject();
            const size_t aligned = NLMSG_ALIGN(static_cast<size_t>(h.nlmsg_len));
            if (aligned > size - offset) return reject();
            const auto* payload = data + offset + NLMSG_HDRLEN;
            const size_t length = h.nlmsg_len - NLMSG_HDRLEN;
            if (h.nlmsg_type == NLMSG_DONE) {
                int result = 0;
                if (length && length < sizeof(result)) return reject();
                if (length) std::memcpy(&result, payload, sizeof(result));
                if (result != 0) return reject();
                done_ = true;
            } else if (h.nlmsg_type == NLMSG_ERROR) {
                nlmsgerr error{};
                if (length < sizeof(error)) return reject();
                std::memcpy(&error, payload, sizeof(error));
                if (error.error != 0) return reject();
            } else if (h.nlmsg_type == RTM_NEWNEIGH) {
                if (++entries_ > kMaxEntries || !neighbor(payload, length)) return reject();
            } else if (h.nlmsg_type != NLMSG_NOOP) return reject();
            offset += aligned;
        }
        return true;
    }
    bool complete() const { return done_ && !failed_; }
    std::string table() const {
        return complete() ? "IP address HW type Flags HW address Mask Device\n" + rows_ : std::string();
    }
private:
    bool reject() { failed_ = true; rows_.clear(); return false; }
    bool neighbor(const unsigned char* data, size_t size) {
        if (size < sizeof(ndmsg)) return false;
        ndmsg n{}; std::memcpy(&n, data, sizeof(n));
        if (n.ndm_family != AF_INET) return true;
        if (n.ndm_ifindex <= 0) return false;
        constexpr unsigned resolved = NUD_REACHABLE | NUD_STALE | NUD_DELAY | NUD_PROBE | NUD_PERMANENT;
        // Android VPN/tunnel NOARP records can legitimately have no hardware
        // address. They are not Ethernet identities and must not poison a dump.
        if (!(n.ndm_state & resolved) || (n.ndm_state & (NUD_FAILED | NUD_INCOMPLETE))) return true;
        std::array<unsigned char, 4> ip{};
        std::array<unsigned char, 6> mac{};
        bool hasIp = false, hasMac = false;
        size_t offset = NLMSG_ALIGN(sizeof(ndmsg));
        while (offset < size) {
            if (size - offset < sizeof(rtattr)) return false;
            rtattr a{}; std::memcpy(&a, data + offset, sizeof(a));
            if (a.rta_len < RTA_LENGTH(0) || a.rta_len > size - offset) return false;
            const size_t aligned = RTA_ALIGN(static_cast<size_t>(a.rta_len));
            if (aligned > size - offset) return false;
            const size_t length = a.rta_len - RTA_LENGTH(0);
            const auto* value = data + offset + RTA_LENGTH(0);
            const unsigned type = a.rta_type & NLA_TYPE_MASK;
            if (type == NDA_DST) {
                if (hasIp || length != ip.size() || a.rta_type != type) return false;
                std::memcpy(ip.data(), value, ip.size()); hasIp = true;
            } else if (type == NDA_LLADDR) {
                if (hasMac || length != mac.size() || a.rta_type != type) return false;
                std::memcpy(mac.data(), value, mac.size()); hasMac = true;
            }
            offset += aligned;
        }
        if (!hasIp || !hasMac ||
            ip[0] == 0 || ip[0] == 127 || ip[0] >= 224 || (mac[0] & 1) ||
            std::all_of(mac.begin(), mac.end(), [](unsigned char b) { return b == 0; })) return true;
        std::string line = std::to_string(ip[0]) + "." + std::to_string(ip[1]) + "." +
                           std::to_string(ip[2]) + "." + std::to_string(ip[3]) + " 0x1 0x2 ";
        constexpr char hex[] = "0123456789abcdef";
        for (size_t i = 0; i < mac.size(); ++i) {
            if (i) line += ':';
            line += hex[mac[i] >> 4]; line += hex[mac[i] & 15];
        }
        line += " * if" + std::to_string(n.ndm_ifindex) + "\n";
        if (line.size() > kMaxBytes - rows_.size()) return false;
        rows_ += line;
        return true;
    }
    uint32_t sequence_;
    size_t bytes_ = 0, entries_ = 0;
    bool done_ = false, failed_ = false;
    std::string rows_;
};

inline std::string read_table() {
    struct Socket {
        int fd = ::socket(AF_NETLINK, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, NETLINK_ROUTE);
        ~Socket() { if (fd >= 0) ::close(fd); }
    } socket;
    if (socket.fd < 0) return {};
    // sendto() auto-assigns a private netlink port. An explicit bind is denied
    // for ordinary Android app UIDs even when this read-only request is allowed.
    struct { nlmsghdr header; ndmsg neighbor; } request{};
    request.header.nlmsg_len = NLMSG_LENGTH(sizeof(ndmsg));
    request.header.nlmsg_type = RTM_GETNEIGH;
    request.header.nlmsg_flags = NLM_F_REQUEST | NLM_F_DUMP;
    request.header.nlmsg_seq = 1;
    request.neighbor.ndm_family = AF_INET;
    sockaddr_nl kernel{}; kernel.nl_family = AF_NETLINK;
    if (::sendto(socket.fd, &request, request.header.nlmsg_len, 0,
                 reinterpret_cast<sockaddr*>(&kernel), sizeof(kernel)) != request.header.nlmsg_len) return {};
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(200);
    DumpParser parser;
    std::array<unsigned char, 32768> buffer{};
    while (!parser.complete()) {
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
        if (left <= 0) return {};
        pollfd p{socket.fd, POLLIN, 0};
        const int ready = ::poll(&p, 1, static_cast<int>(left));
        if (ready < 0 && errno == EINTR) continue;
        if (ready <= 0 || (p.revents & (POLLERR | POLLHUP | POLLNVAL))) return {};
        if (!(p.revents & POLLIN)) continue;
        sockaddr_nl from{};
        iovec vector{buffer.data(), buffer.size()};
        msghdr message{};
        message.msg_name = &from; message.msg_namelen = sizeof(from);
        message.msg_iov = &vector; message.msg_iovlen = 1;
        const ssize_t count = ::recvmsg(socket.fd, &message, MSG_DONTWAIT);
        if (count < 0 && (errno == EINTR || errno == EAGAIN)) continue;
        if (count <= 0 || (message.msg_flags & (MSG_TRUNC | MSG_CTRUNC)) ||
            message.msg_namelen != sizeof(from) || from.nl_family != AF_NETLINK || from.nl_groups != 0 ||
            !parser.append(buffer.data(), static_cast<size_t>(count), from.nl_pid)) return {};
    }
    return parser.table();
}
}  // namespace homeguardian::portal::neighbor_netlink
