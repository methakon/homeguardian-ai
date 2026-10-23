#include "ControlChannel.h"

namespace homeguardian::camera {

bool control_kind_from_string(const std::string& name, ControlKind* out) {
    if (out == nullptr) return false;
    static const ControlKind kinds[] = {
        ControlKind::connector_register, ControlKind::heartbeat,
        ControlKind::authorization_request, ControlKind::authorization_result,
        ControlKind::consent_change, ControlKind::revocation,
        ControlKind::ice_candidate, ControlKind::stream_control,
        ControlKind::session_terminated, ControlKind::telemetry};
    for (ControlKind k : kinds) {
        if (name == control_kind_to_string(k)) { *out = k; return true; }
    }
    return false;
}

std::string ControlChannel::encode(const ControlMessage& message) const {
    // 1. The detail is bounded. A long detail is a payload, and a payload on this channel
    //    is exactly what must not happen.
    if (message.detail.size() > ControlMessage::kMaxDetailChars) {
        ++refused_;
        return {};
    }
    if (!message.device.valid()) {
        ++refused_;
        return {};
    }

    // 2. A compact, self-describing form. Deliberately hand-rolled and fixed-shape so that
    //    nothing binary - and therefore nothing frame-shaped - can be expressed.
    std::string wire;
    wire.reserve(256);
    wire += "HC1|";
    wire += control_kind_to_string(message.kind);
    wire += '|';
    wire += message.device.canonical();
    wire += '|';
    wire += std::to_string(message.seq);
    wire += '|';
    wire += std::to_string(message.at_ms);
    wire += '|';
    wire += message.detail;

    // 3. The hard ceiling, enforced by REFUSAL rather than truncation.
    if (wire.size() > max_bytes_) {
        ++refused_;
        return {};
    }
    ++accepted_;
    if (capture_ != nullptr) capture_->push_back(wire);
    return wire;
}

bool ControlChannel::decode(const std::string& wire, ControlMessage* out, std::string* reason) const {
    auto fail = [reason](const char* why) {
        if (reason != nullptr) *reason = why;
        return false;
    };
    if (out == nullptr) return fail("null output");

    if (wire.size() > max_bytes_) return fail("oversized control message");
    if (wire.rfind("HC1|", 0) != 0) return fail("not a control message");

    // Split into at most six fields; an embedded '|' inside the detail is refused rather
    // than guessed at.
    std::vector<std::string> fields;
    std::string current;
    for (size_t i = 4; i < wire.size(); ++i) {
        if (wire[i] == '|') { fields.push_back(current); current.clear(); }
        else current += wire[i];
    }
    fields.push_back(current);
    if (fields.size() != 5) return fail("malformed control message");

    ControlKind kind{};
    if (!control_kind_from_string(fields[0], &kind)) return fail("unknown control kind");

    const size_t slash = fields[1].find('/');
    if (slash == std::string::npos) return fail("malformed device identity");
    LogicalDeviceId device{fields[1].substr(0, slash), fields[1].substr(slash + 1)};
    if (!device.valid()) return fail("invalid device identity");

    uint64_t seq = 0;
    uint64_t at_ms = 0;
    for (char c : fields[2]) { if (c < '0' || c > '9') return fail("malformed sequence"); seq = seq * 10 + static_cast<uint64_t>(c - '0'); }
    for (char c : fields[3]) { if (c < '0' || c > '9') return fail("malformed timestamp"); at_ms = at_ms * 10 + static_cast<uint64_t>(c - '0'); }

    if (fields[4].size() > ControlMessage::kMaxDetailChars) return fail("detail too long");

    out->kind = kind;
    out->device = device;
    out->seq = seq;
    out->at_ms = at_ms;
    out->detail = fields[4];
    if (reason != nullptr) reason->clear();
    return true;
}

bool ControlChannel::reject_bulk(const std::string& payload, std::string* reason) const {
    auto refuse = [this, reason](const char* why) {
        ++refused_;
        ++refused_bulk_;
        if (reason != nullptr) *reason = why;
        return false;
    };
    if (payload.size() > max_bytes_) {
        return refuse("payload exceeds the control-channel ceiling: this is a media-sized "
                      "transfer and must not travel over the coordination hub");
    }
    if (payload.rfind("HC1|", 0) != 0) return refuse("payload is not a control message");
    if (reason != nullptr) reason->clear();
    return true;
}

}  // namespace homeguardian::camera
