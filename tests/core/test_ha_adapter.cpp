#include "catch2/catch.hpp"

#include "core/HomeAssistantAdapter.h"
#include "core/HttpClient.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

using namespace homeguardian;

namespace {

// A deterministic transport: no network, exact request accounting.
class StubTransport : public IHomeAssistantTransport {
public:
    std::map<std::string, Response> responses;
    mutable std::vector<std::string> requests;
    std::string describe_value = "stub transport (no network)";

    Response get(const std::string& path) override {
        requests.push_back(path);
        const auto it = responses.find(path);
        if (it != responses.end()) return it->second;
        Response missing;
        missing.ok = false;
        missing.status = 404;
        missing.error = "not found in stub";
        return missing;
    }

    std::string describe() const override { return describe_value; }
};

IHomeAssistantTransport::Response ok_json(const std::string& body) {
    IHomeAssistantTransport::Response r;
    r.ok = true;
    r.status = 200;
    r.body = body;
    return r;
}

HomeAssistantConfig enabled_config() {
    HomeAssistantConfig cfg;
    cfg.enabled = true;
    cfg.base_url = "http://127.0.0.1:8123";
    cfg.token_file = "/run/secrets/ha_token";
    cfg.entity_allowlist = {"binary_sensor.front_door", "sensor.living_room_temperature",
                            "camera.driveway"};
    cfg.allow_plaintext = true;
    return cfg;
}

// A throwaway file with the requested mode.
std::string write_temp_file(const std::string& name, const std::string& content, mode_t mode) {
    const std::string path = "/tmp/hg_ha_test_" + std::to_string(::getpid()) + "_" + name;
    const int fd = ::open(path.c_str(), O_CREAT | O_TRUNC | O_WRONLY, mode);
    if (fd >= 0) {
        const ssize_t written = ::write(fd, content.data(), content.size());
        (void)written;
        ::close(fd);
        ::chmod(path.c_str(), mode);
    }
    return path;
}

// A one-shot HTTP server on 127.0.0.1, so the socket path itself is exercised.
class LoopbackServer {
public:
    LoopbackServer(std::string response, size_t truncate_to = 0)
        : response_(std::move(response)), truncate_to_(truncate_to) {
        fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        int one = 1;
        ::setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        struct sockaddr_in addr {};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = ::htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;
        if (::bind(fd_, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) != 0) {
            ok_ = false;
            return;
        }
        socklen_t len = sizeof(addr);
        if (::getsockname(fd_, reinterpret_cast<struct sockaddr*>(&addr), &len) != 0) {
            ok_ = false;
            return;
        }
        port_ = ::ntohs(addr.sin_port);
        ok_ = ::listen(fd_, 1) == 0;
        thread_ = std::thread([this] { serve_once(); });
    }

    ~LoopbackServer() {
        if (thread_.joinable()) thread_.join();
        if (fd_ >= 0) ::close(fd_);
    }

    bool ok() const { return ok_; }
    uint16_t port() const { return port_; }
    const std::string& request() const { return request_; }

private:
    void serve_once() {
        struct sockaddr_in peer {};
        socklen_t len = sizeof(peer);
        const int client = ::accept(fd_, reinterpret_cast<struct sockaddr*>(&peer), &len);
        if (client < 0) return;
        char buffer[1024];
        const ssize_t n = ::recv(client, buffer, sizeof(buffer), 0);
        if (n > 0) request_.assign(buffer, static_cast<size_t>(n));
        std::string body = response_;
        if (truncate_to_ > 0 && body.size() > truncate_to_) body.resize(truncate_to_);
        const ssize_t sent = ::send(client, body.data(), body.size(), 0);
        (void)sent;
        ::shutdown(client, SHUT_WR);
        ::close(client);
    }

    int fd_ = -1;
    uint16_t port_ = 0;
    bool ok_ = false;
    std::string response_;
    size_t truncate_to_ = 0;
    std::string request_;
    std::thread thread_;
};

}  // namespace

