#include "mcp/McpProtocol.h"

#include <string>

#include <nlohmann/json.hpp>

namespace homeguardian::mcp {
namespace {

using nlohmann::json;

// The id echoed back must be a JSON-RPC-legal scalar. Anything else is an invalid request
// and gets a null id, per JSON-RPC 2.0.
bool id_is_legal(const json& id) {
    return id.is_null() || id.is_string() || id.is_number();
}

json error_obj(const json& id, RpcCode code, const std::string& message) {
    return json{
        {"jsonrpc", "2.0"},
        {"id", id_is_legal(id) ? id : json(nullptr)},
        {"error", {{"code", static_cast<int>(code)}, {"message", message}}},
    };
}

json result_obj(const json& id, json result) {
    return json{{"jsonrpc", "2.0"}, {"id", id}, {"result", std::move(result)}};
}

// Tools are advertised from the registry only — there is no way to name a tool that is
// not registered, and no tool that mutates anything.
json tools_list_json(const ToolRegistry& tools) {
    json arr = json::array();
    for (const auto& t : tools.all()) {
        json schema;
        try {
            schema = json::parse(t.input_schema_json);
        } catch (const json::parse_error&) {
            schema = json{{"type", "object"}};  // never fail list because a schema is odd
        }
        arr.push_back(json{{"name", t.name}, {"description", t.description}, {"inputSchema", schema}});
    }
    return json{{"tools", arr}};
}

}  // namespace

std::string make_error_json(const std::string& id_json, RpcCode code, const std::string& message) {
    json parsed_id;
    try {
        parsed_id = json::parse(id_json);
    } catch (const json::parse_error&) {
        parsed_id = nullptr;
    }
    return error_obj(parsed_id, code, message).dump();
}

bool Dispatcher::version_supported(const std::string& v) {
    for (size_t i = 0; i < kSupportedVersionCount; ++i) {
        if (v == kSupportedVersions[i]) return true;
    }
    return false;
}

std::string Dispatcher::negotiate_version(const std::string& requested) {
    // Supported => accept the client's choice. Unsupported => answer with our preferred
    // version so the client can decide to proceed or disconnect. Never a silent coercion.
    return version_supported(requested) ? requested : std::string(kPreferredVersion);
}

std::optional<std::string> Dispatcher::handle_one(const std::string& msg_text,
                                                  SessionState& session,
                                                  const std::string& credential,
                                                  bool& started_session) const {
    json msg;
    try {
        msg = json::parse(msg_text);
    } catch (const json::parse_error&) {
        return error_obj(nullptr, RpcCode::parse_error, "Parse error").dump();
    }

    if (!msg.is_object()) {
        return error_obj(nullptr, RpcCode::invalid_request, "Invalid Request").dump();
    }

    const json id = msg.contains("id") ? msg["id"] : json(nullptr);
    const bool is_notification = !msg.contains("id");  // JSON-RPC: no id == notification

    if (!msg.contains("jsonrpc") || !msg["jsonrpc"].is_string() || msg["jsonrpc"] != "2.0") {
        if (is_notification) return std::nullopt;  // nothing to answer
        return error_obj(id, RpcCode::invalid_request, "Invalid Request").dump();
    }
    if (!msg.contains("method") || !msg["method"].is_string()) {
        // A response object (method absent, result/error present) owes no reply.
        if (!is_notification && !msg.contains("result") && !msg.contains("error")) {
            return error_obj(id, RpcCode::invalid_request, "Invalid Request").dump();
        }
        return std::nullopt;
    }

    const std::string method = msg["method"].get<std::string>();
    if (method.size() > 128) {
        if (is_notification) return std::nullopt;
        return error_obj(id, RpcCode::invalid_request, "Invalid Request").dump();
    }
    const json params = msg.contains("params") && !msg["params"].is_null() ? msg["params"] : json::object();

    // ---- methods that are legal before initialization ------------------------------
    if (method == "initialize") {
        if (!params.is_object() || !params.contains("protocolVersion") ||
            !params["protocolVersion"].is_string()) {
            if (is_notification) return std::nullopt;
            return error_obj(id, RpcCode::invalid_params, "Invalid params: protocolVersion is required")
                .dump();
        }
        const std::string requested = params["protocolVersion"].get<std::string>();
        session.negotiated_version = negotiate_version(requested);
        session.initialized = true;
        started_session = true;
        json result{
            {"protocolVersion", session.negotiated_version},
            {"capabilities", {{"tools", {{"listChanged", false}}}}},
            {"serverInfo", {{"name", kServerName}, {"version", kServerVersion}}},
        };
        if (is_notification) return std::nullopt;
        return result_obj(id, std::move(result)).dump();
    }
    if (method == "ping") {
        if (is_notification) return std::nullopt;
        return result_obj(id, json::object()).dump();
    }
    if (method == "notifications/initialized") {
        if (session.initialized) session.initialized_notification = true;
        return std::nullopt;  // a notification always
    }

    // ---- everything else requires an initialized session ---------------------------
    if (!session.initialized) {
        if (is_notification) return std::nullopt;
        return error_obj(id, RpcCode::invalid_request,
                         "Invalid Request: initialize must be called first")
            .dump();
    }

    if (method == "tools/list") {
        if (is_notification) return std::nullopt;
        return result_obj(id, tools_list_json(tools_)).dump();
    }

    if (method == "tools/call") {
        if (!params.is_object() || !params.contains("name") || !params["name"].is_string()) {
            if (is_notification) return std::nullopt;
            return error_obj(id, RpcCode::invalid_params, "Invalid params: name is required").dump();
        }
        const std::string name = params["name"].get<std::string>();
        if (name.size() > kMaxToolNameChars) {
            if (is_notification) return std::nullopt;
            return error_obj(id, RpcCode::invalid_params, "Invalid params: name is required").dump();
        }
        const Tool* tool = tools_.find(name);
        if (tool == nullptr) {
            if (is_notification) return std::nullopt;
            // Unknown tool is a bad request parameter, not a server fault.
            return error_obj(id, RpcCode::invalid_params, "Invalid params: unknown tool").dump();
        }
        std::string args_text;
        if (params.contains("arguments") && !params["arguments"].is_null()) {
            if (!params["arguments"].is_object()) {
                if (is_notification) return std::nullopt;
                return error_obj(id, RpcCode::invalid_params, "Invalid params: arguments must be an object")
                    .dump();
            }
            args_text = params["arguments"].dump();
        }
        // A tool failure is reported IN the result with isError, per MCP, so the client
        // sees which tool failed. The text is always generic and non-sensitive.
        ToolOutcome outcome = tool->handler(args_text, credential);
        json result{{"content", json::array({json{{"type", "text"}, {"text", outcome.text}}})}};
        if (!outcome.ok) result["isError"] = true;
        if (is_notification) return std::nullopt;
        return result_obj(id, std::move(result)).dump();
    }

    if (is_notification) return std::nullopt;
    return error_obj(id, RpcCode::method_not_found, "Method not found").dump();
}

DispatchOutcome Dispatcher::handle(const std::string& json_text,
                                   SessionState& session,
                                   const std::string& credential) const {
    DispatchOutcome out;

    if (json_text.empty()) {
        out.has_body = true;
        out.body = make_error_json("null", RpcCode::parse_error, "Parse error");
        out.note = "empty body";
        return out;
    }
    if (json_text.size() > kMaxJsonBytes) {
        out.has_body = true;
        out.body = make_error_json("null", RpcCode::invalid_request, "Invalid Request");
        out.note = "body over cap";
        return out;
    }

    json parsed;
    try {
        parsed = json::parse(json_text);
    } catch (const json::parse_error&) {
        out.has_body = true;
        out.body = make_error_json("null", RpcCode::parse_error, "Parse error");
        out.note = "parse error";
        return out;
    }

    // A batch: each element is handled independently; notifications contribute nothing.
    if (parsed.is_array()) {
        if (parsed.empty()) {
            out.has_body = true;
            out.body = make_error_json("null", RpcCode::invalid_request, "Invalid Request");
            out.note = "empty batch";
            return out;
        }
        json responses = json::array();
        bool started = false;
        for (const auto& element : parsed) {
            if (!element.is_object()) {
                responses.push_back(json::parse(error_obj(nullptr, RpcCode::invalid_request, "Invalid Request").dump()));
                continue;
            }
            auto r = handle_one(element.dump(), session, credential, started);
            if (r.has_value()) responses.push_back(json::parse(*r));
        }
        if (started) out.started_session = true;
        if (responses.empty()) {
            out.has_body = false;  // every element was a notification/response
            out.note = "batch with no replies";
            return out;
        }
        out.has_body = true;
        out.body = responses.dump();
        out.note = "batch";
        return out;
    }

    if (!parsed.is_object()) {
        out.has_body = true;
        out.body = make_error_json("null", RpcCode::invalid_request, "Invalid Request");
        out.note = "non-object message";
        return out;
    }

    bool started = false;
    auto single = handle_one(json_text, session, credential, started);
    out.started_session = started;
    if (!single.has_value()) {
        out.has_body = false;  // notification: accepted, nothing to return
        out.note = "notification";
        return out;
    }
    out.has_body = true;
    out.body = *single;
    return out;
}

}  // namespace homeguardian::mcp
