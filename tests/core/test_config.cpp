#include "catch2/catch.hpp"
#include "core/Config.h"
#include <nlohmann/json.hpp>
#include <fstream>
#include <cstdio>
#include <string>
#include <unistd.h>   // getpid

using namespace homeguardian;
using json = nlohmann::json;

// Suite-concurrency: two instances of this binary may run in one working directory, so a
// fixture file with a fixed name is a shared resource two processes race over - which is
// exactly what the suite-concurrency regression reported (server_port read back as 9090
// because the other instance had just rewritten the same file). Every fixture path is
// per-process.
static std::string tmp_name(const char* name) {
    return std::string("/tmp/hg_cfg_") + std::to_string(::getpid()) + "_" + name;
}

TEST_CASE("Config default values", "[config]") {
    Config config;
    REQUIRE(config.log_level == "info");
    REQUIRE(config.server_host == "127.0.0.1");
    REQUIRE(config.server_port == 8080);
    REQUIRE(config.data_dir == "/var/lib/homeguardian");
    REQUIRE(config.max_event_history == 1000);
    REQUIRE(config.correlation_window_ms == 5000);
    REQUIRE(config.alert_confidence_threshold == Catch::Detail::Approx(0.5));
}

TEST_CASE("Config validation throws", "[config]") {
    Config config;
    
    SECTION("Invalid port throws") {
        config.server_port = 0;
        REQUIRE_THROWS_AS(config.save(tmp_name("test_invalid_port.json")), std::invalid_argument);
    }
    
    SECTION("Invalid log level throws") {
        config.log_level = "invalid_level";
        REQUIRE_THROWS_AS(config.save(tmp_name("test_invalid_level.json")), std::invalid_argument);
    }

    SECTION("Invalid history throws") {
        config.max_event_history = 0;
        REQUIRE_THROWS_AS(config.save(tmp_name("test_invalid_hist.json")), std::invalid_argument);
    }

    SECTION("Invalid window throws") {
        config.correlation_window_ms = 0;
        REQUIRE_THROWS_AS(config.save(tmp_name("test_invalid_win.json")), std::invalid_argument);
    }

    SECTION("Invalid threshold throws") {
        config.alert_confidence_threshold = 1.5;
        REQUIRE_THROWS_AS(config.save(tmp_name("test_invalid_thresh.json")), std::invalid_argument);
    }
}

TEST_CASE("Config loading and saving", "[config]") {
    const std::string temp_file = tmp_name("temp_test_config.json");
    
    {
        json j;
        j["log_level"] = "debug";
        j["server_host"] = "192.168.1.100";
        j["server_port"] = 9090;
        j["data_dir"] = "/tmp/data";
        j["max_event_history"] = 500;
        j["correlation_window_ms"] = 10000;
        j["alert_confidence_threshold"] = 0.8;
        
        std::ofstream f(temp_file);
        f << j.dump();
    }
    
    Config config;
    config.load(temp_file);
    
    REQUIRE(config.log_level == "debug");
    REQUIRE(config.max_event_history == 500);
    REQUIRE(config.correlation_window_ms == 10000);
    REQUIRE(config.alert_confidence_threshold == Catch::Detail::Approx(0.8));
    
    config.server_port = 9091;
    config.save(temp_file);
    
    Config config2;
    config2.load(temp_file);
    REQUIRE(config2.server_port == 9091);
    
    std::remove(temp_file.c_str());
}
