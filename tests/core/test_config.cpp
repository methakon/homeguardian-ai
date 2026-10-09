#include <catch2/catch.hpp>
#include "core/Config.h"
#include <nlohmann/json.hpp>
#include <fstream>
#include <cstdio>

using namespace homeguardian;
using json = nlohmann::json;

TEST_CASE("Config default values", "[config]") {
    Config config;
    REQUIRE(config.log_level == "info");
    REQUIRE(config.server_host == "127.0.0.1");
    REQUIRE(config.server_port == 8080);
    REQUIRE(config.data_dir == "/var/lib/homeguardian");
}

TEST_CASE("Config validation throws", "[config]") {
    Config config;
    
    SECTION("Invalid port throws") {
        config.server_port = 0; // invalid port
        REQUIRE_THROWS_AS(config.save("test_invalid_port.json"), std::invalid_argument);
    }
    
    SECTION("Invalid log level throws") {
        config.log_level = "invalid_level";
        REQUIRE_THROWS_AS(config.save("test_invalid_level.json"), std::invalid_argument);
    }
}

TEST_CASE("Config loading and saving", "[config]") {
    const std::string temp_file = "temp_test_config.json";
    
    // Create temp file
    {
        json j;
        j["log_level"] = "debug";
        j["server_host"] = "192.168.1.100";
        j["server_port"] = 9090;
        j["data_dir"] = "/tmp/data";
        
        std::ofstream f(temp_file);
        f << j.dump();
    }
    
    Config config;
    config.load(temp_file);
    
    REQUIRE(config.log_level == "debug");
    REQUIRE(config.server_host == "192.168.1.100");
    REQUIRE(config.server_port == 9090);
    REQUIRE(config.data_dir == "/tmp/data");
    
    config.server_port = 9091;
    config.save(temp_file);
    
    Config config2;
    config2.load(temp_file);
    REQUIRE(config2.server_port == 9091);
    
    std::remove(temp_file.c_str());
}