TEST_CASE("Home Assistant configuration fails closed until it is complete",
          "[ha][config]") {
    SECTION("disabled by default, and that is a reported problem") {
        HomeAssistantConfig cfg;
        REQUIRE_FALSE(cfg.enabled);
        const auto problems = cfg.problems();
        REQUIRE(problems.size() == 1);
        REQUIRE(problems[0].find("disabled") != std::string::npos);
    }

    SECTION("enabled but incomplete is refused with specific reasons") {
        HomeAssistantConfig cfg;
        cfg.enabled = true;
        auto problems = cfg.problems();
        REQUIRE(problems.size() >= 3);  // url, token file, allowlist

        cfg.base_url = "http://ha.local:8123";
        cfg.token_file = "/run/secrets/ha_token";
        cfg.entity_allowlist = {"binary_sensor.front_door"};
        problems = cfg.problems();
        REQUIRE(problems.size() == 1);
        REQUIRE(problems[0].find("plaintext endpoint refused") != std::string::npos);

        cfg.allow_plaintext = true;
        REQUIRE(cfg.problems().empty());
    }

    SECTION("an https endpoint is refused because this build has no TLS") {
        HomeAssistantConfig cfg = enabled_config();
        cfg.base_url = "https://ha.local:8123";
        const auto problems = cfg.problems();
        REQUIRE_FALSE(problems.empty());
        bool saw_tls_reason = false;
        for (const std::string& p : problems) {
            if (p.find("no TLS") != std::string::npos || p.find("TLS") != std::string::npos) {
                saw_tls_reason = true;
            }
        }
        REQUIRE(saw_tls_reason);
    }

    SECTION("an empty allowlist means nothing may be consumed") {
        HomeAssistantConfig cfg = enabled_config();
        cfg.entity_allowlist.clear();
        bool saw_allowlist = false;
        for (const std::string& p : cfg.problems()) {
            if (p.find("allowlist is empty") != std::string::npos) saw_allowlist = true;
        }
        REQUIRE(saw_allowlist);
    }

    SECTION("a malformed entity id is refused") {
        HomeAssistantConfig cfg = enabled_config();
        cfg.entity_allowlist = {"binary_sensor.front door"};  // space is not valid
        bool saw_shape = false;
        for (const std::string& p : cfg.problems()) {
            if (p.find("invalid shape") != std::string::npos) saw_shape = true;
        }
        REQUIRE(saw_shape);
    }

    SECTION("an oversized allowlist is refused") {
        HomeAssistantConfig cfg = enabled_config();
        cfg.entity_allowlist.clear();
        for (size_t i = 0; i <= HomeAssistantConfig::kMaxAllowlist; ++i) {
            cfg.entity_allowlist.push_back("binary_sensor.e" + std::to_string(i));
        }
        bool saw_size = false;
        for (const std::string& p : cfg.problems()) {
            if (p.find("too large") != std::string::npos) saw_size = true;
        }
        REQUIRE(saw_size);
    }
}

