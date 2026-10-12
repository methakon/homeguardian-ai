#include "catch2/catch.hpp"
#include "bridge/NativeHttp.h"

#include <string>
#include <string_view>

using namespace hg_native_http;

// ---------------------------------------------------------------------------
// Native HTTP transport (roadmap H-04): strict input validation, malformed and
// adversarial request handling, framing/smuggling defence, loopback/Host/Origin
// enforcement, and bounded resource behaviour. These exercise the dispatch()
// state machine directly (no socket needed) with attacker-controlled inputs.
// ---------------------------------------------------------------------------

namespace {
// NOTE: dispatch takes a std::function, which COPIES the callable. To observe
// invocations we must share state by reference (a by-value functor would be
// copied and its counter would never be seen).
struct Rec {
    int calls = 0;
    std::string last_op;
    unsigned last_cycles = 0;
};
Handler recording_handler(Rec& r) {
    return [&r](std::string_view op, unsigned cycles) {
        ++r.calls;
        r.last_op = std::string(op);
        r.last_cycles = cycles;
        return std::string("{\"ok\":true}");
    };
}

std::string req(const std::string& request_line, const std::string& headers = "Host: 127.0.0.1\r\n") {
    return request_line + "\r\n" + headers + "\r\n";
}
bool starts_with(const std::string& s, const char* p) { return s.rfind(p, 0) == 0; }
}  // namespace

TEST_CASE("NativeHttp: valid /api/health returns 200 JSON", "[nativehttp][h04]") {
    Rec h; auto handler = recording_handler(h);
    auto out = dispatch(req("GET /api/health HTTP/1.1"), "<html>", handler);
    REQUIRE(starts_with(out, "HTTP/1.1 200 OK"));
    REQUIRE(out.find("\"simulated\":true") != std::string::npos);
    REQUIRE(out.find("Content-Type: application/json") != std::string::npos);
    REQUIRE(h.calls == 0);  // health is a static answer, handler untouched
}

TEST_CASE("NativeHttp: /api/status and scenario invoke the real handler", "[nativehttp][h04]") {
    Rec h; auto handler = recording_handler(h);
    auto s = dispatch(req("GET /api/status HTTP/1.1"), "<html>", handler);
    REQUIRE(starts_with(s, "HTTP/1.1 200 OK"));
    REQUIRE(h.calls == 1);
    REQUIRE(h.last_op == "status");

    auto d = dispatch(req("GET /api/run-scenario?cycles=7 HTTP/1.1"), "<html>", handler);
    REQUIRE(starts_with(d, "HTTP/1.1 200 OK"));
    REQUIRE(h.calls == 2);
    REQUIRE(h.last_op == "scenario");
    REQUIRE(h.last_cycles == 7);
}

TEST_CASE("NativeHttp: default cycles is 5", "[nativehttp][h04]") {
    Rec h; auto handler = recording_handler(h);
    (void)dispatch(req("GET /api/run-scenario HTTP/1.1"), "<html>", handler);
    REQUIRE(h.last_cycles == 5);
}

TEST_CASE("NativeHttp: HTML routes carry security headers", "[nativehttp][h04]") {
    Rec h; auto handler = recording_handler(h);
    auto root = dispatch(req("GET / HTTP/1.1"), "<html>body</html>", handler);
    REQUIRE(starts_with(root, "HTTP/1.1 200 OK"));
    REQUIRE(root.find("Content-Security-Policy:") != std::string::npos);
    REQUIRE(root.find("X-Frame-Options: DENY") != std::string::npos);
    REQUIRE(root.find("X-Content-Type-Options: nosniff") != std::string::npos);
    REQUIRE(root.find("Referrer-Policy: no-referrer") != std::string::npos);
    REQUIRE(root.find("text/html") != std::string::npos);
    auto idx = dispatch(req("GET /index.html HTTP/1.1"), "<html>x</html>", handler);
    REQUIRE(starts_with(idx, "HTTP/1.1 200 OK"));
}

TEST_CASE("NativeHttp: non-GET methods are rejected 405", "[nativehttp][h04]") {
    Rec h; auto handler = recording_handler(h);
    for (const char* m : {"POST", "PUT", "DELETE", "PATCH", "HEAD", "OPTIONS", "TRACE"}) {
        auto out = dispatch(req(std::string(m) + " /api/status HTTP/1.1"), "<html>", handler);
        REQUIRE(starts_with(out, "HTTP/1.1 405"));
    }
    REQUIRE(h.calls == 0);  // never reached the handler
}

TEST_CASE("NativeHttp: missing or duplicate Host is rejected", "[nativehttp][h04]") {
    Rec h; auto handler = recording_handler(h);
    auto no_host = dispatch("GET /api/status HTTP/1.1\r\nX: y\r\n\r\n", "<html>", handler);
    REQUIRE(starts_with(no_host, "HTTP/1.1 400"));
    auto dup_host = dispatch(req("GET /api/status HTTP/1.1", "Host: 127.0.0.1\r\nHost: 127.0.0.1\r\n"), "<html>", handler);
    REQUIRE(starts_with(dup_host, "HTTP/1.1 400"));
    REQUIRE(h.calls == 0);
}

