#include <catch2/catch.hpp>
#include "core/Application.h"
#include <thread>
#include <chrono>
#include <fstream>
#include <cstdio>

using namespace homeguardian;

TEST_CASE("Application construction and shutdown", "[application]") {
    const std::string test_config = "test_app_config.json";
    
    // Create a dummy config for testing
    {
        Config c;
        c.save(test_config);
    }
    
    SECTION("Application runs and shuts down cleanly") {
        Application app;
        
        // Run application in a separate thread so it doesn't block
        std::thread app_thread([&app, test_config]() {
            int result = app.run(test_config);
            REQUIRE(result == 0);
        });
        
        // Wait a bit to let it start
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        
        // Request shutdown
        app.request_shutdown();
        
        // Wait for thread to finish
        if (app_thread.joinable()) {
            app_thread.join();
        }
    }
    
    std::remove(test_config.c_str());
}
