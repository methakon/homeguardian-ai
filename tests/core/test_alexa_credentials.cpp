#include "catch2/catch.hpp"
#include "core/AlexaCredentialStore.h"
#include "core/AlexaContract.h"
#include "core/Logger.h"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

using namespace homeguardian;

// ---------------------------------------------------------------------------
// AlexaCredentialStore: hardened bearer-token verification + secure secret
// handling (security blockers B1 + B4). Simulator-only; synthetic fixtures;
// no real secrets; no new crypto dependency.
// ---------------------------------------------------------------------------

namespace {

std::string creds_tmp(const std::string& tag) {
    std::string p = "test_creds_" + tag + ".json";
    std::filesystem::remove(p);
    return p;
}
void creds_cleanup(const std::string& p) { std::filesystem::remove(p); }

// Write a credential file with the given JSON body and mode.
void write_creds(const std::string& path, const std::string& body, mode_t mode) {
    {
        std::ofstream f(path);
        f << body;
    }
    std::filesystem::permissions(path,
        static_cast<std::filesystem::perms>(mode),
        std::filesystem::perm_options::replace);
}

std::string make_creds_json(const std::string& token, long long expires_ms,
                            bool revoked, bool allow_status, bool allow_summary) {
    std::ostringstream os;
    os << "{\"" << token << "\":{\"principal\":\"tester\","
       << "\"allow_status\":" << (allow_status ? "true" : "false") << ","
       << "\"allow_alert_summary\":" << (allow_summary ? "true" : "false") << ","
       << "\"expires_at_ms\":" << expires_ms << ","
       << "\"revoked\":" << (revoked ? "true" : "false") << "}}";
    return os.str();
}

}  // namespace

// --- secure load: 0600 + owned-by-me succeeds ---
TEST_CASE("Creds: secure 0600 file owned by current user loads",
          "[creds][load]") {
    Logger::initialize("error");
    std::string p = creds_tmp("ok");
    write_creds(p, make_creds_json("tok-good", 0, false, true, true), 0600);
    AlexaCredentialStore store;
    auto res = store.load(p);
    REQUIRE(res.ok);
    REQUIRE(res.entry_count == 1);
    REQUIRE(store.size() == 1);
    creds_cleanup(p);
}

// --- fail closed: too-broad permissions rejected ---
TEST_CASE("Creds: group/other-readable file is rejected (fail closed)",
          "[creds][load][security]") {
    Logger::initialize("error");
    std::string p = creds_tmp("broad");
    write_creds(p, make_creds_json("tok-x", 0, false, true, true), 0644);
    AlexaCredentialStore store;
    auto res = store.load(p);
    REQUIRE_FALSE(res.ok);
    REQUIRE(store.size() == 0);
    REQUIRE_FALSE(store.loaded_ok());
    // Every verify fails closed on an unloaded store.
    auto ctx = store.verify("tok-x", std::chrono::system_clock::now());
    REQUIRE_FALSE(ctx.authenticated);
    creds_cleanup(p);
}

// --- fail closed: missing file ---
TEST_CASE("Creds: missing file is rejected (fail closed)",
          "[creds][load][security]") {
    Logger::initialize("error");
    AlexaCredentialStore store;
    auto res = store.load("test_creds_does_not_exist.json");
    REQUIRE_FALSE(res.ok);
    REQUIRE_FALSE(store.verify("anything", std::chrono::system_clock::now()).authenticated);
}

// --- fail closed: malformed JSON ---
TEST_CASE("Creds: malformed JSON is rejected (fail closed)",
          "[creds][load][security]") {
    Logger::initialize("error");
    std::string p = creds_tmp("bad");
    write_creds(p, "this is not json {{{", 0600);
    AlexaCredentialStore store;
    auto res = store.load(p);
    REQUIRE_FALSE(res.ok);
    REQUIRE_FALSE(store.verify("x", std::chrono::system_clock::now()).authenticated);
    creds_cleanup(p);
}