TEST_CASE("NativeHttp: non-loopback Host is forbidden (DNS-rebinding defence)", "[nativehttp][h04]") {
    Rec h; auto handler = recording_handler(h);
    for (const char* bad : {"evil.example", "127.0.0.1.evil.com", "192.168.1.10", "[::1]:80", "0.0.0.0", "169.254.1.1"}) {
        auto out = dispatch(req("GET /api/status HTTP/1.1", std::string("Host: ") + bad + "\r\n"), "<html>", handler);
        REQUIRE(starts_with(out, "HTTP/1.1 403"));
    }
    REQUIRE(h.calls == 0);
}

TEST_CASE("NativeHttp: loopback Host variants are accepted", "[nativehttp][h04]") {
    Rec h; auto handler = recording_handler(h);
    for (const char* ok : {"127.0.0.1", "127.0.0.1:8080", "localhost", "localhost:9"}) {
        auto out = dispatch(req("GET /api/health HTTP/1.1", std::string("Host: ") + ok + "\r\n"), "<html>", handler);
        REQUIRE(starts_with(out, "HTTP/1.1 200 OK"));
    }
}

TEST_CASE("NativeHttp: cross-site Origin is forbidden on the scenario route", "[nativehttp][h04]") {
    Rec h; auto handler = recording_handler(h);
    auto bad = dispatch(req("GET /api/run-scenario HTTP/1.1",
                            "Host: 127.0.0.1\r\nOrigin: http://evil.example\r\n"), "<html>", handler);
    REQUIRE(starts_with(bad, "HTTP/1.1 403"));
    auto nul = dispatch(req("GET /api/run-scenario HTTP/1.1",
                            "Host: 127.0.0.1\r\nOrigin: null\r\n"), "<html>", handler);
    REQUIRE(starts_with(nul, "HTTP/1.1 403"));
    REQUIRE(h.calls == 0);
    auto ok = dispatch(req("GET /api/run-scenario HTTP/1.1",
                           "Host: 127.0.0.1\r\nOrigin: http://127.0.0.1:8000\r\n"), "<html>", handler);
    REQUIRE(starts_with(ok, "HTTP/1.1 200 OK"));
    REQUIRE(h.calls == 1);
}

TEST_CASE("NativeHttp: request smuggling / framing headers are rejected", "[nativehttp][h04]") {
    Rec h; auto handler = recording_handler(h);
    for (const char* hdr : {"Content-Length: 5", "Transfer-Encoding: chunked",
                            "Authorization: Bearer x", "Expect: 100-continue"}) {
        auto out = dispatch("GET /api/status HTTP/1.1\r\nHost: 127.0.0.1\r\n" + std::string(hdr) + "\r\n\r\n",
                            "<html>", handler);
        REQUIRE(starts_with(out, "HTTP/1.1 400"));
    }
    REQUIRE(h.calls == 0);
}

TEST_CASE("NativeHttp: a request body is rejected (no body framing)", "[nativehttp][h04]") {
    Rec h; auto handler = recording_handler(h);
    // Extra bytes after the header terminator => size != header_end + 4.
    auto out = dispatch("GET /api/status HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\nEXTRA", "<html>", handler);
    REQUIRE(starts_with(out, "HTTP/1.1 400"));
    REQUIRE(h.calls == 0);
}

TEST_CASE("NativeHttp: malformed request lines are rejected", "[nativehttp][h04]") {
    Rec h; auto handler = recording_handler(h);
    REQUIRE(starts_with(dispatch("GET\r\nHost: 127.0.0.1\r\n\r\n", "<html>", handler), "HTTP/1.1 400"));
    REQUIRE(starts_with(dispatch("GET /api/status\r\nHost: 127.0.0.1\r\n\r\n", "<html>", handler), "HTTP/1.1 400"));
    REQUIRE(starts_with(dispatch(req("GET /api/status HTTP/1.0"), "<html>", handler), "HTTP/1.1 400"));
    REQUIRE(starts_with(dispatch(req("GET  HTTP/1.1"), "<html>", handler), "HTTP/1.1 400"));
    // Absolute-form request target (proxy-style) is not accepted.
    REQUIRE(starts_with(dispatch(req("GET http://evil.example/ HTTP/1.1"), "<html>", handler), "HTTP/1.1 400"));
    REQUIRE(h.calls == 0);
}

