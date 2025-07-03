#include "master_controller.hpp"
#include <iostream>
#include <fstream>
#include <chrono>
#include <thread>
#include <filesystem>
#include <cstring>
#include <zmq.hpp>
#include <atomic>
#include "json.hpp"

using json = nlohmann::json;
namespace fs = std::filesystem;

MasterController::MasterController(const MasterConfig& config) 
    : config_(config), 
      context_(1), 
      running_(false),
      trigger_sequence_(0),
      command_sequence_(0) {
    
    std::cout << "Initializing Master Controller..." << std::endl;
    std::cout << "Local Time Controller: " << config_.local_tc_address << std::endl;
    std::cout << "Remote Slave: " << config_.slave_address << std::endl;
    if (config_.local_mode) {
        std::cout << "Running in LOCAL MODE (master and slave on same machine)" << std::endl;
    }
}

MasterController::~MasterController() {
    stop();
}

std::string MasterController::get_endpoint(const std::string& address, int port, bool bind) {
    if (config_.local_mode) {
        // Use IPC for local mode
        std::string endpoint = "ipc:///tmp/timestamp_" + std::to_string(port);
        return endpoint;
    } else {
        // Use TCP for distributed mode
        if (bind) {
            return "tcp://*:" + std::to_string(port);
        } else {
            return "tcp://" + address + ":" + std::to_string(port);
        }
    }
}

bool MasterController::initialize() {
    try {
        // Initialize ZeroMQ sockets
        std::cout << "Setting up communication channels..." << std::endl;
        
        // Trigger publisher
        trigger_socket_ = zmq::socket_t(context_, zmq::socket_type::pub);
        trigger_socket_.bind(get_endpoint("", config_.trigger_port, true));
        
        // Status receiver
        status_socket_ = zmq::socket_t(context_, zmq::socket_type::pull);
        status_socket_.bind(get_endpoint("", config_.status_port, true));
        
        // File receiver
        file_socket_ = zmq::socket_t(context_, zmq::socket_type::pull);
        file_socket_.bind(get_endpoint("", config_.file_port, true));
        
        // Command socket (for sending commands to slave)
        command_socket_ = zmq::socket_t(context_, zmq::socket_type::req);
        command_socket_.connect(get_endpoint(config_.slave_address, config_.command_port, false));
        
        // Set socket options for low latency
        int linger = 0;
        trigger_socket_.set(zmq::sockopt::linger, linger);
        status_socket_.set(zmq::sockopt::linger, linger);
        file_socket_.set(zmq::sockopt::linger, linger);
        command_socket_.set(zmq::sockopt::linger, linger);
        
        // Set high water mark for file transfer
        int hwm = 1000;
        file_socket_.set(zmq::sockopt::rcvhwm, hwm);
        
        // Set timeouts
        command_socket_.set(zmq::sockopt::rcvtimeo, 5000);
        
        // Create output directory if it doesn't exist
        if (!fs::exists(config_.output_dir)) {
            fs::create_directories(config_.output_dir);
        }
        
        // Connect to local Time Controller
        std::cout << "Connecting to local Time Controller..." << std::endl;
        local_tc_socket_ = zmq::socket_t(context_, zmq::socket_type::req);
        local_tc_socket_.connect("tcp://" + config_.local_tc_address + ":5555");
        
        // Test connection to local Time Controller
        zmq::message_t request(5);
        memcpy(request.data(), "*IDN?", 5);
        local_tc_socket_.send(request, zmq::send_flags::none);
        
        zmq::message_t reply;
        auto result = local_tc_socket_.recv(reply, zmq::recv_flags::none);
        if (!result.has_value()) {
            std::cerr << "Failed to connect to local Time Controller" << std::endl;
            return false;
        }
        
        std::string idn(static_cast<char*>(reply.data()), reply.size());
        std::cout << "Local Time Controller identified: " << idn << std::endl;
        
        // Check if slave is available
        std::cout << "Checking slave availability..." << std::endl;
        if (!send_command("status", {})) {
            std::cerr << "Warning: Slave not responding. Will continue to try during operation." << std::endl;
        } else {
            std::cout << "Slave is available and responding." << std::endl;
        }
        
        running_ = true;
        
        // Start monitoring threads
        status_thread_ = std::thread(&MasterController::status_monitor_thread, this);
        file_thread_ = std::thread(&MasterController::file_receiver_thread, this);
        
        std::cout << "Master Controller initialized successfully." << std::endl;
        return true;
        
    } catch (const zmq::error_t& e) {
        std::cerr << "ZeroMQ error during initialization: " << e.what() << std::endl;
        return false;
    } catch (const std::exception& e) {
        std::cerr << "Error during initialization: " << e.what() << std::endl;
        return false;
    }
}