TEST_CASE("the Home Assistant token is read from a restricted file and never exposed",
          "[ha][secret]") {
    SECTION("a 0600 file is accepted and its trailing newline trimmed") {
        const std::string path = write_temp_file("ok.token", "secret-token-value\n", 0600);
        std::string why;
        const auto token = read_secret_file(path, &why);
        REQUIRE(token.has_value());
        REQUIRE(token.value() == "secret-token-value");
        REQUIRE(why.empty());
        ::unlink(path.c_str());
    }

    SECTION("a group- or other-readable file is refused") {
        const std::string path = write_temp_file("loose.token", "secret-token-value\n", 0644);
        std::string why;
        REQUIRE_FALSE(read_secret_file(path, &why).has_value());
        REQUIRE(why.find("group or other") != std::string::npos);
        ::unlink(path.c_str());
    }

    SECTION("missing, empty, oversized and non-regular paths are refused") {
        std::string why;
        REQUIRE_FALSE(read_secret_file("/tmp/definitely-not-a-token-file-" + std::to_string(::getpid()),
                                       &why)
                          .has_value());
        REQUIRE(why.find("not readable") != std::string::npos);

        const std::string empty = write_temp_file("empty.token", "", 0600);
        REQUIRE_FALSE(read_secret_file(empty, &why).has_value());
        REQUIRE(why.find("empty") != std::string::npos);
        ::unlink(empty.c_str());

        const std::string big = write_temp_file("big.token", std::string(5000, 'x'), 0600);
        REQUIRE_FALSE(read_secret_file(big, &why).has_value());
        REQUIRE(why.find("too large") != std::string::npos);
        ::unlink(big.c_str());

        std::string dir = "/tmp/hg_ha_dir_" + std::to_string(::getpid());
        ::mkdir(dir.c_str(), 0700);
        REQUIRE_FALSE(read_secret_file(dir, &why).has_value());
        REQUIRE(why.find("regular file") != std::string::npos);
        ::rmdir(dir.c_str());
    }
}

TEST_CASE("entity mapping is explicit and never guesses", "[ha][mapping]") {
    using T = SecuritySourceType;

    SECTION("binary sensors map by device_class only") {
        auto m = HomeAssistantAdapter::map_entity("binary_sensor.hall", "on",
                                                 {{"device_class", "motion"}});
        REQUIRE(m.handled);
        REQUIRE(m.source_type == T::motion);
        REQUIRE(m.state == "on");

        m = HomeAssistantAdapter::map_entity("binary_sensor.hall", "on",
                                            {{"device_class", "occupancy"}});
        REQUIRE(m.source_type == T::presence);

        m = HomeAssistantAdapter::map_entity("binary_sensor.door", "on",
                                            {{"device_class", "door"}});
        REQUIRE(m.source_type == T::contact);
        REQUIRE(m.state == "open");  // contact uses the open/closed vocabulary
        m = HomeAssistantAdapter::map_entity("binary_sensor.door", "off",
                                            {{"device_class", "door"}});
        REQUIRE(m.state == "closed");

        m = HomeAssistantAdapter::map_entity("binary_sensor.case", "on",
                                            {{"device_class", "tamper"}});
        REQUIRE(m.source_type == T::tamper);
    }

    SECTION("an unmapped or absent device_class is skipped with a reason") {
        auto m = HomeAssistantAdapter::map_entity("binary_sensor.thing", "on", nlohmann::json::object());
        REQUIRE_FALSE(m.handled);
        REQUIRE(m.reason.find("without a device_class") != std::string::npos);

        m = HomeAssistantAdapter::map_entity("binary_sensor.thing", "on",
                                            {{"device_class", "moisture"}});
        REQUIRE_FALSE(m.handled);
        REQUIRE(m.reason.find("unmapped device_class") != std::string::npos);

        // A sound sensor is an acoustic event: there is no honest mapping in this
        // vocabulary yet, so it is skipped rather than forced into "motion".
        m = HomeAssistantAdapter::map_entity("binary_sensor.noise", "on",
                                            {{"device_class", "sound"}});
        REQUIRE_FALSE(m.handled);
    }

    SECTION("cameras and environmental sensors map to their own types") {
        auto m = HomeAssistantAdapter::map_entity("camera.driveway", "idle", nlohmann::json::object());
        REQUIRE(m.handled);
        REQUIRE(m.source_type == T::camera_availability);
        REQUIRE(m.state == "online");

        m = HomeAssistantAdapter::map_entity("sensor.room_temp", "21.4",
                                            {{"device_class", "temperature"},
                                             {"unit_of_measurement", "C"}});
        REQUIRE(m.handled);
        REQUIRE(m.source_type == T::environmental_threshold);
        REQUIRE(m.value.has_value());
        REQUIRE(m.value.value() == Approx(21.4));
        REQUIRE(m.unit == "C");
        // No threshold is configured, so no crossing is claimed.
        REQUIRE(m.state == "within");
        REQUIRE(m.reason.find("no crossing claimed") != std::string::npos);

        m = HomeAssistantAdapter::map_entity("sensor.humidity", "55",
                                            {{"device_class", "humidity"}});
        REQUIRE(m.handled);
        m = HomeAssistantAdapter::map_entity("sensor.pressure", "1013.2",
                                            {{"device_class", "pressure"}});
        REQUIRE(m.handled);

        m = HomeAssistantAdapter::map_entity("sensor.battery", "88",
                                            {{"device_class", "battery"}});
        REQUIRE_FALSE(m.handled);
        m = HomeAssistantAdapter::map_entity("sensor.temp", "warm",
                                            {{"device_class", "temperature"}});
        REQUIRE_FALSE(m.handled);
        REQUIRE(m.reason.find("not numeric") != std::string::npos);
    }

    SECTION("an unavailable entity is an availability finding in any domain") {
        const auto m = HomeAssistantAdapter::map_entity("camera.driveway", "unavailable",
                                                       nlohmann::json::object());
        REQUIRE(m.handled);
        REQUIRE(m.source_type == T::device_availability);
        REQUIRE(m.state == "offline");
    }

    SECTION("invalid ids, unknown domains and empty states are refused") {
        REQUIRE_FALSE(HomeAssistantAdapter::map_entity("no_dot", "on", nlohmann::json::object()).handled);
        REQUIRE_FALSE(HomeAssistantAdapter::map_entity("switch.lamp", "on",
                                                      nlohmann::json::object())
                          .handled);
        REQUIRE_FALSE(HomeAssistantAdapter::map_entity("binary_sensor.hall", "",
                                                      nlohmann::json::object())
                          .handled);
        REQUIRE_FALSE(HomeAssistantAdapter::map_entity(std::string(200, 'a'), "on",
                                                      nlohmann::json::object())
                          .handled);
    }
}

