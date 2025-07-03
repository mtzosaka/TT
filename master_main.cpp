#include <iostream>
#include <string>
#include <vector>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <exception>
#include "master_controller.hpp"

void print_usage() {
    std::cout << "Usage: master_timestamp [OPTIONS]" << std::endl;
    std::cout << "Options:" << std::endl;
    std::cout << "  --master-tc ADDRESS   Address of local Time Controller (default: 127.0.0.1)" << std::endl;
    std::cout << "  --slave ADDRESS      Address of slave PC (default: 127.0.0.1)" << std::endl;
    std::cout << "  --slave-address ADDRESS  Alternative for --slave" << std::endl;
    std::cout << "  --trigger-port PORT  Port for trigger messages (default: 5557)" << std::endl;
    std::cout << "  --status-port PORT   Port for status updates (default: 5559)" << std::endl;
    std::cout << "  --file-port PORT     Port for file transfer (default: 5560)" << std::endl;
    std::cout << "  --command-port PORT  Port for command messages (default: 5561)" << std::endl;
    std::cout << "  --sync-port PORT     Port for synchronization (default: 5562)" << std::endl;
    std::cout << "  --output-dir DIR     Directory for output files (default: ./outputs)" << std::endl;
    std::cout << "  --duration SECONDS   Acquisition duration in seconds (default: 0.6)" << std::endl;
    std::cout << "  --channels LIST      Comma-separated list of channels (default: 1,2,3,4)" << std::endl;
    std::cout << "  --verbose            Enable verbose output" << std::endl;
    std::cout << "  --text-output        Generate human-readable text output files" << std::endl;
    std::cout << "  --help               Display this help message" << std::endl;
}

std::vector<int> parse_channels(const std::string& channels_str) {
    std::vector<int> channels;
    size_t start = 0;
    size_t end = 0;
    
    while ((end = channels_str.find(',', start)) != std::string::npos) {
        channels.push_back(std::stoi(channels_str.substr(start, end - start)));
        start = end + 1;
    }
    
    if (start < channels_str.size()) {
        channels.push_back(std::stoi(channels_str.substr(start)));
    }
    
    return channels;
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
    MasterConfig config;
    config.master_tc_address = "127.0.0.1";  // Default master TC address
    config.local_tc_address = "127.0.0.1";   // Default local TC address
    config.slave_address = "127.0.0.1";      // Default slave address
    config.output_dir = "./outputs"; // Set explicit default
    config.trigger_port = 5557;      // Default trigger port
    config.status_port = 5559;       // Default status port
    config.file_port = 5560;         // Default file port
    config.command_port = 5561;      // Default command port
    config.sync_port = 5562;         // Default sync port - FIXED VALUE
    double duration = 0.6;
    std::string channels_str = "1,2,3,4";
    
    // Parse command line arguments
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        
        if (arg == "--help") {
            print_usage();
            return 0;
        }
        else if (arg == "--master-tc" && i + 1 < argc) {
            config.master_tc_address = argv[++i];
            config.local_tc_address = config.master_tc_address;  // Set both to the same value
        }
        else if ((arg == "--slave" || arg == "--slave-address") && i + 1 < argc) {
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
        else if (arg == "--sync-port" && i + 1 < argc) {
            config.sync_port = std::stoi(argv[++i]);
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
    
    // Parse channels
    std::vector<int> channels = parse_channels(channels_str);
    
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
    
    // Create and initialize master controller
    MasterController controller(config);
    
    if (!controller.initialize()) {
        std::cerr << "Failed to initialize master controller" << std::endl;
        return 1;
    }
    
    // Trigger acquisition
    std::cout << "Triggering synchronized acquisition for " << duration << " seconds..." << std::endl;
    if (!controller.start_acquisition(duration, channels)) {
        std::cerr << "Failed to trigger acquisition" << std::endl;
        return 1;
    }
    
    // Wait for file transfer to complete - extended time for trigger sync + partial data
    std::cout << "Waiting for file transfer and synchronization to complete..." << std::endl;
    std::this_thread::sleep_for(std::chrono::seconds(30)); // Extended wait time for trigger sync + partial data
    
    // Stop controller
    controller.stop();
    
    std::cout << "Master timestamp completed successfully." << std::endl;
    return 0;
    
    } catch (const std::exception& ex) {
        std::cerr << "Unhandled exception in master main: " << ex.what() << std::endl;
        return 1;
    } catch (...) {
        std::cerr << "Unknown exception occurred in master main." << std::endl;
        return 1;
    }
}
