#include "catch2/catch.hpp"
#include "core/Logger.h"
#include <fstream>
#include <string>
#include <cstdio>

using namespace homeguardian;

TEST_CASE("Logger initialization and level", "[logger]") {
    // Initializing logger multiple times should be safe (singleton-like pattern with shared_ptr)
    Logger::initialize("debug");
    
    auto logger = Logger::get();
    REQUIRE(logger != nullptr);
    REQUIRE(logger->level() == spdlog::level::debug);
}

TEST_CASE("Logger get throws when not initialized", "[logger]") {
    Logger::reset();
    REQUIRE_THROWS_AS(Logger::get(), std::runtime_error);
    // Re-initialize for other tests
    Logger::initialize("info");
}

TEST_CASE("Logger file output", "[logger]") {
    // Since logger is initialized to homeguardian.log
    auto logger = Logger::get();
    logger->info("Test log message");
    logger->flush();
    
    std::ifstream log_file("homeguardian.log");
    REQUIRE(log_file.is_open());
    
    std::string line;
    bool found = false;
    while (std::getline(log_file, line)) {
        if (line.find("Test log message") != std::string::npos) {
            found = true;
            break;
        }
    }
    
    REQUIRE(found);
}