TEST_CASE("the adapter performs no request until it is enabled and authorized",
          "[ha][adapter]") {
    const auto now = std::chrono::system_clock::now();

    SECTION("disabled: no request, and the reason says so") {
        HomeAssistantConfig cfg = enabled_config();
        cfg.enabled = false;
        auto stub = std::make_unique<StubTransport>();
        StubTransport* raw = stub.get();
        HomeAssistantAdapter adapter(cfg, std::move(stub));
        adapter.set_authorized(true);
        const auto result = adapter.poll_once(now);
        REQUIRE_FALSE(result.polled);
        REQUIRE(result.observations.empty());
        REQUIRE(raw->requests.empty());
        REQUIRE(result.error.find("disabled") != std::string::npos);
    }

    SECTION("enabled but unauthorized: no request at all (fail-closed)") {
        auto stub = std::make_unique<StubTransport>();
        StubTransport* raw = stub.get();
        HomeAssistantAdapter adapter(enabled_config(), std::move(stub));
        const auto result = adapter.poll_once(now);
        REQUIRE_FALSE(result.polled);
        REQUIRE(raw->requests.empty());
        REQUIRE(result.error.find("not authorized") != std::string::npos);
    }

    SECTION("invalid configuration: no request") {
        HomeAssistantConfig cfg = enabled_config();
        cfg.allow_plaintext = false;  // the plaintext gate is not satisfied
        auto stub = std::make_unique<StubTransport>();
        StubTransport* raw = stub.get();
        HomeAssistantAdapter adapter(cfg, std::move(stub));
        adapter.set_authorized(true);
        const auto result = adapter.poll_once(now);
        REQUIRE_FALSE(result.polled);
        REQUIRE(raw->requests.empty());
        REQUIRE(result.error.find("configuration invalid") != std::string::npos);
    }
}