void MasterController::stop() {
    if (running_) {
        running_ = false;
        
        // Join threads
        if (status_thread_.joinable()) {
            status_thread_.join();
        }
        if (file_thread_.joinable()) {
            file_thread_.join();
        }
        
        // Close sockets
        trigger_socket_.close();
        status_socket_.close();
        file_socket_.close();
        command_socket_.close();
        local_tc_socket_.close();
        
        std::cout << "Master Controller stopped." << std::endl;
    }
}

bool MasterController::trigger_acquisition(double duration, const std::vector<int>& channels) {
    if (!running_) {
        std::cerr << "Cannot trigger: Master Controller not running" << std::endl;
        return false;
    }
    
    std::cout << "Preparing for synchronized acquisition..." << std::endl;
    
    // Configure local Time Controller
    try {
        // Configure timestamps references
        for (int ch : channels) {
            std::string cmd = "RAW" + std::to_string(ch) + ":REF:LINK NONE";
            send_tc_command(cmd);
        }
        
        // Configure recording settings
        send_tc_command("REC:TRIG:ARM:MODE MANUal");
        send_tc_command("REC:ENABle ON");
        send_tc_command("REC:STOP");
        send_tc_command("REC:NUM INF");
        
        // Set sub-acquisition duration and period
        long long pwid_ps = static_cast<long long>(1e12 * config_.sub_duration);
        long long pper_ps = static_cast<long long>(1e12 * (config_.sub_duration + 40e-9));
        send_tc_command("REC:PWID " + std::to_string(pwid_ps) + ";PPER " + std::to_string(pper_ps));
        
        // Prepare channels
        for (int ch : channels) {
            send_tc_command("RAW" + std::to_string(ch) + ":ERRORS:CLEAR");
            send_tc_command("RAW" + std::to_string(ch) + ":SEND ON");
        }
    } catch (const std::exception& e) {
        std::cerr << "Error configuring local Time Controller: " << e.what() << std::endl;
        return false;
    }
    
    // Prepare trigger message
    json trigger_msg;
    trigger_msg["type"] = "trigger";
    trigger_msg["timestamp"] = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    trigger_msg["duration"] = duration;
    trigger_msg["channels"] = channels;
    trigger_msg["sequence"] = ++trigger_sequence_;
    
    std::string trigger_str = trigger_msg.dump();
    
    // Send trigger message to slave
    std::cout << "Sending trigger to slave..." << std::endl;
    zmq::message_t trigger(trigger_str.size());
    memcpy(trigger.data(), trigger_str.c_str(), trigger_str.size());
    trigger_socket_.send(trigger, zmq::send_flags::none);
    
    // Small delay to ensure slave receives trigger before starting local acquisition
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    
    // Start local acquisition
    std::cout << "Starting local acquisition..." << std::endl;
    send_tc_command("REC:PLAY");
    
    // Wait for specified duration
    std::cout << "Acquisition in progress for " << duration << " seconds..." << std::endl;
    std::this_thread::sleep_for(std::chrono::milliseconds(static_cast<int>(duration * 1000)));
    
    // Stop local acquisition
    std::cout << "Stopping local acquisition..." << std::endl;
    send_tc_command("REC:STOP");
    
    // Wait for data processing to complete
    std::cout << "Waiting for data processing to complete..." << std::endl;
    std::this_thread::sleep_for(std::chrono::seconds(2));
    
    // Turn off sending for all channels
    for (int ch : channels) {
        send_tc_command("RAW" + std::to_string(ch) + ":SEND OFF");
    }
    
    std::cout << "Acquisition completed." << std::endl;
    
    // In local mode, wait a bit longer for the slave to complete file transfer
    if (config_.local_mode) {
        std::cout << "Local mode: Waiting for slave file processing..." << std::endl;
        std::this_thread::sleep_for(std::chrono::seconds(3));
    }
    
    return true;
}

