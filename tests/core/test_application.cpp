#include "catch2/catch.hpp"
#include "core/Application.h"
#include <thread>
#include <chrono>
#include <fstream>
#include <cstdio>
#include <atomic>

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
        std::atomic<int> result{-1};
        std::atomic<bool> thread_done{false};
        
        // Run application in a separate thread so it doesn't block
        std::thread app_thread([&]() {
            result = app.run(test_config);
            thread_done = true;
        });
        
        // Wait a bit to let it start
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        
        // Request shutdown
        app.request_shutdown();
        
        // Wait for thread to finish
        if (app_thread.joinable()) {
            app_thread.join();
        }
        
        // Verify results in main thread (not worker thread)
        REQUIRE(result == 0);
        REQUIRE(thread_done);
    }
    
    std::remove(test_config.c_str());
}