TEST_CASE("authorized polling maps, de-duplicates and tracks liveness", "[ha][adapter]") {
    auto stub = std::make_unique<StubTransport>();
    StubTransport* raw = stub.get();
    raw->responses["/api/states/binary_sensor.front_door"] =
        ok_json(R"({"entity_id":"binary_sensor.front_door","state":"on","attributes":{"device_class":"door","friendly_name":"Front Door"}})");
    raw->responses["/api/states/sensor.living_room_temperature"] =
        ok_json(R"({"entity_id":"sensor.living_room_temperature","state":"21.4","attributes":{"device_class":"temperature","unit_of_measurement":"C"}})");
    raw->responses["/api/states/camera.driveway"] =
        ok_json(R"({"entity_id":"camera.driveway","state":"idle","attributes":{}})");

    HomeAssistantAdapter adapter(enabled_config(), std::move(stub));
    adapter.set_authorized(true);
    const auto t0 = std::chrono::system_clock::now();

    SECTION("only allowlisted entities are requested, and the mapping is real-provenance") {
        const auto result = adapter.poll_once(t0);
        REQUIRE(result.polled);
        REQUIRE(raw->requests.size() == 3);
        for (const std::string& path : raw->requests) {
            REQUIRE(path.find("/api/states/") == 0);
        }
        REQUIRE(result.entities_checked == 3);
        REQUIRE(result.errors == 0);

        bool saw_door = false;
        bool saw_temperature = false;
        for (const SecurityObservation& o : result.observations) {
            REQUIRE(o.provenance == Provenance::real);
            REQUIRE(o.authorized);
            REQUIRE(o.consent_purpose == "security_monitor");
            // The token must never reach an observation.
            const std::string dumped = SecurityObservation::to_json(o).dump();
            REQUIRE(dumped.find("Bearer") == std::string::npos);
            REQUIRE(dumped.find("secret") == std::string::npos);
            if (o.source_type == SecuritySourceType::contact && o.device_id == "binary_sensor.front_door") {
                saw_door = true;
                REQUIRE(o.state == "open");
                REQUIRE(o.features["device_class"] == "door");
            }
            if (o.source_type == SecuritySourceType::environmental_threshold) {
                saw_temperature = true;
                REQUIRE(o.features["value"] == Approx(21.4));
                REQUIRE(o.features["unit"] == "C");
            }
        }
        REQUIRE(saw_door);
        REQUIRE(saw_temperature);
    }

    SECTION("an unchanged second poll is fully de-duplicated") {
        const auto first = adapter.poll_once(t0);
        REQUIRE_FALSE(first.observations.empty());
        const auto second = adapter.poll_once(t0 + std::chrono::seconds(5));
        REQUIRE(second.duplicates >= 3);
        REQUIRE(second.observations.size() < first.observations.size());
    }

    SECTION("a changed state is delivered again after the cooldown") {
        adapter.poll_once(t0);
        raw->responses["/api/states/binary_sensor.front_door"] =
            ok_json(R"({"entity_id":"binary_sensor.front_door","state":"off","attributes":{"device_class":"door"}})");
        const auto later = adapter.poll_once(t0 + std::chrono::seconds(120));
        bool saw_closed = false;
        for (const SecurityObservation& o : later.observations) {
            if (o.device_id == "binary_sensor.front_door" &&
                o.source_type == SecuritySourceType::contact) {
                saw_closed = o.state == "closed";
            }
        }
        REQUIRE(saw_closed);
    }

    SECTION("an unreadable entity is counted as an error, not as a silence") {
        raw->responses.erase("/api/states/camera.driveway");
        const auto result = adapter.poll_once(t0);
        REQUIRE(result.errors >= 1);
        REQUIRE(result.entities_checked == 2);
        REQUIRE(result.error.find("entity read failed") != std::string::npos);
    }

    SECTION("liveness decays into exactly one offline observation") {
        adapter.poll_once(t0);
        raw->responses.clear();  // every read now fails
        adapter.poll_once(t0 + std::chrono::seconds(10));
        const auto result = adapter.poll_once(t0 + std::chrono::seconds(400));
        REQUIRE(result.availability.size() == 3);
        size_t offline = 0;
        for (const SecurityObservation& o : result.observations) {
            if (o.source_type == SecuritySourceType::device_availability && o.state == "offline") {
                ++offline;
            }
        }
        REQUIRE(offline == 3);
    }

    SECTION("a reconnect yields an availability observation only") {
        adapter.poll_once(t0);
        raw->responses.clear();  // every read fails, so liveness must decay
        const auto went_offline = adapter.poll_once(t0 + std::chrono::seconds(400));
        REQUIRE(went_offline.availability.size() == 3);
        raw->responses["/api/states/binary_sensor.front_door"] =
            ok_json(R"({"entity_id":"binary_sensor.front_door","state":"on","attributes":{"device_class":"door"}})");
        const auto back = adapter.poll_once(t0 + std::chrono::seconds(401));
        bool saw_reconnect = false;
        for (const SecurityObservation& o : back.observations) {
            if (o.source_type == SecuritySourceType::device_availability && o.state == "online" &&
                o.features.contains("first_seen") && o.features["first_seen"] == false) {
                saw_reconnect = true;
            }
        }
        REQUIRE(saw_reconnect);
    }

    SECTION("the transport description never contains a credential") {
        const std::string description = adapter.transport_description();
        REQUIRE(description.find("Bearer") == std::string::npos);
        REQUIRE(description.find("secret") == std::string::npos);
        REQUIRE(description.find("token") == std::string::npos);
    }
}

