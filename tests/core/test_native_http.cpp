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
    std::string last_scenario;
    unsigned last_cycles = 0;
    bool last_cycles_set = false;
};
Handler recording_handler(Rec& r) {
    return [&r](const HandlerRequest& req, int& status_out) {
        ++r.calls;
        r.last_op = std::string(req.op);
        r.last_scenario = std::string(req.scenario);
        r.last_cycles = req.cycles;
        r.last_cycles_set = req.cycles_set;
        (void)status_out;   // recording handler always succeeds
        return std::string("{\"ok\":true}");
    };
}

// A handler that mirrors the bridge's scenario contract: a well-formed but
// unsupported scenario id is rejected with 400 + a generic JSON error body.
Handler rejecting_handler(Rec& r) {
    return [&r](const HandlerRequest& req, int& status_out) {
        ++r.calls;
        r.last_op = std::string(req.op);
        r.last_scenario = std::string(req.scenario);
        if (req.op == "scenario" && req.scenario != "default") {
            status_out = 400;
            return std::string("{\"ok\":false,\"error\":\"unknown_scenario\",\"scenario_id\":\"") +
                   std::string(req.scenario) + "\"}";
        }
        return std::string("{\"ok\":true}");
    };
}

std::string req(const std::string& request_line, const std::string& headers = "Host: 127.0.0.1\r\n") {
    return request_line + "\r\n" + headers + "\r\n";
}
bool starts_with(const std::string& s, const char* p) { return s.rfind(p, 0) == 0; }

// ---- HG-SEC-01 harness -------------------------------------------------------------------
// One store for the file, a fixed clock and a fixed port, so assertions stay deterministic.
// This is NOT a compatibility overload of the production API: the store still requires the
// operator secret and a real CSRF exchange, so nothing here bypasses authorization.
constexpr uint16_t kTestPort = 18700;
const char* const kTestSecret = "test-operator-secret";
hg_native_http::SessionStore& test_sessions() {
    static hg_native_http::SessionStore store;
    store.operator_secret = kTestSecret;
    return store;
}
// Advances per call so the issuance window rolls between requests: a fixed clock made the
// rate limiter fire across unrelated assertions, which is a fixture bug, not a policy change.
int64_t test_now_ms() { static int64_t t = 1700000000000; t += 20000; return t; }

std::string dispatch(std::string_view r, std::string_view html, const Handler& h) {
    return hg_native_http::dispatch(r, html, h, test_sessions(), test_now_ms(), kTestPort);
}

// A fresh, session-bound, single-use CSRF token (the flow the dashboard performs).
std::string test_csrf() {
    const std::string body = "{}";
    const std::string session_req = "POST /api/session HTTP/1.1\r\nHost: 127.0.0.1:" +
        std::to_string(kTestPort) + "\r\nOrigin: http://127.0.0.1:" + std::to_string(kTestPort) +
        "\r\nContent-Type: application/json\r\nX-HG-Secret: " + kTestSecret +
        "\r\nContent-Length: 2\r\n\r\n" + body;
    Handler noop = [](const hg_native_http::HandlerRequest&, int& status) {
        status = 200;
        return std::string("{}");
    };
    const std::string resp = dispatch(session_req, "<html>", noop);
    const std::string key = "\"csrf\":\"";
    const auto at = resp.find(key);
    if (at == std::string::npos) return {};
    const auto start = at + key.size();
    return resp.substr(start, resp.find('"', start) - start);
}

// An authorized POST to the state route. `origin` defaults to this listener's own origin.
std::string auth_post(const std::string& query, const std::string& origin = std::string()) {
    const std::string o = origin.empty() ? "http://127.0.0.1:" + std::to_string(kTestPort) : origin;
    return "POST /api/run-scenario" + query + " HTTP/1.1\r\n" +
           "Host: 127.0.0.1:" + std::to_string(kTestPort) + "\r\n" +
           "Origin: " + o + "\r\n" +
           "Content-Type: application/json\r\n" +
           "X-HG-CSRF: " + test_csrf() + "\r\n" +
           "Content-Length: 2\r\n\r\n{}";
}
}  // namespace

TEST_CASE("NativeHttp: valid /api/health returns 200 JSON", "[nativehttp][h04]") {
    Rec h; auto handler = recording_handler(h);
    auto out = dispatch(req("GET /api/health HTTP/1.1"), "<html>", handler);
    REQUIRE(starts_with(out, "HTTP/1.1 200 OK"));
    REQUIRE(out.find("\"simulated\":true") != std::string::npos);
    REQUIRE(out.find("Content-Type: application/json") != std::string::npos);
    REQUIRE(h.calls == 0);  // health is a static answer, handler untouched
}