// --- fail closed: non-object top level ---
TEST_CASE("Creds: non-object credential file is rejected",
          "[creds][load][security]") {
    Logger::initialize("error");
    std::string p = creds_tmp("arr");
    write_creds(p, "[1,2,3]", 0600);
    AlexaCredentialStore store;
    REQUIRE_FALSE(store.load(p).ok);
    creds_cleanup(p);
}

// --- fail closed: empty token key ---
TEST_CASE("Creds: empty token key is rejected",
          "[creds][load][security]") {
    Logger::initialize("error");
    std::string p = creds_tmp("empty");
    write_creds(p, "{\"\":{\"principal\":\"x\"}}", 0600);
    AlexaCredentialStore store;
    REQUIRE_FALSE(store.load(p).ok);
    creds_cleanup(p);
}

// --- valid token authenticates with correct permissions ---
TEST_CASE("Creds: valid token authenticates with its permissions",
          "[creds][verify]") {
    Logger::initialize("error");
    std::string p = creds_tmp("valid");
    write_creds(p, make_creds_json("tok-v", 0, false, true, false), 0600);
    AlexaCredentialStore store;
    REQUIRE(store.load(p).ok);
    auto ctx = store.verify("tok-v", std::chrono::system_clock::now());
    REQUIRE(ctx.authenticated);
    REQUIRE(ctx.principal == "tester");
    REQUIRE(ctx.allow_status);
    REQUIRE_FALSE(ctx.allow_alert_summary);
    creds_cleanup(p);
}

// --- unknown token fails closed ---
TEST_CASE("Creds: unknown token fails closed",
          "[creds][verify][security]") {
    Logger::initialize("error");
    std::string p = creds_tmp("unknown");
    write_creds(p, make_creds_json("tok-known", 0, false, true, true), 0600);
    AlexaCredentialStore store;
    REQUIRE(store.load(p).ok);
    REQUIRE_FALSE(store.verify("tok-unknown", std::chrono::system_clock::now()).authenticated);
    REQUIRE_FALSE(store.verify("", std::chrono::system_clock::now()).authenticated);
    creds_cleanup(p);
}

// --- expired token fails closed ---
TEST_CASE("Creds: expired token fails closed",
          "[creds][verify][expiry]") {
    Logger::initialize("error");
    std::string p = creds_tmp("expired");
    long long past = std::chrono::duration_cast<std::chrono::milliseconds>(
        (std::chrono::system_clock::now() - std::chrono::hours(1)).time_since_epoch()).count();
    write_creds(p, make_creds_json("tok-exp", past, false, true, true), 0600);
    AlexaCredentialStore store;
    REQUIRE(store.load(p).ok);
    REQUIRE_FALSE(store.verify("tok-exp", std::chrono::system_clock::now()).authenticated);
    creds_cleanup(p);
}

// --- revoked token fails closed ---
TEST_CASE("Creds: revoked token fails closed",
          "[creds][verify][revocation]") {
    Logger::initialize("error");
    std::string p = creds_tmp("revoked");
    write_creds(p, make_creds_json("tok-rev", 0, /*revoked=*/true, true, true), 0600);
    AlexaCredentialStore store;
    REQUIRE(store.load(p).ok);
    REQUIRE_FALSE(store.verify("tok-rev", std::chrono::system_clock::now()).authenticated);
    creds_cleanup(p);
}

// --- runtime revocation works ---
TEST_CASE("Creds: runtime revoke disables a previously valid token",
          "[creds][revocation]") {
    Logger::initialize("error");
    std::string p = creds_tmp("runtime-rev");
    write_creds(p, make_creds_json("tok-rr", 0, false, true, true), 0600);
    AlexaCredentialStore store;
    REQUIRE(store.load(p).ok);
    REQUIRE(store.verify("tok-rr", std::chrono::system_clock::now()).authenticated);
    REQUIRE(store.revoke("tok-rr"));
    REQUIRE_FALSE(store.verify("tok-rr", std::chrono::system_clock::now()).authenticated);
    // Revoking an unknown token returns false, no crash.
    REQUIRE_FALSE(store.revoke("nope"));
    creds_cleanup(p);
}

