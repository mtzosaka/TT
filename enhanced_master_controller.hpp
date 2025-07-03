#pragma once

#include <string>
#include <vector>
#include <thread>
#include <atomic>
#include <chrono>
#include <zmq.hpp>
#include "json.hpp"

struct MasterConfig {
    std::string local_tc_address;
    std::string slave_address;
    std::string output_dir;
    int trigger_port = 5557;
    int status_port = 5559;
    int file_port = 5560;
    int command_port = 5561;
    int sync_port = 5562;
    double sub_duration = 0.2;
    bool streaming_mode = false;
    int max_files = 10;
    double sync_percentage = 0.1;  // Use first 10% of data for sync calculation
    bool verbose_output = false;   // New option to control verbosity
    bool text_output = false;      // New option to enable text output format
};

class MasterController {
public:
    MasterController(const MasterConfig& config);
    ~MasterController();
    
    bool initialize();
    void stop();
    bool start_acquisition(double duration, const std::vector<int>& channels);
    
private:
    void status_monitor_thread();
    void file_receiver_thread();
    std::string send_tc_command(const std::string& cmd);
    bool send_command_to_slave(const std::string& command, nlohmann::json& response);
    void calculate_sync(const std::string& master_file, const std::string& slave_file);
    void write_timestamps_to_bin(const std::vector<uint64_t>& timestamps, const std::string& filename);
    void write_timestamps_to_txt(const std::vector<uint64_t>& timestamps, const std::vector<int>& channels, const std::string& filename);
    std::vector<uint64_t> read_timestamps_from_bin(const std::string& filename, double percentage);
    std::string generate_filename(bool is_master, int file_index, bool text_format = false);
    void log_message(const std::string& message, bool always_show = false);
    
    MasterConfig config_;
    zmq::context_t context_;
    zmq::socket_t trigger_socket_{context_, zmq::socket_type::pub};
    zmq::socket_t status_socket_{context_, zmq::socket_type::pull};
    zmq::socket_t file_socket_{context_, zmq::socket_type::pull};
    zmq::socket_t command_socket_{context_, zmq::socket_type::req};
    zmq::socket_t local_tc_socket_{context_, zmq::socket_type::req};
    zmq::socket_t sync_socket_{context_, zmq::socket_type::pull};
    
    std::thread status_thread_;
    std::thread file_thread_;
    
    std::atomic<bool> running_;
    std::atomic<bool> acquisition_active_;
    std::string current_state_;
    std::string current_error_;
    double current_progress_;
    
    uint64_t trigger_timestamp_ns_;
    uint64_t slave_trigger_timestamp_ns_;
    double calculated_offset_ns_;
    
    std::chrono::steady_clock::time_point acquisition_start_time_;
    double acquisition_duration_;
    
    int command_sequence_;
    int file_counter_;
    std::vector<int> active_channels_;
};
