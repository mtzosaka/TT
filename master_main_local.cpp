#include <iostream>
#include <string>
#include <vector>
#include <cstring>
#include <chrono>
#include <thread>
#include "master_controller.hpp"

void print_usage() {
    std::cout << "Usage: master_timestamp [OPTIONS]" << std::endl;
    std::cout << "Options:" << std::endl;
    std::cout << "  --local-tc ADDRESS   Address of local Time Controller (default: 127.0.0.1)" << std::endl;
    std::cout << "  --slave ADDRESS      Address of slave PC (default: 127.0.0.1)" << std::endl;
    std::cout << "  --trigger-port PORT  Port for trigger messages (default: 5557)" << std::endl;
    std::cout << "  --status-port PORT   Port for status updates (default: 5559)" << std::endl;
    std::cout << "  --file-port PORT     Port for file transfer (default: 5560)" << std::endl;
    std::cout << "  --command-port PORT  Port for command messages (default: 5561)" << std::endl;
    std::cout << "  --output-dir DIR     Directory for output files (default: ./outputs)" << std::endl;
    std::cout << "  --duration SECONDS   Acquisition duration in seconds (default: 0.6)" << std::endl;
    std::cout << "  --channels LIST      Comma-separated list of channels (default: 1,2,3,4)" << std::endl;
    std::cout << "  --local-mode         Run in local mode (master and slave on same machine)" << std::endl;
    std::cout << "  --help               Display this help message" << std::endl;
}

std::vector<int> parse_channels(const std::string& channels_str) {
    std::vector<int> channels;
    size_t start = 0;
    
    while (start < channels_str.size()) {
        // Skip delimiters
        if (channels_str[start] == ',' || channels_str[start] == ' ') {
            ++start;
            continue;
        }
        
        size_t end = channels_str.find_first_of(", ", start);
        if (end == std::string::npos) end = channels_str.size();
        
        int ch = std::stoi(channels_str.substr(start, end - start));
        if (ch >= 1 && ch <= 4) {
            channels.push_back(ch);
        }
        
        start = end;
    }
    
    return channels;
}

int main(int argc, char* argv[]) {
    // Default configuration
    MasterConfig config;
    double duration = 0.6;
    std::string channels_str = "1,2,3,4";
    
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
        else if (arg == "--slave" && i + 1 < argc) {
            config.slave_address = argv[++i];
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
        else if (arg == "--duration" && i + 1 < argc) {
            duration = std::stod(argv[++i]);
        }
        else if (arg == "--channels" && i + 1 < argc) {
            channels_str = argv[++i];
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
    
    // Parse channels
    std::vector<int> channels = parse_channels(channels_str);
    if (channels.empty()) {
        std::cerr << "Error: No valid channels specified" << std::endl;
        return 1;
    }
    
    // Create and initialize master controller
    MasterController controller(config);
    if (!controller.initialize()) {
        std::cerr << "Failed to initialize Master Controller" << std::endl;
        return 1;
    }
    
    // Wait for user input to start acquisition
    std::cout << "Press Enter to start acquisition..." << std::endl;
    std::cin.get();
    
    // Trigger acquisition
    if (!controller.trigger_acquisition(duration, channels)) {
        std::cerr << "Acquisition failed" << std::endl;
        return 1;
    }
    
    // Wait for user input to exit
    std::cout << "Press Enter to exit..." << std::endl;
    std::cin.get();
    
    return 0;
}