// --- constant-time equality semantics ---
TEST_CASE("Creds: equal_bytes matches only identical same-length strings",
          "[creds][compare]") {
    Logger::initialize("error");
    // equal_bytes is private; exercise it through verify with add_for_testing.
    AlexaCredentialStore store;
    AlexaCredential c;
    c.principal = "p"; c.allow_status = true;
    store.add_for_testing("secret-token-abc", c);
    REQUIRE(store.verify("secret-token-abc", std::chrono::system_clock::now()).authenticated);
    // Same length, one byte different.
    REQUIRE_FALSE(store.verify("secret-token-abd", std::chrono::system_clock::now()).authenticated);
    // Different length.
    REQUIRE_FALSE(store.verify("secret-token-ab", std::chrono::system_clock::now()).authenticated);
    REQUIRE_FALSE(store.verify("secret-token-abcd", std::chrono::system_clock::now()).authenticated);
}

// --- no permissive fallback: unloaded store denies everything ---
TEST_CASE("Creds: unloaded store denies all (no permissive fallback)",
          "[creds][security]") {
    Logger::initialize("error");
    AlexaCredentialStore store;  // never loaded
    REQUIRE_FALSE(store.loaded_ok());
    REQUIRE(store.size() == 0);
    REQUIRE_FALSE(store.verify("anything", std::chrono::system_clock::now()).authenticated);
    REQUIRE_FALSE(store.verify("", std::chrono::system_clock::now()).authenticated);
}

// --- end-to-end: store wired into the contract, authorization still separate ---
TEST_CASE("Creds: contract + store enforce authn then authz separately",
          "[creds][contract]") {
    Logger::initialize("error");
    std::string p = creds_tmp("contract");
    // tok-status: status only. tok-none: authenticated but no permissions.
    std::ofstream f(p);
    f << "{\"tok-status\":{\"principal\":\"a\",\"allow_status\":true,"
         "\"allow_alert_summary\":false,\"expires_at_ms\":0,\"revoked\":false},"
         "\"tok-none\":{\"principal\":\"b\",\"allow_status\":false,"
         "\"allow_alert_summary\":false,\"expires_at_ms\":0,\"revoked\":false}}";
    f.close();
    std::filesystem::permissions(p, std::filesystem::perms::owner_read,
                                 std::filesystem::perm_options::replace);

    AlexaCredentialStore store;
    REQUIRE(store.load(p).ok);

    // A trivial provider that is always available.
    struct AvailProvider : IAlexaStatusProvider {
        AlexaStatusSnapshot snapshot() const override {
            AlexaStatusSnapshot s; s.available = true; s.state = "Running"; return s;
        }
    } provider;

    auto now = std::chrono::system_clock::now();
    auto mkreq = [](const std::string& tok) {
        return std::string("{\"version\":1,\"operation\":\"status\",\"credential\":\"") + tok + "\"}";
    };

    // Authenticated + permitted -> ok.
    auto r1 = AlexaContract::handle(
        std::get<AlexaRequest>(AlexaContract::parse(mkreq("tok-status"))), store, provider, now);
    REQUIRE(r1.ok);

    // Authenticated but not permitted -> forbidden.
    auto r2 = AlexaContract::handle(
        std::get<AlexaRequest>(AlexaContract::parse(mkreq("tok-none"))), store, provider, now);
    REQUIRE_FALSE(r2.ok);
    REQUIRE(r2.error == AlexaError::forbidden);

    // Unknown -> unauthorized.
    auto r3 = AlexaContract::handle(
        std::get<AlexaRequest>(AlexaContract::parse(mkreq("tok-bogus"))), store, provider, now);
    REQUIRE_FALSE(r3.ok);
    REQUIRE(r3.error == AlexaError::unauthorized);

    creds_cleanup(p);
}
