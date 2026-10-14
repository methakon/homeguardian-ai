#include "catch2/catch.hpp"
#include "core/Config.h"
#include <nlohmann/json.hpp>
#include <fstream>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <unistd.h>   // getpid (unique temp config path)

using namespace homeguardian;
using json = nlohmann::json;

// Suite-concurrency: two instances of this binary may run in one working directory, so
// a fixture file with a fixed name is a shared resource two processes race over. Every
// fixture path is per-process; the suite-concurrency regression depends on this.
static std::string tmp_name(const char* name) {
    return std::string("/tmp/hg_cfg_e_") + std::to_string(::getpid()) + "_" + name;
}

TEST_CASE("Config Phase E safe defaults", "[config][privacy]") {
    Config config;

    // Media capture and cloud/external processing must be OFF by default.
    REQUIRE(config.media_capture_enabled == false);
    REQUIRE(config.cloud_processing_enabled == false);
    REQUIRE(config.consent_policy_version == "1.0.0");
    REQUIRE(config.routine_retention_days == 730);
    REQUIRE(config.profile_retention_days == 1825);
}

TEST_CASE("Config Phase E validation throws", "[config]") {
    Config config;

    SECTION("Empty consent_policy_version throws") {
        config.consent_policy_version = "";
        REQUIRE_THROWS_AS(config.save(tmp_name("test_invalid_policy.json")), std::invalid_argument);
        config.consent_policy_version = "1.0.0";
    }

    SECTION("Negative routine_retention_days throws") {
        config.routine_retention_days = -1;
        REQUIRE_THROWS_AS(config.save(tmp_name("test_invalid_routine_ret.json")), std::invalid_argument);
        config.routine_retention_days = 730;
    }

    SECTION("Excessive routine_retention_days throws") {
        config.routine_retention_days = 99999;
        REQUIRE_THROWS_AS(config.save(tmp_name("test_invalid_routine_ret2.json")), std::invalid_argument);
        config.routine_retention_days = 730;
    }

    SECTION("Negative profile_retention_days throws") {
        config.profile_retention_days = -5;
        REQUIRE_THROWS_AS(config.save(tmp_name("test_invalid_profile_ret.json")), std::invalid_argument);
        config.profile_retention_days = 1825;
    }
}

TEST_CASE("Config Phase E loading and saving round trip", "[config]") {
    const std::string temp_file = tmp_name("temp_test_config_e.json");

    {
        json j;
        j["media_capture_enabled"] = true;
        j["cloud_processing_enabled"] = false;
        j["consent_policy_version"] = "2.1.0";
        j["routine_retention_days"] = 365;
        j["profile_retention_days"] = 1095;

        std::ofstream f(temp_file);
        f << j.dump();
    }

    Config config;
    config.load(temp_file);

    REQUIRE(config.media_capture_enabled == true);
    REQUIRE(config.cloud_processing_enabled == false);
    REQUIRE(config.consent_policy_version == "2.1.0");
    REQUIRE(config.routine_retention_days == 365);
    REQUIRE(config.profile_retention_days == 1095);

    config.consent_policy_version = "2.2.0";
    config.save(temp_file);

    Config config2;
    config2.load(temp_file);
    REQUIRE(config2.consent_policy_version == "2.2.0");

    std::remove(temp_file.c_str());
}

TEST_CASE("Shipped default config has media capture and cloud disabled", "[config][privacy]") {
    // The repository's committed default configuration must not enable capture
    // or cloud processing. Resolve the path robustly so the test passes
    // regardless of the working directory (ctest sets the repo root, but a
    // direct binary run from an out-of-source build dir must also work).
    std::string cfg_path = "config/homeguardian.json";
    if (!std::ifstream(cfg_path).is_open()) {
        // Fall back to a path relative to the source tree.
        const char* src_dir = std::getenv("HG_SOURCE_DIR");
        if (src_dir) cfg_path = std::string(src_dir) + "/config/homeguardian.json";
    }
    std::ifstream f(cfg_path);
    REQUIRE(f.is_open());
    json j;
    f >> j;
    REQUIRE(j.contains("media_capture_enabled"));
    REQUIRE(j["media_capture_enabled"].get<bool>() == false);
    REQUIRE(j.contains("cloud_processing_enabled"));
    REQUIRE(j["cloud_processing_enabled"].get<bool>() == false);
}

TEST_CASE("Config: the alert retention window is a bounded, configurable policy",
          "[config][v21]") {
    // Both the application and the native bridge honour this value for their
    // durable alert stores. A default of 0 (keep forever) would let the demo
    // store grow without bound, so the default and the override path are pinned.
    REQUIRE(Config{}.alert_retention_days == 30);

    const std::string path = "/tmp/hg_v21_cfg_retention_" +
                             std::to_string(::getpid()) + ".json";

    {   // an explicit window round-trips
        Config c; c.alert_retention_days = 7; c.save(path);
        Config l; l.load(path);
        REQUIRE(l.alert_retention_days == 7);
    }
    {   // 0 is an explicit, valid choice: retention disabled (keep forever)
        Config c; c.alert_retention_days = 0; c.save(path);
        Config l; l.load(path);
        REQUIRE(l.alert_retention_days == 0);
    }
    {   // out-of-range values are rejected by validation, never silently kept
        Config c; c.alert_retention_days = -1;
        REQUIRE_THROWS_AS(c.save(path), std::invalid_argument);
        Config c2; c2.alert_retention_days = 36501;
        REQUIRE_THROWS_AS(c2.save(path), std::invalid_argument);
    }
    std::remove(path.c_str());
}
