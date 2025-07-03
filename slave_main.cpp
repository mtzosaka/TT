#include <iostream>
#include <string>
#include <vector>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <exception>
#include "slave_agent.hpp"

void print_usage() {
    std::cout << "Usage: slave_timestamp [OPTIONS]" << std::endl;
    std::cout << "Options:" << std::endl;
    std::cout << "  --slave-tc ADDRESS   Address of local Time Controller (default: 127.0.0.1)" << std::endl;
    std::cout << "  --master-address ADDRESS     Address of master PC (default: 127.0.0.1)" << std::endl;
    std::cout << "  --trigger-port PORT  Port for trigger messages (default: 5557)" << std::endl;
    std::cout << "  --status-port PORT   Port for status updates (default: 5559)" << std::endl;
    std::cout << "  --file-port PORT     Port for file transfer (default: 5560)" << std::endl;
    std::cout << "  --command-port PORT  Port for command messages (default: 5561)" << std::endl;
    std::cout << "  --sync-port PORT     Port for synchronization (default: 5562)" << std::endl;
    std::cout << "  --output-dir DIR     Directory for output files (default: ./outputs)" << std::endl;
    std::cout << "  --verbose            Enable verbose output" << std::endl;
    std::cout << "  --text-output        Generate human-readable text output files" << std::endl;
    std::cout << "  --help               Display this help message" << std::endl;
}

int main(int argc, char* argv[]) {
    // Set up terminate handler to prevent crashes from uncaught exceptions
    std::set_terminate([]() {
        std::cerr << "🔴 std::terminate() called (uncaught exception or thread crash)." << std::endl;
        std::cerr << "Attempting graceful shutdown..." << std::endl;
        std::abort(); // ensure termination
    });

    try {
    // Default configuration
    SlaveConfig config;
    config.output_dir = "./outputs"; // Set explicit default
    config.trigger_port = 5557;      // Default trigger port
    config.status_port = 5559;       // Default status port
    config.file_port = 5560;         // Default file port
    config.command_port = 5561;      // Default command port
    config.sync_port = 5562;         // Default sync port - FIXED VALUE
    config.heartbeat_interval_ms = 1000; // Default heartbeat interval
    
    // Parse command line arguments
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        
        if (arg == "--help") {
            print_usage();
            return 0;
        }
        else if (arg == "--slave-tc" && i + 1 < argc) {
            config.slave_tc_address = argv[++i];
            config.local_tc_address = config.slave_tc_address;  // Set both to the same value
        }
        else if (arg == "--master-address" && i + 1 < argc) {
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
        else if (arg == "--sync-port" && i + 1 < argc) {
            config.sync_port = std::stoi(argv[++i]);
        }
        else if (arg == "--output-dir" && i + 1 < argc) {
            config.output_dir = argv[++i];
        }
        else if (arg == "--verbose") {
            config.verbose_output = true;
        }
        else if (arg == "--text-output") {
            config.text_output = true;
        }
        else {
            std::cerr << "Unknown option: " << arg << std::endl;
            print_usage();
            return 1;
        }
    }
    
    // Create output directory if it doesn't exist
    try {
        // Validate output directory path
        if (config.output_dir.empty()) {
            std::cerr << "Warning: Empty output directory specified, using './outputs' instead" << std::endl;
            config.output_dir = "./outputs";
        }
        
        // Create directory with error handling
        std::filesystem::create_directories(config.output_dir);
        std::cout << "Output directory set to: " << std::filesystem::absolute(config.output_dir) << std::endl;
    }
    catch (const std::filesystem::filesystem_error& e) {
        std::cerr << "Error creating output directory: " << e.what() << std::endl;
        std::cerr << "Using current directory instead." << std::endl;
        config.output_dir = ".";
    }
    
    // Create and initialize slave agent
    SlaveAgent agent(config);
    
    if (!agent.initialize()) {
        std::cerr << "Failed to initialize slave agent" << std::endl;
        return 1;
    }
    
    std::cout << "Slave agent initialized and waiting for trigger commands..." << std::endl;
    std::cout << "Press Ctrl+C to stop" << std::endl;
    
    // Wait for signal to stop
    while (true) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    
    // This point is never reached in normal operation
    agent.stop();
    
    return 0;
    
    } catch (const std::exception& ex) {
        std::cerr << "Unhandled exception in slave main: " << ex.what() << std::endl;
        return 1;
    } catch (...) {
        std::cerr << "Unknown exception occurred in slave main." << std::endl;
        return 1;
    }
}
