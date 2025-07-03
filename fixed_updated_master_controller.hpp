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

// Configuration for the Master Controller
struct MasterConfig {
    std::string master_tc_address;   // Address of the local Time Controller
    std::string local_tc_address;    // Local Time Controller address for DLT
    std::string slave_address;       // Address of the slave agent
    std::string output_dir;          // Directory for output files
    double duration;                 // Acquisition duration in seconds
    std::vector<int> channels;       // Channels to acquire
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
};

// Master Controller class
class MasterController {
public:
    MasterController(const MasterConfig& config);
    ~MasterController();
    
    bool initialize();
    void stop();
    bool run_single_file_mode(double duration, const std::vector<int>& channels);
    bool run_streaming_mode(double duration, const std::vector<int>& channels, int num_files);
    bool start_acquisition(double duration, const std::vector<int>& channels);
    
    // Thread functions
    void start_monitor_thread();
    void start_file_receiver_thread();
    
    // Helper methods
    bool check_slave_availability();
    void log_message(const std::string& message, bool verbose_only = false);
    std::string get_current_timestamp_str();
    void write_offset_report(const std::string& filename, double mean_offset, double min_offset, double max_offset, double std_dev, double relative_spread);
    void perform_synchronization_calculation(const std::string& slave_file_path);
    void apply_synchronization_correction(const std::string& master_file_path, int64_t offset);
    void save_synchronization_report(double mean_offset, int64_t min_offset, int64_t max_offset, double std_dev, size_t sample_count);
    void write_timestamps_to_txt(const std::vector<uint64_t>& timestamps, 
                                 const std::vector<int>& channels, 
                                 const std::string& filename);
    void request_partial_data_from_slave();
    void request_partial_data_from_slave_with_response();
    void request_full_data_from_slave();
    void request_text_data_from_slave();
    bool finalize_communication();
    
private:
    // Configuration
    MasterConfig config_;
    
    // ZeroMQ context and sockets
    zmq::context_t context_;
    zmq::socket_t trigger_socket_;
    zmq::socket_t status_socket_;
    zmq::socket_t file_socket_;
    zmq::socket_t status_socket_;
    zmq::socket_t command_socket_;
    zmq::socket_t sync_socket_;
    zmq::socket_t local_tc_socket_;
    
    // State variables
    std::atomic<bool> running_;
    std::atomic<bool> acquisition_active_;
    uint32_t command_sequence_;
    
    // Data storage for synchronization
    std::vector<uint64_t> latest_timestamps_;
    std::vector<int> latest_channels_;
    uint64_t master_trigger_timestamp_ns_;  // Master's trigger timestamp
    uint64_t slave_trigger_timestamp_ns_;   // Slave's trigger timestamp (received)
    int64_t calculated_offset_ns_;
    uint32_t file_counter_;
    double acquisition_duration_;
    std::chrono::steady_clock::time_point acquisition_start_time_;
    std::vector<int> active_channels_;
    
    // Thread management
    std::thread monitor_thread_;
    std::thread status_thread_;
    std::thread file_receiver_thread_;
    std::mutex mutex_;
    
    // Helper functions
    bool send_command_to_slave(json& command, json& response);
};
