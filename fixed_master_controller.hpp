#include <string>
#include <vector>
#include <thread>
#include <atomic>
#include <chrono>
#include <zmq.hpp>
#include "json.hpp"

struct MasterConfig {
    std::string master_tc_address;  // Changed from local_tc_address
    std::string slave_address;
    std::string output_dir;
    int trigger_port = 5557;
    int status_port = 5559;
    int file_port = 5560;
    int command_port = 5561;
    int sync_port = 5562;  // Port for subscription synchronization
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
    bool run_single_file_mode(double duration, const std::vector<int>& channels);
    bool run_streaming_mode(double duration, const std::vector<int>& channels, int num_files);
    
private:
    void start_monitor_thread();
    void start_file_receiver_thread();
    bool check_slave_availability();
    void log_message(const std::string& message, bool verbose_only = false);
    std::string get_current_timestamp_str();
    void write_timestamps_to_txt(const std::vector<uint64_t>& timestamps, const std::vector<int>& channels, const std::string& filename);
    void write_offset_report(const std::string& filename, double mean_offset, double min_offset, double max_offset, double std_dev, double quality_factor);
    
    MasterConfig config_;
    zmq::context_t context_;
    zmq::socket_t trigger_socket_;
    zmq::socket_t file_socket_;
    zmq::socket_t command_socket_;
    zmq::socket_t sync_socket_;
    zmq::socket_t local_tc_socket_;
    
    std::thread monitor_thread_;
    std::thread file_receiver_thread_;
    
    std::atomic<bool> running_;
    std::atomic<bool> acquisition_active_;
    std::atomic<int> command_sequence_;
    
    uint64_t slave_trigger_timestamp_ns_;
    int64_t calculated_offset_ns_;
    int file_counter_;
    double acquisition_duration_;
    std::vector<int> active_channels_;
};