void MasterController::status_monitor_thread() {
    std::cout << "Status monitor thread started" << std::endl;
    
    while (running_) {
        try {
            zmq::message_t message;
            auto result = status_socket_.recv(message, zmq::recv_flags::dontwait);
            
            if (result.has_value()) {
                std::string status_str(static_cast<char*>(message.data()), message.size());
                json status = json::parse(status_str);
                
                if (status["type"] == "status") {
                    std::cout << "Slave status: " << status["state"].get<std::string>();
                    
                    if (status.contains("progress")) {
                        std::cout << " (" << status["progress"].get<double>() << "%)";
                    }
                    
                    if (status.contains("error") && !status["error"].is_null()) {
                        std::cout << " Error: " << status["error"].get<std::string>();
                    }
                    
                    std::cout << std::endl;
                }
            }
        } catch (const zmq::error_t& e) {
            // Ignore EAGAIN errors (no message available)
            if (e.num() != EAGAIN) {
                std::cerr << "ZeroMQ error in status monitor: " << e.what() << std::endl;
            }
        } catch (const std::exception& e) {
            std::cerr << "Error in status monitor: " << e.what() << std::endl;
        }
        
        // Sleep to avoid busy waiting
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    
    std::cout << "Status monitor thread stopped" << std::endl;
}

void MasterController::file_receiver_thread() {
    std::cout << "File receiver thread started" << std::endl;
    
    std::string current_filename;
    std::ofstream output_file;
    size_t expected_chunks = 0;
    size_t received_chunks = 0;
    std::string file_checksum;
    
    while (running_) {
        try {
            zmq::message_t message;
            auto result = file_socket_.recv(message, zmq::recv_flags::dontwait);
            
            if (result.has_value()) {
                // Check if this is a header, data chunk, or footer
                if (message.size() > 0 && static_cast<char*>(message.data())[0] == '{') {
                    // This is a JSON control message (header or footer)
                    std::string msg_str(static_cast<char*>(message.data()), message.size());
                    json msg = json::parse(msg_str);
                    
                    if (msg["type"] == "file_header") {
                        // Start new file reception
                        current_filename = msg["filename"].get<std::string>();
                        expected_chunks = msg["chunks"].get<size_t>();
                        file_checksum = msg["checksum"].get<std::string>();
                        received_chunks = 0;
                        
                        std::cout << "Receiving file: " << current_filename 
                                  << " (" << msg["size"].get<size_t>() << " bytes in " 
                                  << expected_chunks << " chunks)" << std::endl;
                        
                        // Open output file
                        fs::path output_path = fs::path(config_.output_dir) / current_filename;
                        output_file.open(output_path, std::ios::binary);
                        
                        if (!output_file.is_open()) {
                            std::cerr << "Failed to open output file: " << output_path << std::endl;
                        }
                    }
                    else if (msg["type"] == "file_footer") {
                        // End file reception
                        if (output_file.is_open()) {
                            output_file.close();
                        }
                        
                        std::cout << "File reception complete: " << current_filename 
                                  << " (" << received_chunks << "/" << expected_chunks << " chunks)" << std::endl;
                        
                        // TODO: Verify checksum
                        
                        current_filename.clear();
                    }
                }
                else {
                    // This is a data chunk
                    if (output_file.is_open()) {
                        output_file.write(static_cast<char*>(message.data()), message.size());
                        received_chunks++;
                        
                        // Periodically report progress
                        if (received_chunks % 10 == 0 || received_chunks == expected_chunks) {
                            std::cout << "File transfer progress: " << received_chunks << "/" 
                                      << expected_chunks << " chunks (" 
                                      << (received_chunks * 100 / expected_chunks) << "%)" << std::endl;
                        }
                    }
                    else {
                        std::cerr << "Received data chunk but no file is open" << std::endl;
                    }
                }
            }
        } catch (const zmq::error_t& e) {
            // Ignore EAGAIN errors (no message available)
            if (e.num() != EAGAIN) {
                std::cerr << "ZeroMQ error in file receiver: " << e.what() << std::endl;
            }
        } catch (const std::exception& e) {
            std::cerr << "Error in file receiver: " << e.what() << std::endl;
            
            // Close file if open
            if (output_file.is_open()) {
                output_file.close();
            }
        }
        
        // Sleep to avoid busy waiting
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    
    // Close file if still open
    if (output_file.is_open()) {
        output_file.close();
    }
    
    std::cout << "File receiver thread stopped" << std::endl;
}

bool MasterController::send_command(const std::string& command, const json& params) {
    try {
        json cmd;
        cmd["type"] = "command";
        cmd["command"] = command;
        cmd["params"] = params;
        cmd["sequence"] = ++command_sequence_;
        
        std::string cmd_str = cmd.dump();
        zmq::message_t request(cmd_str.size());
        memcpy(request.data(), cmd_str.c_str(), cmd_str.size());
        
        command_socket_.send(request, zmq::send_flags::none);
        
        zmq::message_t reply;
        auto result = command_socket_.recv(reply);
        
        if (!result.has_value()) {
            std::cerr << "No response received for command: " << command << std::endl;
            return false;
        }
        
        std::string reply_str(static_cast<char*>(reply.data()), reply.size());
        json response = json::parse(reply_str);
        
        if (!response["success"].get<bool>()) {
            std::cerr << "Command failed: " << command;
            if (response.contains("error") && !response["error"].is_null()) {
                std::cerr << " - " << response["error"].get<std::string>();
            }
            std::cerr << std::endl;
            return false;
        }
        
        return true;
    } catch (const std::exception& e) {
        std::cerr << "Error sending command: " << e.what() << std::endl;
        return false;
    }
}

std::string MasterController::send_tc_command(const std::string& cmd) {
    zmq::message_t request(cmd.size());
    memcpy(request.data(), cmd.c_str(), cmd.size());
    local_tc_socket_.send(request, zmq::send_flags::none);
    
    zmq::message_t reply;
    auto result = local_tc_socket_.recv(reply);
    if (!result.has_value()) {
        throw std::runtime_error("No response from Time Controller for command: " + cmd);
    }
    
    return std::string(static_cast<char*>(reply.data()), reply.size());
}
