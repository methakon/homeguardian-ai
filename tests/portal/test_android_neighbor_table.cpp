// Hermetic kernel-message fixtures only; no sockets, camera, or LAN traffic.
#include "bridge/AndroidNeighborTable.h"
#include "bridge/CameraLocator.h"
#include <linux/rtnetlink.h>
#include <sys/socket.h>
#include <array>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <vector>
using Bytes = std::vector<unsigned char>;
using homeguardian::portal::neighbor_netlink::DumpParser;
size_t assertions = 0;
void require(bool condition, const char* reason) { if (!condition) throw std::runtime_error(reason); ++assertions; }
Bytes message(uint16_t type, const Bytes& payload = {}, uint16_t flags = 0, uint32_t sequence = 7) {
    nlmsghdr header{}; header.nlmsg_len = NLMSG_LENGTH(payload.size()); header.nlmsg_type = type;
    header.nlmsg_flags = flags; header.nlmsg_seq = sequence;
    Bytes out(NLMSG_ALIGN(header.nlmsg_len));
    std::memcpy(out.data(), &header, sizeof(header));
    if (!payload.empty()) std::memcpy(out.data() + NLMSG_HDRLEN, payload.data(), payload.size());
    return out;
}
void attribute(Bytes& out, uint16_t type, const Bytes& value) {
    rtattr a{}; a.rta_type = type; a.rta_len = RTA_LENGTH(value.size());
    const size_t start = out.size(); out.resize(start + RTA_ALIGN(a.rta_len));
    std::memcpy(out.data() + start, &a, sizeof(a));
    if (!value.empty()) std::memcpy(out.data() + start + RTA_LENGTH(0), value.data(), value.size());
}
Bytes neighbor(uint16_t state = NUD_REACHABLE, bool duplicate = false, unsigned char firstMac = 2, bool emptyHardware = false) {
    ndmsg n{}; n.ndm_family = AF_INET; n.ndm_ifindex = 2; n.ndm_state = state;
    Bytes body(NLMSG_ALIGN(sizeof(n))); std::memcpy(body.data(), &n, sizeof(n));
    attribute(body, NDA_DST, {192, 0, 2, 8});
    if (duplicate) attribute(body, NDA_DST, {192, 0, 2, 9});
    attribute(body, NDA_LLADDR, emptyHardware ? Bytes{} : Bytes{firstMac, 0, 0, 0, 0, 1});
    return message(RTM_NEWNEIGH, body, NLM_F_MULTI);
}
bool append(DumpParser& p, const Bytes& b, uint32_t sender = 0) { return p.append(b.data(), b.size(), sender); }
int main() {
    try {
        DumpParser parser(7);
        require(append(parser, neighbor()), "valid neighbor accepted");
        require(parser.table().empty(), "partial dump must not authorize a destination");
        require(append(parser, message(NLMSG_DONE)), "dump completes");
        require(parser.table().find("192.0.2.8 0x1 0x2 02:00:00:00:00:01 * if2") != std::string::npos, "complete IPv4/MAC mapping retained");
        auto bad = [](const Bytes& b, uint32_t sender = 0) {
            DumpParser p(7);
            require(!append(p, b, sender), "malformed/untrusted dump rejected");
            require(p.table().empty(), "failure never exposes a partial identity table");
        };
        bad(neighbor(), 123);
        bad(message(RTM_NEWNEIGH, {}, 0, 8));
        bad(Bytes(3));
        bad(message(RTM_NEWNEIGH, Bytes(2)));
        bad(message(NLMSG_OVERRUN));
        bad(message(NLMSG_DONE, Bytes(1)));
        bad(neighbor(NUD_REACHABLE, true));
        auto truncated = neighbor(); truncated.pop_back(); bad(truncated);
        auto junk = neighbor(); junk.push_back(0); bad(junk);
        auto interrupted = message(NLMSG_DONE, {}, NLM_F_DUMP_INTR); bad(interrupted);
        nlmsgerr error{}; error.error = -EPERM;
        Bytes errorBytes(sizeof(error)); std::memcpy(errorBytes.data(), &error, sizeof(error));
        bad(message(NLMSG_ERROR, errorBytes));
        int interruptedResult = -EINTR;
        Bytes result(sizeof(interruptedResult)); std::memcpy(result.data(), &interruptedResult, sizeof(interruptedResult));
        bad(message(NLMSG_DONE, result));
        bad(Bytes(homeguardian::portal::neighbor_netlink::kMaxBytes + 1));
        auto shortAttribute = neighbor();
        uint16_t shortLength = 7;
        std::memcpy(shortAttribute.data() + NLMSG_HDRLEN + NLMSG_ALIGN(sizeof(ndmsg)), &shortLength, sizeof(shortLength));
        bad(shortAttribute);
        for (auto state : {NUD_FAILED, NUD_INCOMPLETE, NUD_NONE}) {
            DumpParser p(7);
            require(append(p, neighbor(state)), "unresolved state is safely ignored");
            require(append(p, message(NLMSG_DONE)), "ignored state completes");
            require(p.table().find("192.0.2.8") == std::string::npos, "unresolved neighbor is not identity");
        }
        for (auto state : {NUD_STALE, NUD_DELAY, NUD_PROBE, NUD_PERMANENT}) {
            DumpParser p(7);
            require(append(p, neighbor(state)), "resolved state accepted");
            require(append(p, message(NLMSG_DONE)), "resolved state completes");
            require(p.table().find("192.0.2.8") != std::string::npos, "resolved mapping retained");
        }
        DumpParser multicast(7);
        require(append(multicast, neighbor(NUD_REACHABLE, false, 3)), "multicast MAC ignored");
        require(append(multicast, message(NLMSG_DONE)), "multicast dump complete");
        require(multicast.table().find("192.0.2.8") == std::string::npos, "multicast is not an identity");
        DumpParser tooMany(7);
        for (size_t i = 0; i < homeguardian::portal::neighbor_netlink::kMaxEntries; ++i)
            require(append(tooMany, neighbor()), "bounded entries accepted");
        require(!append(tooMany, neighbor()), "entry bound enforced");
        require(tooMany.table().empty(), "oversized dump discards partial identity");
        require(!append(parser, neighbor()), "messages after completion refused");
        require(parser.table().empty(), "post-completion corruption fails closed");
        DumpParser mixed(7);
        require(append(mixed, neighbor(NUD_NOARP, false, 2, true)), "Android tunnel NOARP empty hardware entry is ignored");
        require(append(mixed, neighbor()), "real Ethernet neighbor after tunnel entry accepted");
        require(append(mixed, message(NLMSG_DONE)), "mixed Android dump completes");
        require(mixed.table().find("192.0.2.8") != std::string::npos, "tunnel entry cannot poison valid mappings");
        const std::string mac = "02:00:00:00:00:01";
        for (const auto& conflict : {
                std::string("192.0.2.8 0x1 0x2 ") + mac + " * if2\n192.0.2.9 0x1 0x2 " + mac + " * if2\n",
                std::string("192.0.2.8 0x1 0x2 ") + mac + " * if2\n192.0.2.8 0x1 0x2 02:00:00:00:00:02 * if3\n"}) {
            using namespace homeguardian::portal;
            const auto entries = parse_arp(conflict);
            require(!resolve_host_by_mac(entries, mac, 0xc0000200u), "ambiguous whole-pass identity cannot be selected");
            LocatorIo io; io.read_arp = [conflict] { return conflict; };
            CameraLocator locator("192.0.2.1", 80, "/snapshot", mac, io);
            require(!locator.destination_matches("192.0.2.8"), "ambiguous table cannot authorize credentials");
        }
        std::cout << "PASS " << assertions << " netlink/identity assertions (no network)\n";
        return 0;
    } catch (const std::exception& e) { std::cerr << "FAIL " << e.what() << '\n'; return 1; }
}
