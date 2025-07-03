#pragma once

#include <string>
#include <vector>
#include <thread>
#include <atomic>
#include <mutex>
#include <chrono>
#include <filesystem>
#include <zmq.hpp>
#include "json.hpp"
#include "working_common.hpp"
#include "streams.hpp"

namespace fs = std::filesystem;
using json = nlohmann::json;

// Forward declarations
class BufferStreamClient;

// Configuration for the Slave Agent
struct SlaveConfig {
    std::string slave_tc_address;    // Address of the local Time Controller
    std::string local_tc_address;    // Local Time Controller address for DLT
    std::string master_address;      // Address of the master controller
    std::string output_dir;          // Directory for output files
    bool streaming_mode;             // Whether to use streaming mode
    int max_files;                   // Maximum number of files in streaming mode
    double sub_duration;             // Duration of each sub-acquisition
    double sync_percentage;          // Percentage of data to use for synchronization
    bool verbose_output;             // Whether to show verbose output
    bool text_output;                // Whether to generate text output files
    int trigger_port;                // Port for trigger messages
    int status_port;                 // Port for status messages
    int file_port;                   // Port for file transfer
    int command_port;                // Port for command messages
    int sync_port;                   // Port for subscription synchronization
    int heartbeat_interval_ms;       // Interval for heartbeat messages in milliseconds
};

// Slave Agent class
class SlaveAgent {
public:
    SlaveAgent(const SlaveConfig& config);
    ~SlaveAgent();
    
    bool initialize();
    void stop();
    
    // Thread functions
    void start_trigger_listener_thread();
    void start_command_handler_thread();
    void start_heartbeat_thread();
    
    // Processing methods
    void process_trigger(uint64_t trigger_timestamp, int sequence, double duration, const std::vector<int>& channels);
    json handle_partial_data_request();
    void send_partial_data_to_master(const std::vector<uint64_t>& timestamps, const std::vector<int>& channels, int sequence);
    void send_trigger_timestamp_to_master(uint64_t slave_trigger_timestamp, int sequence);
    
    // Helper methods
    void log_message(const std::string& message, bool verbose_only = false);
    std::string get_current_timestamp_str();
    void send_file_to_master(const std::string& filename);
    void write_timestamps_to_txt(const std::vector<uint64_t>& timestamps, const std::vector<int>& channels, const std::string& filename);
    
private:
    // Configuration
    SlaveConfig config_;
    
    // ZeroMQ context and sockets
    zmq::context_t context_;
    zmq::socket_t trigger_socket_;
    zmq::socket_t status_socket_;
    zmq::socket_t file_socket_;
    zmq::socket_t command_socket_;
    zmq::socket_t sync_socket_;
    zmq::socket_t local_tc_socket_;
    
    // State variables
    std::atomic<bool> running_;
    std::atomic<bool> acquisition_active_;
    uint32_t command_sequence_;
    std::chrono::steady_clock::time_point acquisition_start_time_;
    std::vector<int> active_channels_;
    
    // Data storage
    std::vector<uint64_t> latest_timestamps_;
    std::vector<int> latest_channels_;
    std::string latest_bin_filename_;
    std::string latest_txt_filename_;
    
    // Thread management
    std::thread trigger_thread_;
    std::thread command_thread_;
    std::thread heartbeat_thread_;
    std::mutex mutex_;
};
