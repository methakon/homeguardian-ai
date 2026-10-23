// MCP protocol tests: JSON-RPC framing, version negotiation, tool discovery and execution,
// error taxonomy, batching, and the session/auth boundary — all against the pure protocol
// layer, with no socket involved. The transport itself is exercised end to end by
// scripts/verify_mcp.sh, which drives the real binary over loopback.
//
// What these tests are for: the protocol rules are the part a reviewer must be able to
// check without trusting the HTTP plumbing, and the security rules (unauthorized => generic
// failure, unknown tool => invalid params, no method before initialize) are the part that
// must not regress silently.

#include "catch2/catch.hpp"

#include <chrono>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/AlexaContract.h"
#include "mcp/McpHttpServer.h"
#include "mcp/McpProtocol.h"
#include "mcp/McpTools.h"

using namespace homeguardian;
using namespace homeguardian::mcp;
using nlohmann::json;

namespace {

// Deterministic verifier: exactly one good token, with configurable permissions.
class FakeVerifier : public IAlexaCredentialVerifier {
public:
    std::string good_token = "good-token";
    std::string principal = "demo-principal";
    bool allow_status = true;
    bool allow_alert_summary = true;

    AlexaAuthContext verify(const std::string& credential,
                            std::chrono::system_clock::time_point) const override {
        AlexaAuthContext ctx;
        if (credential == good_token) {
            ctx.authenticated = true;
            ctx.principal = principal;
            ctx.allow_status = allow_status;
            ctx.allow_alert_summary = allow_alert_summary;
        }
        return ctx;  // anything else: unauthenticated, fail closed
    }
};

class FakeProvider : public IAlexaStatusProvider {
public:
    AlexaStatusSnapshot s;
    FakeProvider() {
        s.available = true;
        s.state = "Running";
        s.capture_enabled = false;
        s.alert_count = 3;
        s.highest_severity = "warning";
        s.alert_ids = {"a-1", "a-2", "a-3"};
    }
    AlexaStatusSnapshot snapshot() const override { return s; }
};

struct Harness {
    FakeVerifier verifier;
    FakeProvider provider;
    ToolRegistry tools;
    Dispatcher dispatcher;
    SessionState session;
    std::string credential = "good-token";

    Harness() : tools(make_homeguardian_tools(verifier, provider)), dispatcher(tools) {}

    DispatchOutcome call(const std::string& body) { return dispatcher.handle(body, session, credential); }

    // Convenience: run initialize so later requests are in a valid session.
    void initialized() {
        (void)call(R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-11-25","capabilities":{},"clientInfo":{"name":"t","version":"1"}}})");
    }
};

json parse(const std::string& s) { return json::parse(s); }

}  // namespace

TEST_CASE("mcp: initialize negotiates a supported version and reports capabilities", "[mcp]") {
    Harness h;
    auto out = h.call(R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-11-25","capabilities":{},"clientInfo":{"name":"c","version":"1"}}})");
    REQUIRE(out.has_body);
    REQUIRE(out.started_session);
    json r = parse(out.body);
    REQUIRE(r["jsonrpc"] == "2.0");
    REQUIRE(r["id"] == 1);
    REQUIRE(r["result"]["protocolVersion"] == "2025-11-25");
    REQUIRE(r["result"]["serverInfo"]["name"] == std::string(kServerName));
    REQUIRE(r["result"]["capabilities"]["tools"]["listChanged"] == false);
    REQUIRE(h.session.initialized);
    REQUIRE(h.session.negotiated_version == "2025-11-25");
}

TEST_CASE("mcp: an unsupported version is answered with a supported one, never silently coerced", "[mcp]") {
    Harness h;
    auto out = h.call(R"({"jsonrpc":"2.0","id":"x","method":"initialize","params":{"protocolVersion":"1999-01-01"}})");
    REQUIRE(out.has_body);
    json r = parse(out.body);
    // The client is TOLD which version the server speaks so it can disconnect deliberately.
    REQUIRE(r["result"]["protocolVersion"] == std::string(kPreferredVersion));
    REQUIRE(h.session.negotiated_version == std::string(kPreferredVersion));
    REQUIRE(Dispatcher::version_supported(std::string(kPreferredVersion)));
    REQUIRE_FALSE(Dispatcher::version_supported("1999-01-01"));
}

TEST_CASE("mcp: initialize without a protocolVersion is invalid params", "[mcp]") {
    Harness h;
    auto out = h.call(R"({"jsonrpc":"2.0","id":7,"method":"initialize","params":{}})");
    REQUIRE(out.has_body);
    json r = parse(out.body);
    REQUIRE(r["error"]["code"] == static_cast<int>(RpcCode::invalid_params));
    REQUIRE_FALSE(h.session.initialized);
}

TEST_CASE("mcp: notifications produce no body", "[mcp]") {
    Harness h;
    h.initialized();
    auto out = h.call(R"({"jsonrpc":"2.0","method":"notifications/initialized"})");
    REQUIRE_FALSE(out.has_body);
    REQUIRE(h.session.initialized_notification);
}

