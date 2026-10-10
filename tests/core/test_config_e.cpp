#include "catch2/catch.hpp"
#include "core/Config.h"
#include <nlohmann/json.hpp>
#include <fstream>
#include <cstdio>

using namespace homeguardian;
using json = nlohmann::json;

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
        REQUIRE_THROWS_AS(config.save("test_invalid_policy.json"), std::invalid_argument);
        config.consent_policy_version = "1.0.0";
    }

    SECTION("Negative routine_retention_days throws") {
        config.routine_retention_days = -1;
        REQUIRE_THROWS_AS(config.save("test_invalid_routine_ret.json"), std::invalid_argument);
        config.routine_retention_days = 730;
    }

    SECTION("Excessive routine_retention_days throws") {
        config.routine_retention_days = 99999;
        REQUIRE_THROWS_AS(config.save("test_invalid_routine_ret2.json"), std::invalid_argument);
        config.routine_retention_days = 730;
    }

    SECTION("Negative profile_retention_days throws") {
        config.profile_retention_days = -5;
        REQUIRE_THROWS_AS(config.save("test_invalid_profile_ret.json"), std::invalid_argument);
        config.profile_retention_days = 1825;
    }
}

TEST_CASE("Config Phase E loading and saving round trip", "[config]") {
    const std::string temp_file = "temp_test_config_e.json";

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
    // or cloud processing.
    std::ifstream f("config/homeguardian.json");
    REQUIRE(f.is_open());
    json j;
    f >> j;
    REQUIRE(j.contains("media_capture_enabled"));
    REQUIRE(j["media_capture_enabled"].get<bool>() == false);
    REQUIRE(j.contains("cloud_processing_enabled"));
    REQUIRE(j["cloud_processing_enabled"].get<bool>() == false);
}
