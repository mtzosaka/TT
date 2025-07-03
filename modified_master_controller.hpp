#ifndef MASTER_CONTROLLER_HPP
#define MASTER_CONTROLLER_HPP

#include <string>
#include <vector>
#include <thread>
#include <atomic>
#include <zmq.hpp>
#include "json.hpp"

// Configuration for the Master Controller
struct MasterConfig {
    // Network configuration
    std::string local_tc_address = "127.0.0.1";  // Address of local Time Controller
    std::string slave_address = "127.0.0.1";     // Address of slave PC
    
    // Port configuration
    int trigger_port = 5557;  // Port for trigger messages
    int status_port = 5559;   // Port for status updates
    int file_port = 5560;     // Port for file transfer
    int command_port = 5561;  // Port for command messages
    
    // Acquisition parameters
    double sub_duration = 0.2;  // Sub-acquisition duration in seconds
    
    // Output configuration
    std::string output_dir = "./outputs";  // Directory for output files
    
    // Local mode configuration
    bool local_mode = false;  // Whether to run in local mode (master and slave on same machine)
};

// Master Controller class
class MasterController {
public:
    // Constructor
    MasterController(const MasterConfig& config);
    
    // Destructor
    ~MasterController();
    
    // Initialize the controller
    bool initialize();
    
    // Stop the controller
    void stop();
    
    // Trigger a synchronized acquisition on both master and slave
    bool trigger_acquisition(double duration, const std::vector<int>& channels);
    
    // Send a command to the slave
    bool send_command(const std::string& command, const nlohmann::json& params);

private:
    // Configuration
    MasterConfig config_;
    
    // ZeroMQ context and sockets
    zmq::context_t context_;
    zmq::socket_t trigger_socket_;
    zmq::socket_t status_socket_;
    zmq::socket_t file_socket_;
    zmq::socket_t command_socket_;
    zmq::socket_t local_tc_socket_;
    
    // Thread control
    std::atomic<bool> running_;
    std::thread status_thread_;
    std::thread file_thread_;
    
    // Sequence counters
    uint64_t trigger_sequence_;
    uint64_t command_sequence_;
    
    // Thread functions
    void status_monitor_thread();
    void file_receiver_thread();
    
    // Helper functions
    std::string send_tc_command(const std::string& cmd);
    
    // Get the appropriate endpoint for a socket based on local mode
    std::string get_endpoint(const std::string& address, int port, bool bind);
};

#endif // MASTER_CONTROLLER_HPP
