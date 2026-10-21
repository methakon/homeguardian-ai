#include "catch2/catch.hpp"
#include "core/Logger.h"
#include <fstream>
#include <string>
#include <cstdio>
#include <filesystem>
#include <unistd.h>

using namespace homeguardian;

TEST_CASE("Logger initialization and level", "[logger]") {
    Logger::reset();
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
    const std::string test_log = "/tmp/hg_test_log_" + std::to_string(::getpid()) + ".log";
    std::filesystem::remove(test_log);
    Logger::reset();
    Logger::initialize("info", test_log);

    auto logger = Logger::get();
    logger->info("Test log message");
    logger->flush();

    std::ifstream log_file(test_log);
    REQUIRE(log_file.is_open());

    std::string line;
    bool found = false;
    while (std::getline(log_file, line)) {
        if (line.find("Test log message") != std::string::npos) {
            found = true;
            break;
        }
    }
    log_file.close();
    std::filesystem::remove(test_log);

    REQUIRE(found);
}

TEST_CASE("Logger: an unwritable working directory does not abort initialization",
          "[logger][v21]") {
    // Regression (v21): the file sink used to throw spdlog_ex out of
    // Logger::initialize(); nothing caught it, so the shipped binaries died with
    // SIGABRT (exit 134) whenever the working directory was not writable --
    // instead of starting degraded. Logging is diagnostic: it must never be
    // fatal, because consent enforcement and capture gating must still run.
    Logger::reset();

    char cwd[4096];
    REQUIRE(::getcwd(cwd, sizeof(cwd)) != nullptr);
    const std::string saved = cwd;

    // Deterministic failure on any account, including root: place a DIRECTORY
    // where the log file must be created, so opening it for append fails (EISDIR).
    const std::string dir = "/tmp/hg_logger_v21_" + std::to_string(::getpid());
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir + "/homeguardian.log");
    const std::string unwritable_log = dir + "/homeguardian.log";

    bool threw = false;
    try {
        Logger::initialize("error", unwritable_log);
    } catch (...) {
        threw = true;
    }
    auto logger = Logger::get();

    // Verify process working directory remained uncorrupted
    REQUIRE(::getcwd(cwd, sizeof(cwd)) != nullptr);
    REQUIRE(std::string(cwd) == saved);
    std::filesystem::remove_all(dir);

    REQUIRE_FALSE(threw);
    REQUIRE(logger != nullptr);
    REQUIRE_NOTHROW(logger->info("console-only logging still works"));

    Logger::reset();
    Logger::initialize("info");   // leave a usable logger for the remaining tests
}
