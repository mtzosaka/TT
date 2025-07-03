#pragma once

#include <string>
#include <vector>
#include <thread>
#include <atomic>
#include <chrono>
#include <zmq.hpp>
#include "json.hpp"

struct SlaveConfig {
    std::string local_tc_address;
    std::string master_address;
    std::string output_dir;
    int trigger_port = 5557;
    int status_port = 5559;
    int file_port = 5560;
    int command_port = 5561;
    int sync_port = 5562;
    int heartbeat_interval_ms = 100;
    double sub_duration = 0.2;
    bool streaming_mode = false;
    int max_files = 10;
    double sync_percentage = 0.1;  // Use first 10% of data for sync calculation
    bool verbose_output = false;   // New option to control verbosity
    bool text_output = false;      // New option to enable text output format
};

class SlaveAgent {
public:
    SlaveAgent(const SlaveConfig& config);
    ~SlaveAgent();
    
    bool initialize();
    void stop();
    
private:
    void trigger_listener_thread();
    void command_handler_thread();
    void heartbeat_thread();
    void handle_acquisition(double duration, const std::vector<int>& channels);
    void send_status_update();
    nlohmann::json get_status_data();
    std::string send_tc_command(const std::string& cmd);
    void transfer_file(const std::string& filepath);
    void transfer_partial_file(const std::string& filepath, double percentage);
    void write_timestamps_to_bin(const std::vector<uint64_t>& timestamps, const std::string& filename);
    void write_timestamps_to_txt(const std::vector<uint64_t>& timestamps, const std::vector<int>& channels, const std::string& filename);
    std::vector<uint64_t> read_timestamps_from_bin(const std::string& filename, double percentage);
    std::string generate_filename(int file_index, bool text_format = false);
    void log_message(const std::string& message, bool always_show = false);
    
    SlaveConfig config_;
    zmq::context_t context_;
    zmq::socket_t trigger_socket_{context_, zmq::socket_type::sub};
    zmq::socket_t status_socket_{context_, zmq::socket_type::push};
    zmq::socket_t file_socket_{context_, zmq::socket_type::push};
    zmq::socket_t command_socket_{context_, zmq::socket_type::rep};
    zmq::socket_t local_tc_socket_{context_, zmq::socket_type::req};
    zmq::socket_t sync_socket_{context_, zmq::socket_type::push};
    
    std::thread trigger_thread_;
    std::thread command_thread_;
    std::thread heartbeat_thread_;
    std::thread acquisition_thread_;
    
    std::atomic<bool> running_;
    std::string current_state_;
    std::string current_error_;
    double current_progress_;
    
    uint64_t trigger_timestamp_ns_;
    
    std::chrono::steady_clock::time_point acquisition_start_time_;
    double acquisition_duration_;
    
    int status_sequence_;
    int file_sequence_;
    int file_counter_;
    std::vector<int> active_channels_;
};