TEST_CASE("mcp: ping answers with an empty result", "[mcp]") {
    Harness h;
    auto out = h.call(R"({"jsonrpc":"2.0","id":5,"method":"ping"})");
    REQUIRE(out.has_body);
    json r = parse(out.body);
    REQUIRE(r["result"].is_object());
    REQUIRE(r["result"].empty());
}

TEST_CASE("mcp: tools/list is refused before initialize and lists both tools after", "[mcp]") {
    Harness h;
    auto early = h.call(R"({"jsonrpc":"2.0","id":2,"method":"tools/list"})");
    REQUIRE(early.has_body);
    REQUIRE(parse(early.body)["error"]["code"] == static_cast<int>(RpcCode::invalid_request));

    h.initialized();
    auto out = h.call(R"({"jsonrpc":"2.0","id":3,"method":"tools/list"})");
    REQUIRE(out.has_body);
    json r = parse(out.body);
    REQUIRE(r["result"]["tools"].is_array());
    REQUIRE(r["result"]["tools"].size() == 3);
    std::vector<std::string> names;
    for (const auto& t : r["result"]["tools"]) {
        names.push_back(t["name"]);
        REQUIRE(t["description"].is_string());
        // Every advertised tool must carry a real JSON Schema object.
        REQUIRE(t["inputSchema"]["type"] == "object");
        REQUIRE(t["inputSchema"]["additionalProperties"] == false);
    }
    REQUIRE(names[0] == std::string(kToolStatus));
    REQUIRE(names[1] == std::string(kToolAlertSummary));
}

TEST_CASE("mcp: tools/call returns the status content through the contract", "[mcp]") {
    Harness h;
    h.initialized();
    auto out = h.call(R"({"jsonrpc":"2.0","id":9,"method":"tools/call","params":{"name":"get_homeguardian_status","arguments":{}}})");
    REQUIRE(out.has_body);
    json r = parse(out.body);
    REQUIRE(r["result"]["content"].is_array());
    REQUIRE(r["result"]["content"][0]["type"] == "text");
    json payload = json::parse(r["result"]["content"][0]["text"].get<std::string>());
    REQUIRE(payload["state"] == "Running");
    REQUIRE(payload["capture_enabled"] == false);
    REQUIRE(payload["read_only"] == true);
    REQUIRE_FALSE(r["result"].contains("isError"));
}

TEST_CASE("mcp: tools/call returns the aggregate alert summary and never raw alerts", "[mcp]") {
    Harness h;
    h.initialized();
    auto out = h.call(R"({"jsonrpc":"2.0","id":10,"method":"tools/call","params":{"name":"get_alert_summary"}})");
    REQUIRE(out.has_body);
    json r = parse(out.body);
    json payload = json::parse(r["result"]["content"][0]["text"].get<std::string>());
    REQUIRE(payload["alert_count"] == 3);
    REQUIRE(payload["highest_severity"] == "warning");
    REQUIRE(payload["alert_ids"].size() == 3);
    // The payload is an aggregate: no media, no payload bodies, no identifiers beyond ids.
    REQUIRE_FALSE(payload.contains("payload"));
    REQUIRE_FALSE(payload.contains("media"));
}

TEST_CASE("mcp: an unknown tool is invalid params, in the error channel", "[mcp]") {
    Harness h;
    h.initialized();
    auto out = h.call(R"({"jsonrpc":"2.0","id":11,"method":"tools/call","params":{"name":"delete_everything"}})");
    REQUIRE(out.has_body);
    json r = parse(out.body);
    REQUIRE(r.contains("error"));
    REQUIRE(r["error"]["code"] == static_cast<int>(RpcCode::invalid_params));
}

TEST_CASE("mcp: tools/call without a name is invalid params", "[mcp]") {
    Harness h;
    h.initialized();
    auto out = h.call(R"({"jsonrpc":"2.0","id":12,"method":"tools/call","params":{"arguments":{}}})");
    json r = parse(out.body);
    REQUIRE(r["error"]["code"] == static_cast<int>(RpcCode::invalid_params));
}

TEST_CASE("mcp: non-object arguments are invalid params", "[mcp]") {
    Harness h;
    h.initialized();
    auto out = h.call(R"({"jsonrpc":"2.0","id":13,"method":"tools/call","params":{"name":"get_homeguardian_status","arguments":[1,2,3]}})");
    json r = parse(out.body);
    REQUIRE(r["error"]["code"] == static_cast<int>(RpcCode::invalid_params));
}

TEST_CASE("mcp: a bad credential yields a generic tool error, not a diagnostic", "[mcp]") {
    Harness h;
    h.initialized();
    h.credential = "wrong-token";
    auto out = h.call(R"({"jsonrpc":"2.0","id":14,"method":"tools/call","params":{"name":"get_homeguardian_status"}})");
    REQUIRE(out.has_body);
    json r = parse(out.body);
    REQUIRE(r.contains("result"));          // the tool ran and failed...
    REQUIRE(r["result"]["isError"] == true); // ...reported in MCP's isError channel
    const std::string text = r["result"]["content"][0]["text"];
    REQUIRE(text == "the request could not be authorized");
    REQUIRE(text.find("token") == std::string::npos);
    REQUIRE(text.find("expired") == std::string::npos);
    REQUIRE(text.find("forbidden") == std::string::npos);
}