TEST_CASE("HTTP client: URL policy, header safety and a real loopback exchange",
          "[ha][http]") {
    SECTION("url parsing accepts the supported shape and refuses the rest") {
        const auto good = HttpClient::parse_url("http://127.0.0.1:8123/api/states/camera.x");
        REQUIRE(good.valid);
        REQUIRE(good.scheme == "http");
        REQUIRE(good.host == "127.0.0.1");
        REQUIRE(good.port == 8123);
        REQUIRE(good.path == "/api/states/camera.x");

        const auto default_port = HttpClient::parse_url("http://ha.local");
        REQUIRE(default_port.valid);
        REQUIRE(default_port.port == 80);
        REQUIRE(default_port.path == "/");

        const auto https = HttpClient::parse_url("https://ha.local:8123/");
        REQUIRE_FALSE(https.valid);
        REQUIRE(https.error.find("no TLS") != std::string::npos);

        REQUIRE_FALSE(HttpClient::parse_url("ftp://ha.local").valid);
        REQUIRE_FALSE(HttpClient::parse_url("ha.local:8123").valid);
        REQUIRE_FALSE(HttpClient::parse_url("http://").valid);
        REQUIRE_FALSE(HttpClient::parse_url("http://host:99999/").valid);
        REQUIRE_FALSE(HttpClient::parse_url("http://host:abc/").valid);
        REQUIRE_FALSE(HttpClient::parse_url(std::string(400, 'u')).valid);
    }

    SECTION("a header value with a line break is refused, not sanitised") {
        const auto url = HttpClient::parse_url("http://127.0.0.1:8123/api/states/x");
        std::string error;
        const std::string head = HttpClient::build_request_head(
            url, {{"Authorization", "Bearer abc\r\nX-Injected: yes"}}, &error);
        REQUIRE(head.empty());
        REQUIRE(error.find("line break") != std::string::npos);

        const std::string oversized = HttpClient::build_request_head(
            url, {{"X", std::string(HttpPolicy::kMaxHeaderValueChars + 1, 'v')}}, &error);
        REQUIRE(oversized.empty());
        REQUIRE(error.find("too long") != std::string::npos);
    }

    SECTION("a well-formed request carries the method, host and authorization") {
        const auto url = HttpClient::parse_url("http://127.0.0.1:8123/api/states/camera.x");
        std::string error;
        const std::string head =
            HttpClient::build_request_head(url, {{"Authorization", "Bearer t0ken"}}, &error);
        REQUIRE(error.empty());
        REQUIRE(head.find("GET /api/states/camera.x HTTP/1.1\r\n") == 0);
        REQUIRE(head.find("Host: 127.0.0.1:8123\r\n") != std::string::npos);
        REQUIRE(head.find("Authorization: Bearer t0ken\r\n") != std::string::npos);
        REQUIRE(head.find("\r\n\r\n") != std::string::npos);
    }

    SECTION("status line parsing") {
        int status = 0;
        REQUIRE(HttpClient::parse_status_line("HTTP/1.1 200 OK", &status));
        REQUIRE(status == 200);
        REQUIRE(HttpClient::parse_status_line("HTTP/1.1 404 Not Found", &status));
        REQUIRE(status == 404);
        REQUIRE_FALSE(HttpClient::parse_status_line("garbage", &status));
        REQUIRE_FALSE(HttpClient::parse_status_line("HTTP/1.1 xx", &status));
    }

    SECTION("a real loopback GET returns the peer's status and body") {
        const std::string canned =
            "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: 34\r\n"
            "Connection: close\r\n\r\n{\"entity_id\":\"camera.driveway\"}";
        LoopbackServer server(canned);
        REQUIRE(server.ok());

        HttpClient client;
        const auto url =
            HttpClient::parse_url("http://127.0.0.1:" + std::to_string(server.port()) +
                                  "/api/states/camera.driveway");
        REQUIRE(url.valid);
        const HttpResponse response = client.get(url, {{"Authorization", "Bearer stub"}});
        REQUIRE(response.ok);
        REQUIRE(response.status == 200);
        REQUIRE(response.body == "{\"entity_id\":\"camera.driveway\"}");
        REQUIRE(response.bytes_received > 0);
        REQUIRE_FALSE(response.error.size() > 0);
        // The request really reached a socket peer.
        REQUIRE(server.request().find("GET /api/states/camera.driveway HTTP/1.1") == 0);
        REQUIRE(server.request().find("Authorization: Bearer stub") != std::string::npos);
    }

    SECTION("a non-2xx status is reported, not treated as success") {
        LoopbackServer server("HTTP/1.1 401 Unauthorized\r\nContent-Length: 0\r\n\r\n");
        REQUIRE(server.ok());
        HttpClient client;
        const auto url = HttpClient::parse_url("http://127.0.0.1:" +
                                               std::to_string(server.port()) + "/api/states/x");
        const HttpResponse response = client.get(url, {});
        REQUIRE_FALSE(response.ok);
        REQUIRE(response.status == 401);
    }

    SECTION("an oversized response is truncated at the cap") {
        std::string huge = "HTTP/1.1 200 OK\r\nContent-Length: 999999\r\n\r\n";
        huge += std::string(HttpPolicy::kMaxResponseBytes + 4096, 'x');
        LoopbackServer server(huge);
        REQUIRE(server.ok());
        HttpClient client;
        const auto url = HttpClient::parse_url("http://127.0.0.1:" +
                                               std::to_string(server.port()) + "/api/states/x");
        const HttpResponse response = client.get(url, {});
        REQUIRE(response.truncated);
        REQUIRE(response.body.size() <= HttpPolicy::kMaxResponseBytes);
    }

    SECTION("a refused connection is an error, not a crash") {
        HttpClient client;
        // Port 1 on loopback is not listening.
        const auto url = HttpClient::parse_url("http://127.0.0.1:1/api/states/x");
        REQUIRE(url.valid);
        const HttpResponse response = client.get(url, {});
        REQUIRE_FALSE(response.ok);
        REQUIRE(response.status == 0);
        REQUIRE_FALSE(response.error.empty());
    }

    SECTION("an invalid url never reaches the socket layer") {
        HttpClient client;
        HttpClient::Url invalid;
        invalid.valid = false;
        invalid.error = "synthetic";
        const HttpResponse response = client.get(invalid, {});
        REQUIRE_FALSE(response.ok);
        REQUIRE(response.error.find("invalid url") != std::string::npos);
    }
}
