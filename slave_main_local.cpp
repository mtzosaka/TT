#include <iostream>
#include <string>
#include <vector>
#include <cstring>
#include <chrono>
#include <thread>
#include "slave_agent.hpp"

void print_usage() {
    std::cout << "Usage: slave_timestamp [OPTIONS]" << std::endl;
    std::cout << "Options:" << std::endl;
    std::cout << "  --local-tc ADDRESS   Address of local Time Controller (default: 127.0.0.1)" << std::endl;
    std::cout << "  --master ADDRESS     Address of master PC (default: 127.0.0.1)" << std::endl;
    std::cout << "  --trigger-port PORT  Port for trigger messages (default: 5557)" << std::endl;
    std::cout << "  --status-port PORT   Port for status updates (default: 5559)" << std::endl;
    std::cout << "  --file-port PORT     Port for file transfer (default: 5560)" << std::endl;
    std::cout << "  --command-port PORT  Port for command messages (default: 5561)" << std::endl;
    std::cout << "  --output-dir DIR     Directory for output files (default: ./outputs)" << std::endl;
    std::cout << "  --local-mode         Run in local mode (master and slave on same machine)" << std::endl;
    std::cout << "  --help               Display this help message" << std::endl;
}

int main(int argc, char* argv[]) {
    // Default configuration
    SlaveConfig config;
    
    // Parse command line arguments
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        
        if (arg == "--help") {
            print_usage();
            return 0;
        }
        else if (arg == "--local-tc" && i + 1 < argc) {
            config.local_tc_address = argv[++i];
        }
        else if (arg == "--master" && i + 1 < argc) {
            config.master_address = argv[++i];
        }
        else if (arg == "--trigger-port" && i + 1 < argc) {
            config.trigger_port = std::stoi(argv[++i]);
        }
        else if (arg == "--status-port" && i + 1 < argc) {
            config.status_port = std::stoi(argv[++i]);
        }
        else if (arg == "--file-port" && i + 1 < argc) {
            config.file_port = std::stoi(argv[++i]);
        }
        else if (arg == "--command-port" && i + 1 < argc) {
            config.command_port = std::stoi(argv[++i]);
        }
        else if (arg == "--output-dir" && i + 1 < argc) {
            config.output_dir = argv[++i];
        }
        else if (arg == "--local-mode") {
            config.local_mode = true;
        }
        else {
            std::cerr << "Unknown option: " << arg << std::endl;
            print_usage();
            return 1;
        }
    }
    
    // Create and initialize slave agent
    SlaveAgent agent(config);
    if (!agent.initialize()) {
        std::cerr << "Failed to initialize Slave Agent" << std::endl;
        return 1;
    }
    
    std::cout << "Slave agent initialized and waiting for trigger commands..." << std::endl;
    std::cout << "Press Ctrl+C to stop" << std::endl;
    
    // Keep the main thread alive
    while (true) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    
    return 0;
}
