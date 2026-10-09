#include "core/Application.h"
#include <iostream>

int main(int argc, char* argv[]) {
    std::string config_path = "config/homeguardian.json";
    
    if (argc > 1) {
        config_path = argv[1];
    }
    
    try {
        homeguardian::Application app;
        return app.run(config_path);
    } catch (const std::exception& e) {
        std::cerr << "Fatal error: " << e.what() << std::endl;
        return 1;
    }
    
    return 0;
}