TEST_CASE("NativeHttp: physical camera reads require operator authorization",
          "[nativehttp][camera-auth]") {
    Rec h;
    auto handler = recording_handler(h);
    SessionStore sessions;
    sessions.operator_secret = kTestSecret;
    int frames = 0;
    CameraHandler camera = [&frames](int& status, std::string& mime) {
        ++frames;
        status = 200;
        mime = "image/jpeg";
        return std::string("fixture-frame");
    };
    for (const auto* path : {"/api/camera/frame", "/api/camera/frame?t=1",
                             "/api/camera/status", "/api/camera/status?t=1"}) {
        const auto unauth = hg_native_http::dispatch(
            req(std::string("GET ") + path + " HTTP/1.1"), "", handler,
            sessions, test_now_ms(), kTestPort, {}, camera);
        REQUIRE(starts_with(unauth, "HTTP/1.1 401"));
        REQUIRE(frames == 0);
        REQUIRE(h.calls == 0);
    }
    const auto authorized = hg_native_http::dispatch(
        req("GET /api/camera/frame?t=2 HTTP/1.1",
            std::string("Host: 127.0.0.1\r\nX-HG-Secret: ") + kTestSecret + "\r\n"),
        "", handler, sessions, test_now_ms(), kTestPort, {}, camera);
    REQUIRE(starts_with(authorized, "HTTP/1.1 200"));
    REQUIRE(frames == 1);
}

TEST_CASE("NativeHttp: a public hostname is refused unless explicitly configured",
          "[nativehttp][public-host]") {
    Rec h;
    auto handler = recording_handler(h);
    const std::string request =
        "GET /api/status HTTP/1.1\r\nHost: portal.example.test\r\n"
        "Origin: https://portal.example.test\r\n\r\n";
    REQUIRE(starts_with(dispatch(request, "", handler), "HTTP/1.1 403"));
    REQUIRE(h.calls == 0);

    setenv("HG_HTTP_PUBLIC_HOST", "portal.example.test", 1);
    setenv("HG_HTTP_PUBLIC_ORIGIN", "https://portal.example.test", 1);
    const auto allowed = dispatch(request, "", handler);
    unsetenv("HG_HTTP_PUBLIC_HOST");
    unsetenv("HG_HTTP_PUBLIC_ORIGIN");
    REQUIRE(starts_with(allowed, "HTTP/1.1 200"));
    REQUIRE(h.calls == 1);

    // Any other host and any other origin stay refused even with the setting present.
    setenv("HG_HTTP_PUBLIC_HOST", "portal.example.test", 1);
    setenv("HG_HTTP_PUBLIC_ORIGIN", "https://portal.example.test", 1);
    const auto other_host = dispatch(
        "GET /api/status HTTP/1.1\r\nHost: evil.example.test\r\nOrigin: https://portal.example.test\r\n\r\n",
        "", handler);
    const auto other_origin = dispatch(
        "GET /api/status HTTP/1.1\r\nHost: portal.example.test\r\nOrigin: https://evil.example.test\r\n\r\n",
        "", handler);
    unsetenv("HG_HTTP_PUBLIC_HOST");
    unsetenv("HG_HTTP_PUBLIC_ORIGIN");
    REQUIRE(starts_with(other_host, "HTTP/1.1 403"));
    REQUIRE(starts_with(other_origin, "HTTP/1.1 403"));
    REQUIRE(h.calls == 1);
}

TEST_CASE("NativeHttp: camera status is readable without a secret only when unconfigured",
          "[nativehttp][camera-auth]") {
    Rec h;
    auto handler = recording_handler(h);
    SessionStore sessions;
    sessions.operator_secret = kTestSecret;
    // No camera handler: nothing physical exists, so the "not configured" answer is public.
    const auto none = hg_native_http::dispatch(
        req("GET /api/camera/status HTTP/1.1"), "", handler, sessions, test_now_ms(), kTestPort);
    REQUIRE(starts_with(none, "HTTP/1.1 200"));
    REQUIRE(h.calls == 1);
    REQUIRE(h.last_op == "camera_status");

    // With a camera configured the same route requires the operator secret again.
    int frames = 0;
    CameraHandler camera = [&frames](int& status, std::string& mime) {
        ++frames;
        status = 200;
        mime = "image/jpeg";
        return std::string("frame");
    };
    const auto configured = hg_native_http::dispatch(
        req("GET /api/camera/status HTTP/1.1"), "", handler, sessions, test_now_ms(), kTestPort,
        {}, camera);
    REQUIRE(starts_with(configured, "HTTP/1.1 401"));
    REQUIRE(frames == 0);
}

