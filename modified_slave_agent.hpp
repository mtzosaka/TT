#ifndef SLAVE_AGENT_HPP
#define SLAVE_AGENT_HPP

#include <string>
#include <vector>
#include <thread>
#include <atomic>
#include <chrono>
#include <zmq.hpp>
#include "json.hpp"

// Configuration for the Slave Agent
struct SlaveConfig {
    // Network configuration
    std::string local_tc_address = "127.0.0.1";  // Address of local Time Controller
    std::string master_address = "127.0.0.1";    // Address of master PC
    
    // Port configuration
    int trigger_port = 5557;  // Port for trigger messages
    int status_port = 5559;   // Port for status updates
    int file_port = 5560;     // Port for file transfer
    int command_port = 5561;  // Port for command messages
    
    // Acquisition parameters
    double sub_duration = 0.2;  // Sub-acquisition duration in seconds
    
    // Output configuration
    std::string output_dir = "./outputs";  // Directory for output files
    
    // Heartbeat configuration
    int heartbeat_interval_ms = 100;  // Interval between heartbeat messages
    
    // Local mode configuration
    bool local_mode = false;  // Whether to run in local mode (master and slave on same machine)
};

// Slave Agent class
class SlaveAgent {
public:
    // Constructor
    SlaveAgent(const SlaveConfig& config);
    
    // Destructor
    ~SlaveAgent();
    
    // Initialize the agent
    bool initialize();
    
    // Stop the agent
    void stop();

private:
    // Configuration
    SlaveConfig config_;
    
    // ZeroMQ context and sockets
    zmq::context_t context_;
    zmq::socket_t trigger_socket_;
    zmq::socket_t status_socket_;
    zmq::socket_t file_socket_;
    zmq::socket_t command_socket_;
    zmq::socket_t local_tc_socket_;
    
    // Thread control
    std::atomic<bool> running_;
    std::thread trigger_thread_;
    std::thread command_thread_;
    std::thread heartbeat_thread_;
    std::thread acquisition_thread_;
    
    // Status tracking
    std::string current_state_ = "idle";
    double current_progress_ = 0.0;
    std::string current_error_;
    std::chrono::steady_clock::time_point acquisition_start_time_;
    double acquisition_duration_ = 0.0;
    
    // Sequence counters
    uint64_t status_sequence_ = 0;
    uint64_t file_sequence_ = 0;
    
    // Thread functions
    void trigger_listener_thread();
    void command_handler_thread();
    void heartbeat_thread();
    void handle_acquisition(double duration, const std::vector<int>& channels);
    
    // Helper functions
    void send_status_update();
    nlohmann::json get_status_data();
    void transfer_file(const std::string& filepath);
    std::string send_tc_command(const std::string& cmd);
    
    // Get the appropriate endpoint for a socket based on local mode
    std::string get_endpoint(const std::string& address, int port, bool bind);
};

#endif // SLAVE_AGENT_HPP