TEST_CASE("mcp: authenticated but unauthorized for this operation stays generic", "[mcp]") {
    Harness h;
    h.initialized();
    h.verifier.allow_status = false;  // valid token, no permission for this tool
    auto out = h.call(R"({"jsonrpc":"2.0","id":15,"method":"tools/call","params":{"name":"get_homeguardian_status"}})");
    json r = parse(out.body);
    REQUIRE(r["result"]["isError"] == true);
    REQUIRE(r["result"]["content"][0]["text"] == "the request could not be authorized");
}

TEST_CASE("mcp: an unavailable application fails closed through the contract", "[mcp]") {
    Harness h;
    h.initialized();
    h.provider.s.available = false;  // persistence unhealthy => contract refuses
    auto out = h.call(R"({"jsonrpc":"2.0","id":16,"method":"tools/call","params":{"name":"get_homeguardian_status"}})");
    json r = parse(out.body);
    REQUIRE(r["result"]["isError"] == true);
}

TEST_CASE("mcp: error taxonomy — parse, invalid request, unknown method", "[mcp]") {
    Harness h;
    {
        auto out = h.call("{not json");
        REQUIRE(parse(out.body)["error"]["code"] == static_cast<int>(RpcCode::parse_error));
    }
    {
        auto out = h.call(R"([1,2,3])");  // a batch of non-objects
        REQUIRE(parse(out.body).is_array());
    }
    {
        auto out = h.call("");
        REQUIRE(parse(out.body)["error"]["code"] == static_cast<int>(RpcCode::parse_error));
    }
    h.initialized();
    {
        auto out = h.call(R"({"jsonrpc":"2.0","id":20,"method":"resources/list"})");
        REQUIRE(parse(out.body)["error"]["code"] == static_cast<int>(RpcCode::method_not_found));
    }
    {
        auto out = h.call(R"({"jsonrpc":"1.0","id":21,"method":"ping"})");
        REQUIRE(parse(out.body)["error"]["code"] == static_cast<int>(RpcCode::invalid_request));
    }
}

TEST_CASE("mcp: batching returns one response per request and nothing for notifications", "[mcp]") {
    Harness h;
    h.initialized();
    auto out = h.call(R"([{"jsonrpc":"2.0","id":31,"method":"ping"},{"jsonrpc":"2.0","method":"notifications/initialized"},{"jsonrpc":"2.0","id":32,"method":"tools/list"}])");
    REQUIRE(out.has_body);
    json r = parse(out.body);
    REQUIRE(r.is_array());
    REQUIRE(r.size() == 2);
    REQUIRE(r[0]["id"] == 31);
    REQUIRE(r[1]["id"] == 32);

    auto only_notifications = h.call(R"([{"jsonrpc":"2.0","method":"notifications/initialized"}])");
    REQUIRE_FALSE(only_notifications.has_body);

    auto empty_batch = h.call("[]");
    REQUIRE(empty_batch.has_body);
    REQUIRE(parse(empty_batch.body)["error"]["code"] == static_cast<int>(RpcCode::invalid_request));
}

TEST_CASE("mcp: ids are echoed faithfully and absent ids mean notification", "[mcp]") {
    Harness h;
    h.initialized();
    auto s = h.call(R"({"jsonrpc":"2.0","id":"abc","method":"ping"})");
    REQUIRE(parse(s.body)["id"] == "abc");
    auto n = h.call(R"({"jsonrpc":"2.0","id":3.5,"method":"ping"})");
    REQUIRE(parse(n.body)["id"] == 3.5);
    // A notification with an unknown method still owes no response.
    auto quiet = h.call(R"({"jsonrpc":"2.0","method":"does/not/exist"})");
    REQUIRE_FALSE(quiet.has_body);
}

TEST_CASE("mcp: the registry cannot be extended by a client and exposes only reads", "[mcp]") {
    Harness h;
    h.initialized();
    // Every registered tool name is one of the two read tools.
    for (const auto& t : h.tools.all()) {
        REQUIRE((t.name == std::string(kToolStatus) || t.name == std::string(kToolAlertSummary) ||
               t.name == std::string(kToolDevices)));
    }
    // A plausible-sounding mutating tool does not exist.
    auto out = h.call(R"({"jsonrpc":"2.0","id":40,"method":"tools/call","params":{"name":"set_capture_enabled"}})");
    REQUIRE(parse(out.body)["error"]["code"] == static_cast<int>(RpcCode::invalid_params));
}

TEST_CASE("mcp: build refusal rules for the transport bind are unit-testable", "[mcp]") {
    REQUIRE(is_loopback_literal("127.0.0.1"));
    REQUIRE(is_loopback_literal("127.0.0.5"));
    REQUIRE(is_loopback_literal("localhost"));
    REQUIRE(is_loopback_literal("::1"));
    REQUIRE_FALSE(is_loopback_literal("0.0.0.0"));
    REQUIRE_FALSE(is_loopback_literal("10.20.30.6"));
    REQUIRE_FALSE(is_loopback_literal(""));
}