TEST_CASE("NativeHttp: /api/status and scenario invoke the real handler", "[nativehttp][h04]") {
    Rec h; auto handler = recording_handler(h);
    auto s = dispatch(req("GET /api/status HTTP/1.1"), "<html>", handler);
    REQUIRE(starts_with(s, "HTTP/1.1 200 OK"));
    REQUIRE(h.calls == 1);
    REQUIRE(h.last_op == "status");

    auto d = dispatch(auth_post("?cycles=7"), "<html>", handler);
    REQUIRE(starts_with(d, "HTTP/1.1 200 OK"));
    REQUIRE(h.calls == 2);
    REQUIRE(h.last_op == "scenario");
    REQUIRE(h.last_cycles == 7);
}

TEST_CASE("NativeHttp: default cycles is 5", "[nativehttp][h04]") {
    Rec h; auto handler = recording_handler(h);
    (void)dispatch(auth_post(""), "<html>", handler);
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
    for (const char* bad : {"evil.example", "127.0.0.1.evil.com", "10.20.30.10", "[::1]:80", "0.0.0.0", "169.254.1.1"}) {
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
    auto bad = dispatch(auth_post("", "http://evil.example"), "<html>", handler);
    REQUIRE(starts_with(bad, "HTTP/1.1 403"));
    auto nul = dispatch(auth_post("", "null"), "<html>", handler);
    REQUIRE(starts_with(nul, "HTTP/1.1 403"));
    REQUIRE(h.calls == 0);
    auto ok = dispatch(auth_post("", "http://127.0.0.1:" + std::to_string(kTestPort)), "<html>", handler);
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
    REQUIRE(starts_with(dispatch(auth_post("?cycles="), "<html>", handler), "HTTP/1.1 400"));
    REQUIRE(starts_with(dispatch(auth_post("?cycles=5a"), "<html>", handler), "HTTP/1.1 400"));
    REQUIRE(starts_with(dispatch(auth_post("?cycles=-1"), "<html>", handler), "HTTP/1.1 400"));
    REQUIRE(starts_with(dispatch(auth_post("?cycles=999"), "<html>", handler), "HTTP/1.1 400"));
    REQUIRE(starts_with(dispatch(auth_post("?cycles=1234"), "<html>", handler), "HTTP/1.1 400"));
    REQUIRE(starts_with(dispatch(auth_post("?cycles=5&x=1"), "<html>", handler), "HTTP/1.1 400"));
    // Boundaries accepted.
    REQUIRE(starts_with(dispatch(auth_post("?cycles=0"), "<html>", handler), "HTTP/1.1 200"));
    REQUIRE(starts_with(dispatch(auth_post("?cycles=100"), "<html>", handler), "HTTP/1.1 200"));
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

// ---------------------------------------------------------------------------
// v17: scenario selection (allowlisted query), read-only demo routes.
// ---------------------------------------------------------------------------

TEST_CASE("NativeHttp: scenario selection is parsed and passed to the handler",
          "[nativehttp][h04][scenario]") {
    Rec h; auto handler = recording_handler(h);

    auto d = dispatch(auth_post("?scenario=unusual"), "<html>", handler);
    REQUIRE(starts_with(d, "HTTP/1.1 200 OK"));
    REQUIRE(h.calls == 1);
    REQUIRE(h.last_op == "scenario");
    REQUIRE(h.last_scenario == "unusual");
    REQUIRE_FALSE(h.last_cycles_set);   // cycles not given -> spec default applies

    auto b = dispatch(auth_post("?scenario=normal&cycles=3"), "<html>", handler);
    REQUIRE(starts_with(b, "HTTP/1.1 200 OK"));
    REQUIRE(h.last_scenario == "normal");
    REQUIRE(h.last_cycles == 3);
    REQUIRE(h.last_cycles_set);

    auto c = dispatch(auth_post("?cycles=7&scenario=consent-denied"), "<html>", handler);
    REQUIRE(starts_with(c, "HTTP/1.1 200 OK"));
    REQUIRE(h.last_scenario == "consent-denied");
    REQUIRE(h.last_cycles == 7);
}

TEST_CASE("NativeHttp: malformed scenario identifiers are rejected 400",
          "[nativehttp][h04][scenario]") {
    Rec h; auto handler = recording_handler(h);
    for (const char* p : {
             "/api/run-scenario?scenario=",
             "/api/run-scenario?scenario=NORMAL",
             "/api/run-scenario?scenario=ab!",
             "/api/run-scenario?scenario=a.b",
             "/api/run-scenario?scenario=a/b",
             "/api/run-scenario?scenario=aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"}) {
        // POST-only route: drive the same queries through the authorized path, so this still
        // asserts the SCENARIO TOKEN validator rather than the method rule.
        auto out = dispatch(auth_post(std::string(p).substr(std::string("/api/run-scenario").size())),
                            "<html>", handler);
        REQUIRE(starts_with(out, "HTTP/1.1 400"));
    }
    REQUIRE(h.calls == 0);  // never reached the handler
}

TEST_CASE("NativeHttp: duplicate and unknown query keys are rejected 400",
          "[nativehttp][h04][scenario]") {
    Rec h; auto handler = recording_handler(h);
    REQUIRE(starts_with(dispatch(auth_post("?scenario=a&scenario=b"), "<html>", handler), "HTTP/1.1 400"));
    REQUIRE(starts_with(dispatch(auth_post("?cycles=1&cycles=2"), "<html>", handler), "HTTP/1.1 400"));
    REQUIRE(starts_with(dispatch(auth_post("?foo=1"), "<html>", handler), "HTTP/1.1 400"));
    REQUIRE(starts_with(dispatch(auth_post("?scenario"), "<html>", handler), "HTTP/1.1 400"));
    REQUIRE(starts_with(dispatch(auth_post("?"), "<html>", handler), "HTTP/1.1 400"));
    REQUIRE(h.calls == 0);
}

TEST_CASE("NativeHttp: read-only demo routes reach the handler",
          "[nativehttp][h04][scenario]") {
    Rec h; auto handler = recording_handler(h);
    auto a = dispatch(req("GET /api/alerts HTTP/1.1"), "<html>", handler);
    REQUIRE(starts_with(a, "HTTP/1.1 200 OK"));
    REQUIRE(h.last_op == "alerts");
    auto c = dispatch(req("GET /api/scenarios HTTP/1.1"), "<html>", handler);
    REQUIRE(starts_with(c, "HTTP/1.1 200 OK"));
    REQUIRE(h.last_op == "scenarios");
    REQUIRE(h.calls == 2);
}

// ---------------------------------------------------------------------------
// v21: the handler owns the business status. A well-formed request naming an
// unsupported value (e.g. an unknown scenario id) must answer 400 Bad Request
// with a generic JSON error body, not 200-with-ok:false.
// ---------------------------------------------------------------------------

TEST_CASE("NativeHttp: a handler-decided 400 is returned as 400 Bad Request",
          "[nativehttp][h04][v21]") {
    Rec h; auto handler = rejecting_handler(h);
    auto out = dispatch(auth_post("?scenario=nope"), "<html>", handler);
    REQUIRE(starts_with(out, "HTTP/1.1 400 Bad Request"));
    REQUIRE(out.find("\"ok\":false") != std::string::npos);
    REQUIRE(out.find("\"error\":\"unknown_scenario\"") != std::string::npos);
    REQUIRE(out.find("\"scenario_id\":\"nope\"") != std::string::npos);
    REQUIRE(out.find("Content-Type: application/json") != std::string::npos);
    REQUIRE(h.calls == 1);
    REQUIRE(h.last_scenario == "nope");
}

TEST_CASE("NativeHttp: a supported scenario keeps the 200 status",
          "[nativehttp][h04][v21]") {
    Rec h; auto handler = rejecting_handler(h);
    auto out = dispatch(auth_post("?scenario=default"), "<html>", handler);
    REQUIRE(starts_with(out, "HTTP/1.1 200 OK"));
    REQUIRE(h.calls == 1);
}

TEST_CASE("NativeHttp: a handler status never overrides transport validation",
          "[nativehttp][h04][v21]") {
    // Malformed query / bad Host are rejected by the transport BEFORE the
    // handler runs, so a handler cannot be used to bypass input validation.
    Rec h; auto handler = rejecting_handler(h);
    auto bad_query = dispatch(auth_post("?bogus=1"), "<html>", handler);
    REQUIRE(starts_with(bad_query, "HTTP/1.1 400 Bad Request"));
    REQUIRE(bad_query.find("invalid_query") != std::string::npos);
    REQUIRE(h.calls == 0);

    auto bad_host = dispatch(req("GET /api/run-scenario?scenario=nope HTTP/1.1",
                                 "Host: evil.example\r\n"), "<html>", handler);
    REQUIRE(starts_with(bad_host, "HTTP/1.1 403 Forbidden"));
    REQUIRE(h.calls == 0);

    // A status the handler sets on a read-only route is still its own: the
    // route is not special-cased.
    Rec h2; auto handler2 = rejecting_handler(h2);
    auto alerts = dispatch(req("GET /api/alerts HTTP/1.1"), "<html>", handler2);
    REQUIRE(starts_with(alerts, "HTTP/1.1 200 OK"));
    REQUIRE(h2.last_op == "alerts");
}