TEST_CASE("NativeHttp: header lines must be well-formed (no folding/injection)", "[nativehttp][h04]") {
    Rec h; auto handler = recording_handler(h);
    // ob-fold (leading whitespace) is rejected.
    REQUIRE(starts_with(dispatch("GET /api/status HTTP/1.1\r\n Host: 127.0.0.1\r\n\r\n", "<html>", handler), "HTTP/1.1 400"));
    // No colon.
    REQUIRE(starts_with(dispatch("GET /api/status HTTP/1.1\r\nHost 127.0.0.1\r\n\r\n", "<html>", handler), "HTTP/1.1 400"));
    // Empty header name.
    REQUIRE(starts_with(dispatch("GET /api/status HTTP/1.1\r\n: x\r\n\r\n", "<html>", handler), "HTTP/1.1 400"));
    REQUIRE(h.calls == 0);
}

TEST_CASE("NativeHttp: path traversal and encoding are rejected", "[nativehttp][h04]") {
    Rec h; auto handler = recording_handler(h);
    for (const char* p : {"/../etc/passwd", "/../../", "/a/../../b", "/%2e%2e/x", "/a%00b", "/a\\b", "/a#b"}) {
        auto out = dispatch(req(std::string("GET ") + p + " HTTP/1.1"), "<html>", handler);
        REQUIRE((starts_with(out, "HTTP/1.1 400") || starts_with(out, "HTTP/1.1 404")));
    }
    REQUIRE(h.calls == 0);
}

TEST_CASE("NativeHttp: oversized path and unknown routes are rejected", "[nativehttp][h04]") {
    Rec h; auto handler = recording_handler(h);
    std::string long_path = "/" + std::string(200, 'a');
    REQUIRE(starts_with(dispatch(req("GET " + long_path + " HTTP/1.1"), "<html>", handler), "HTTP/1.1 400"));
    REQUIRE(starts_with(dispatch(req("GET /api/nope HTTP/1.1"), "<html>", handler), "HTTP/1.1 404"));
    REQUIRE(h.calls == 0);
}

TEST_CASE("NativeHttp: cycles validation is exact", "[nativehttp][h04]") {
    Rec h; auto handler = recording_handler(h);
    REQUIRE(starts_with(dispatch(req("GET /api/run-scenario?cycles= HTTP/1.1"), "<html>", handler), "HTTP/1.1 400"));
    REQUIRE(starts_with(dispatch(req("GET /api/run-scenario?cycles=5a HTTP/1.1"), "<html>", handler), "HTTP/1.1 400"));
    REQUIRE(starts_with(dispatch(req("GET /api/run-scenario?cycles=-1 HTTP/1.1"), "<html>", handler), "HTTP/1.1 400"));
    REQUIRE(starts_with(dispatch(req("GET /api/run-scenario?cycles=999 HTTP/1.1"), "<html>", handler), "HTTP/1.1 400"));
    REQUIRE(starts_with(dispatch(req("GET /api/run-scenario?cycles=1234 HTTP/1.1"), "<html>", handler), "HTTP/1.1 400"));
    REQUIRE(starts_with(dispatch(req("GET /api/run-scenario?cycles=5&x=1 HTTP/1.1"), "<html>", handler), "HTTP/1.1 400"));
    // Boundaries accepted.
    REQUIRE(starts_with(dispatch(req("GET /api/run-scenario?cycles=0 HTTP/1.1"), "<html>", handler), "HTTP/1.1 200"));
    REQUIRE(starts_with(dispatch(req("GET /api/run-scenario?cycles=100 HTTP/1.1"), "<html>", handler), "HTTP/1.1 200"));
    REQUIRE(h.calls == 2);  // exactly the two accepted ones
}

TEST_CASE("NativeHttp: responses are non-cacheable and length-delimited", "[nativehttp][h04]") {
    Rec h; auto handler = recording_handler(h);
    auto out = dispatch(req("GET /api/health HTTP/1.1"), "<html>", handler);
    REQUIRE(out.find("Cache-Control: no-store") != std::string::npos);
    REQUIRE(out.find("Connection: close") != std::string::npos);
    REQUIRE(out.find("Content-Length:") != std::string::npos);
}

TEST_CASE("NativeHttp: loopback authority helper is strict", "[nativehttp][h04]") {
    REQUIRE(is_loopback_authority("127.0.0.1"));
    REQUIRE(is_loopback_authority("127.0.0.1:1"));
    REQUIRE(is_loopback_authority("localhost"));
    REQUIRE(is_loopback_authority("localhost:65535"));
    REQUIRE_FALSE(is_loopback_authority(""));
    REQUIRE_FALSE(is_loopback_authority("127.0.0.1:"));
    REQUIRE_FALSE(is_loopback_authority("127.0.0.1:80x"));
    REQUIRE_FALSE(is_loopback_authority("127.0.0.1:123456"));
    REQUIRE_FALSE(is_loopback_authority("example.com"));
    REQUIRE_FALSE(is_loopback_authority("127.0.0.1.evil"));
    REQUIRE_FALSE(is_loopback_authority("localhost.evil"));
}
