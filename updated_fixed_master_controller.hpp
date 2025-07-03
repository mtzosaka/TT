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

namespace fs = std::filesystem;

// Configuration for the Master Controller
struct MasterConfig {
    std::string local_tc_address;    // Address of the local Time Controller
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
    bool start_acquisition(double duration, const std::vector<int>& channels);
    void calculate_sync(const std::string& master_file, const std::string& slave_file);
    void write_offset_report(const std::string& filename, double min_offset, double max_offset, 
                           double avg_offset, double std_dev, double quality);
    void write_corrected_master_file(const std::string& master_file, double offset);
    
    // Helper methods
    void log_message(const std::string& message, bool always_show = false);
    std::string send_tc_command(const std::string& cmd);
    std::string generate_filename(bool is_master, int file_index = 0, bool text_format = false);
    std::string get_current_timestamp_str();
    void write_timestamps_to_bin(const std::vector<uint64_t>& timestamps, const std::string& filename);
    void write_timestamps_to_txt(const std::vector<uint64_t>& timestamps, const std::vector<int>& channels, const std::string& filename);
    void convert_text_to_binary(const std::string& text_filename, const std::string& bin_filename);
    
private:
    // Configuration
    MasterConfig config_;
    
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
    std::string current_state_;
    std::string current_error_;
    double current_progress_;
    uint64_t trigger_timestamp_ns_;
    uint32_t command_sequence_;
    std::chrono::steady_clock::time_point acquisition_start_time_;
    std::vector<int> active_channels_;
    
    // Thread management
    std::thread status_thread_;
    std::thread file_thread_;
    std::thread acquisition_thread_;
    std::mutex mutex_;
    
    // Thread functions
    void status_monitor_thread();
    void file_receiver_thread();
    
    // Helper functions
    bool send_command_to_slave(const std::string& command, json& response);
    bool wait_for_slave_ready();
    bool send_trigger_to_slave(double duration, const std::vector<int>& channels);
    bool receive_file_from_slave(std::string& filename);
};
