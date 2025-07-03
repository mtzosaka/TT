#include "enhanced_master_controller.hpp"
#include <iostream>
#include <fstream>
#include <vector>
#include <string>
#include <chrono>
#include <thread>
#include <atomic>
#include <cstring>
#include <filesystem>
#include <iomanip>
#include <sstream>
#include "json.hpp"

using json = nlohmann::json;
namespace fs = std::filesystem;

MasterController::MasterController(const MasterConfig& config) 
    : config_(config), 
      context_(1), 
      running_(false),
      acquisition_active_(false),
      current_state_("idle"),
      current_progress_(0.0),
      trigger_timestamp_ns_(0),
      slave_trigger_timestamp_ns_(0),
      calculated_offset_ns_(0.0),
      command_sequence_(0),
      file_counter_(0) {
    
    std::cout << "Initializing Master Controller..." << std::endl;
    std::cout << "Local Time Controller: " << config_.local_tc_address << std::endl;
    std::cout << "Remote Slave: " << config_.slave_address << std::endl;
}

MasterController::~MasterController() {
    stop();
}

bool MasterController::initialize() {
    try {
        // Initialize ZeroMQ sockets
        std::cout << "Setting up communication channels..." << std::endl;
        
        // Trigger publisher
        trigger_socket_ = zmq::socket_t(context_, zmq::socket_type::pub);
        std::string trigger_endpoint = "tcp://*:" + std::to_string(config_.trigger_port);
        std::cout << "DEBUG: Binding trigger socket to " << trigger_endpoint << std::endl;
        trigger_socket_.bind(trigger_endpoint);
        
        // Status receiver
        status_socket_ = zmq::socket_t(context_, zmq::socket_type::pull);
        std::string status_endpoint = "tcp://*:" + std::to_string(config_.status_port);
        std::cout << "DEBUG: Binding status socket to " << status_endpoint << std::endl;
        status_socket_.bind(status_endpoint);
        
        // File receiver
        file_socket_ = zmq::socket_t(context_, zmq::socket_type::pull);
        std::string file_endpoint = "tcp://*:" + std::to_string(config_.file_port);
        std::cout << "DEBUG: Binding file socket to " << file_endpoint << std::endl;
        file_socket_.bind(file_endpoint);
        
        // Command socket (for sending commands to slave)
        command_socket_ = zmq::socket_t(context_, zmq::socket_type::req);
        std::string command_endpoint = "tcp://" + config_.slave_address + ":" + std::to_string(config_.command_port);
        std::cout << "DEBUG: Connecting command socket to " << command_endpoint << std::endl;
        command_socket_.connect(command_endpoint);
        
        // Sync socket (for subscription synchronization)
        sync_socket_ = zmq::socket_t(context_, zmq::socket_type::pull);
        std::string sync_endpoint = "tcp://*:" + std::to_string(config_.sync_port);
        std::cout << "DEBUG: Binding sync socket to " << sync_endpoint << std::endl;
        sync_socket_.bind(sync_endpoint);
        
        // Set socket options for low latency
        int linger = 0;
        trigger_socket_.set(zmq::sockopt::linger, linger);
        status_socket_.set(zmq::sockopt::linger, linger);
        file_socket_.set(zmq::sockopt::linger, linger);
        command_socket_.set(zmq::sockopt::linger, linger);
        sync_socket_.set(zmq::sockopt::linger, linger);
        
        // Set high water mark for file transfer
        int hwm = 1000;
        file_socket_.set(zmq::sockopt::rcvhwm, hwm);
        
        // Create output directory if it doesn't exist
        if (!fs::exists(config_.output_dir)) {
            fs::create_directories(config_.output_dir);
            std::cout << "DEBUG: Created output directory: " << config_.output_dir << std::endl;
        }
        
        // Connect to local Time Controller
        std::cout << "Connecting to local Time Controller..." << std::endl;
        local_tc_socket_ = zmq::socket_t(context_, zmq::socket_type::req);
        std::string tc_endpoint = "tcp://" + config_.local_tc_address + ":5555";
        std::cout << "DEBUG: Connecting to Time Controller at " << tc_endpoint << std::endl;
        local_tc_socket_.connect(tc_endpoint);
        
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
        
        // Check slave availability
        std::cout << "Checking slave availability..." << std::endl;
        json response;
        if (!send_command_to_slave("status", response)) {
            std::cout << "WARNING: Slave not responding. Will continue to try during operation." << std::endl;
            std::cout << "DEBUG: Make sure slave is running and reachable at " << config_.slave_address << std::endl;
            std::cout << "DEBUG: Check network connectivity with: ping " << config_.slave_address << std::endl;
        } else {
            std::cout << "DEBUG: Slave responded successfully!" << std::endl;
            std::cout << "DEBUG: Slave status: " << response.dump(2) << std::endl;
        }
        
        running_ = true;
        
        // Start worker threads
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
        sync_socket_.close();
        local_tc_socket_.close();
        
        std::cout << "Master Controller stopped." << std::endl;
    }
}

bool MasterController::start_acquisition(double duration, const std::vector<int>& channels) {
    if (acquisition_active_) {
        std::cerr << "Acquisition already in progress" << std::endl;
        return false;
    }
    
    try {
        std::cout << "Preparing for synchronized acquisition..." << std::endl;
        
        // Update state
        current_state_ = "starting";
        current_progress_ = 0.0;
        acquisition_duration_ = duration;
        
        // Wait for slave to be ready to receive triggers
        std::cout << "Waiting for slave to be ready for trigger..." << std::endl;
        
        // First, send a command to ensure slave is alive
        json response;
        if (!send_command_to_slave("status", response)) {
            std::cerr << "ERROR: Slave not responding to status command" << std::endl;
            return false;
        }
        
        // Tell slave to prepare for trigger
        if (!send_command_to_slave("prepare_trigger", response)) {
            std::cerr << "ERROR: Failed to prepare slave for trigger" << std::endl;
            return false;
        }
        
        if (!response["success"].get<bool>()) {
            std::cerr << "ERROR: Slave failed to prepare for trigger: " << response["error"].get<std::string>() << std::endl;
            return false;
        }
        
        std::cout << "DEBUG: Slave is preparing for trigger" << std::endl;
        
        // Wait for sync message from slave indicating it's ready for trigger
        std::cout << "DEBUG: Waiting for slave subscription sync message..." << std::endl;
        
        // Set timeout for sync message
        int timeout = 5000;  // 5 seconds
        sync_socket_.set(zmq::sockopt::rcvtimeo, timeout);
        
        zmq::message_t sync_msg;
        auto result = sync_socket_.recv(sync_msg);
        
        if (!result.has_value()) {
            std::cerr << "ERROR: Timeout waiting for slave subscription sync" << std::endl;
            return false;
        }
        
        std::string sync_str(static_cast<char*>(sync_msg.data()), sync_msg.size());
        std::cout << "DEBUG: Received sync message: " << sync_str << std::endl;
        
        // Send trigger to slave
        std::cout << "Sending trigger to slave..." << std::endl;
        
        // Record precise trigger timestamp
        trigger_timestamp_ns_ = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        
        json trigger;
        trigger["type"] = "trigger";
        trigger["timestamp"] = trigger_timestamp_ns_;
        trigger["duration"] = duration;
        trigger["channels"] = channels;
        
        std::string trigger_str = trigger.dump();
        zmq::message_t message(trigger_str.size());
        memcpy(message.data(), trigger_str.c_str(), trigger_str.size());
        
        std::cout << "DEBUG: Sending trigger message: " << trigger_str << std::endl;
        trigger_socket_.send(message, zmq::send_flags::none);
        
        // Wait a moment to ensure trigger is received
        std::cout << "DEBUG: Waiting for slave to process trigger..." << std::endl;
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        
        // Check if slave received trigger
        if (send_command_to_slave("status", response)) {
            if (response.contains("data") && response["data"].contains("state")) {
                std::string slave_state = response["data"]["state"].get<std::string>();
                std::cout << "DEBUG: Slave state after trigger: " << slave_state << std::endl;
                
                if (slave_state != "running" && slave_state != "starting") {
                    std::cout << "WARNING: Slave may not have received trigger command!" << std::endl;
                    
                    // Try sending trigger again
                    std::cout << "DEBUG: Attempting to resend trigger..." << std::endl;
                    trigger_socket_.send(message, zmq::send_flags::none);
                    
                    // Check again after a short delay
                    std::this_thread::sleep_for(std::chrono::milliseconds(500));
                    if (send_command_to_slave("status", response)) {
                        if (response.contains("data") && response["data"].contains("state")) {
                            slave_state = response["data"]["state"].get<std::string>();
                            std::cout << "DEBUG: Slave state after resending trigger: " << slave_state << std::endl;
                            
                            if (slave_state != "running" && slave_state != "starting") {
                                std::cerr << "ERROR: Slave still not responding to trigger" << std::endl;
                                return false;
                            }
                        }
                    }
                }
            }
        }
        
        // Configure local Time Controller
        std::cout << "Starting local acquisition..." << std::endl;
        
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
        
        // Start acquisition
        acquisition_active_ = true;
        acquisition_start_time_ = std::chrono::steady_clock::now();
        current_state_ = "running";
        
        send_tc_command("REC:PLAY");
        
        // Wait for specified duration
        std::cout << "Acquisition in progress for " << duration << " seconds..." << std::endl;
        
        const int update_interval_ms = 100;
        int total_updates = static_cast<int>(duration * 1000 / update_interval_ms);
        
        for (int i = 0; i < total_updates && running_; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(update_interval_ms));
            
            // Update progress
            current_progress_ = static_cast<double>(i) / total_updates * 100.0;
            
            // Periodically check slave status
            if (i % 10 == 0) {  // Every ~1 second
                if (send_command_to_slave("status", response)) {
                    if (response.contains("data") && response["data"].contains("progress")) {
                        double slave_progress = response["data"]["progress"].get<double>();
                        std::cout << "DEBUG: Slave progress: " << slave_progress << "%" << std::endl;
                    }
                }
            }
        }
        
        // Stop acquisition
        std::cout << "Stopping local acquisition..." << std::endl;
        send_tc_command("REC:STOP");
        
        // Wait for data processing to complete
        std::cout << "Waiting for data processing to complete..." << std::endl;
        std::this_thread::sleep_for(std::chrono::seconds(2));
        
        // Turn off sending for all channels
        for (int ch : channels) {
            send_tc_command("RAW" + std::to_string(ch) + ":SEND OFF");
        }
        
        // Generate output filename
        auto now = std::chrono::system_clock::now();
        auto now_time_t = std::chrono::system_clock::to_time_t(now);
        std::tm now_tm = *std::localtime(&now_time_t);
        
        char time_str[20];
        std::strftime(time_str, sizeof(time_str), "%Y%m%d_%H%M%S", &now_tm);
        
        // Process and save data
        if (config_.streaming_mode) {
            // In streaming mode, we'll have multiple files
            for (int i = 0; i < config_.max_files; i++) {
                std::string master_filename = generate_filename(true, i);
                
                // Generate dummy data for now - this would be replaced with actual data from TC
                std::vector<uint64_t> timestamps;
                for (int j = 0; j < 1000; j++) {
                    timestamps.push_back(trigger_timestamp_ns_ + j * 1000000);
                }
                
                // Write to binary file
                write_timestamps_to_bin(timestamps, master_filename);
                
                std::cout << "Saved master timestamps to " << master_filename << std::endl;
            }
        } else {
            // Single file mode
            std::string master_filename = fs::path(config_.output_dir) / ("master_results_" + std::string(time_str) + ".bin");
            
            // Generate dummy data for now - this would be replaced with actual data from TC
            std::vector<uint64_t> timestamps;
            for (int i = 0; i < 10000; i++) {
                timestamps.push_back(trigger_timestamp_ns_ + i * 1000000);
            }
            
            // Write to binary file
            write_timestamps_to_bin(timestamps, master_filename);
            
            std::cout << "Saved master timestamps to " << master_filename << std::endl;
        }
        
        // Wait for slave to complete and transfer files
        std::cout << "DEBUG: Waiting for slave to complete and transfer files..." << std::endl;
        std::this_thread::sleep_for(std::chrono::seconds(5));
        
        // Update state
        acquisition_active_ = false;
        current_state_ = "completed";
        current_progress_ = 100.0;
        
        std::cout << "Acquisition completed." << std::endl;
        return true;
        
    } catch (const std::exception& e) {
        std::cerr << "Error during acquisition: " << e.what() << std::endl;
        
        // Update state
        acquisition_active_ = false;
        current_state_ = "error";
        current_error_ = e.what();
        
        return false;
    }
}

void MasterController::status_monitor_thread() {
    std::cout << "Status monitor thread started" << std::endl;
    
    while (running_) {
        try {
            zmq::message_t message;
            auto result = status_socket_.recv(message, zmq::recv_flags::dontwait);
            
            if (result.has_value()) {
                std::string status_str(static_cast<char*>(message.data()), message.size());
                std::cout << "DEBUG: Received status update: " << status_str << std::endl;
                
                json status = json::parse(status_str);
                
                if (status["type"] == "status") {
                    // Extract slave trigger timestamp if present
                    if (status.contains("trigger_timestamp")) {
                        slave_trigger_timestamp_ns_ = status["trigger_timestamp"].get<uint64_t>();
                        
                        // Calculate offset between master and slave trigger timestamps
                        if (trigger_timestamp_ns_ > 0 && slave_trigger_timestamp_ns_ > 0) {
                            calculated_offset_ns_ = static_cast<double>(slave_trigger_timestamp_ns_) - 
                                                  static_cast<double>(trigger_timestamp_ns_);
                            
                            std::cout << "Calculated trigger offset: " << calculated_offset_ns_ << " ns" << std::endl;
                        }
                    }
                    
                    // Process other status information as needed
                    std::string slave_state = status["state"].get<std::string>();
                    
                    if (slave_state == "running" && status.contains("progress")) {
                        double slave_progress = status["progress"].get<double>();
                        std::cout << "Slave progress: " << slave_progress << "%" << std::endl;
                    }
                    else if (slave_state == "error" && status.contains("error")) {
                        std::string error = status["error"].get<std::string>();
                        std::cerr << "Slave error: " << error << std::endl;
                    }
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
    size_t current_file_size = 0;
    size_t current_chunks = 0;
    size_t chunks_received = 0;
    std::ofstream output_file;
    
    while (running_) {
        try {
            zmq::message_t message;
            auto result = file_socket_.recv(message, zmq::recv_flags::dontwait);
            
            if (result.has_value()) {
                std::cout << "DEBUG: Received file data of size " << message.size() << " bytes" << std::endl;
                
                // Check if this is a JSON header/footer or binary chunk
                if (message.size() > 0 && 
                    (static_cast<char*>(message.data())[0] == '{' || 
                     static_cast<char*>(message.data())[0] == '[')) {
                    
                    // Try to parse as JSON
                    try {
                        std::string json_str(static_cast<char*>(message.data()), message.size());
                        json msg = json::parse(json_str);
                        
                        if (msg["type"] == "file_header") {
                            // Start of new file
                            current_filename = msg["filename"].get<std::string>();
                            current_file_size = msg["size"].get<size_t>();
                            current_chunks = msg["chunks"].get<size_t>();
                            chunks_received = 0;
                            
                            std::cout << "DEBUG: File header received: " << json_str << std::endl;
                            
                            // Close previous file if open
                            if (output_file.is_open()) {
                                output_file.close();
                            }
                            
                            // Open new file
                            fs::path output_path = fs::path(config_.output_dir) / current_filename;
                            output_file.open(output_path, std::ios::binary);
                            
                            if (!output_file.is_open()) {
                                std::cerr << "Failed to open output file: " << output_path << std::endl;
                                continue;
                            }
                            
                            std::cout << "Receiving file: " << current_filename << " (" << current_file_size << " bytes)" << std::endl;
                        }
                        else if (msg["type"] == "file_footer") {
                            // End of file
                            std::cout << "DEBUG: File footer received: " << json_str << std::endl;
                            
                            if (output_file.is_open()) {
                                output_file.close();
                                
                                // Check if this is a slave results file for synchronization
                                if (current_filename.find("slave_results_") != std::string::npos) {
                                    // Find corresponding master file
                                    std::string master_filename = current_filename;
                                    master_filename.replace(master_filename.find("slave_"), 6, "master_");
                                    
                                    fs::path master_path = fs::path(config_.output_dir) / master_filename;
                                    fs::path slave_path = fs::path(config_.output_dir) / current_filename;
                                    
                                    if (fs::exists(master_path) && fs::exists(slave_path)) {
                                        // Calculate synchronization
                                        calculate_sync(master_path.string(), slave_path.string());
                                    } else {
                                        std::cout << "DEBUG: Cannot calculate sync, files not found:" << std::endl;
                                        std::cout << "DEBUG: Master: " << master_path << " exists: " << fs::exists(master_path) << std::endl;
                                        std::cout << "DEBUG: Slave: " << slave_path << " exists: " << fs::exists(slave_path) << std::endl;
                                    }
                                }
                                
                                std::cout << "File transfer complete: " << current_filename << std::endl;
                            }
                        }
                    } catch (const json::parse_error& e) {
                        // Not valid JSON, treat as binary chunk
                        std::cout << "DEBUG: Received binary chunk (not JSON)" << std::endl;
                        if (output_file.is_open()) {
                            output_file.write(static_cast<char*>(message.data()), message.size());
                            chunks_received++;
                        } else {
                            std::cerr << "ERROR: Received binary chunk but no file is open!" << std::endl;
                        }
                    }
                } else {
                    // Binary chunk
                    if (output_file.is_open()) {
                        output_file.write(static_cast<char*>(message.data()), message.size());
                        chunks_received++;
                        
                        // Print progress for large files
                        if (current_chunks > 10 && chunks_received % (current_chunks / 10) == 0) {
                            double progress = static_cast<double>(chunks_received) / current_chunks * 100.0;
                            std::cout << "File transfer progress: " << progress << "%" << std::endl;
                        }
                    } else {
                        std::cerr << "ERROR: Received binary chunk but no file is open!" << std::endl;
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

std::string MasterController::send_tc_command(const std::string& cmd) {
    try {
        zmq::message_t request(cmd.size());
        memcpy(request.data(), cmd.c_str(), cmd.size());
        local_tc_socket_.send(request, zmq::send_flags::none);
        
        zmq::message_t reply;
        auto result = local_tc_socket_.recv(reply, zmq::recv_flags::none);
        if (!result.has_value()) {
            return "ERROR: No response from Time Controller";
        }
        
        return std::string(static_cast<char*>(reply.data()), reply.size());
    } catch (const std::exception& e) {
        return "ERROR: " + std::string(e.what());
    }
}

bool MasterController::send_command_to_slave(const std::string& command, json& response) {
    try {
        // Prepare command
        json cmd;
        cmd["type"] = "command";
        cmd["command"] = command;
        cmd["sequence"] = command_sequence_++;
        
        std::string cmd_str = cmd.dump();
        zmq::message_t request(cmd_str.size());
        memcpy(request.data(), cmd_str.c_str(), cmd_str.size());
        
        // Set timeout for command
        int timeout = 1000;  // 1 second
        command_socket_.set(zmq::sockopt::rcvtimeo, timeout);
        
        std::cout << "DEBUG: Sending command to slave: " << cmd_str << std::endl;
        
        // Send command
        command_socket_.send(request, zmq::send_flags::none);
        
        // Wait for response
        zmq::message_t reply;
        auto result = command_socket_.recv(reply, zmq::recv_flags::none);
        if (!result.has_value()) {
            std::cerr << "DEBUG: No response received for command: " << command << std::endl;
            return false;
        }
        
        // Parse response
        std::string reply_str(static_cast<char*>(reply.data()), reply.size());
        std::cout << "DEBUG: Received response: " << reply_str << std::endl;
        response = json::parse(reply_str);
        
        return true;
    } catch (const std::exception& e) {
        std::cerr << "Error sending command to slave: " << e.what() << std::endl;
        return false;
    }
}

void MasterController::calculate_sync(const std::string& master_file, const std::string& slave_file) {
    try {
        std::cout << "Calculating synchronization between master and slave..." << std::endl;
        
        // Read timestamps from files (only first 10% for sync calculation)
        std::vector<uint64_t> master_timestamps = read_timestamps_from_bin(master_file, config_.sync_percentage);
        std::vector<uint64_t> slave_timestamps = read_timestamps_from_bin(slave_file, config_.sync_percentage);
        
        if (master_timestamps.empty() || slave_timestamps.empty()) {
            std::cerr << "Error: Empty timestamp data" << std::endl;
            return;
        }
        
        std::cout << "DEBUG: Read " << master_timestamps.size() << " timestamps from master file" << std::endl;
        std::cout << "DEBUG: Read " << slave_timestamps.size() << " timestamps from slave file" << std::endl;
        
        // Calculate offset statistics
        double min_offset = 1e12;
        double max_offset = -1e12;
        double sum_offset = 0.0;
        int count = 0;
        
        // Use the smaller of the two arrays
        size_t compare_size = std::min(master_timestamps.size(), slave_timestamps.size());
        
        for (size_t i = 0; i < compare_size; i++) {
            double offset = static_cast<double>(slave_timestamps[i]) - static_cast<double>(master_timestamps[i]);
            
            min_offset = std::min(min_offset, offset);
            max_offset = std::max(max_offset, offset);
            sum_offset += offset;
            count++;
        }
        
        double avg_offset = sum_offset / count;
        
        // Calculate standard deviation
        double sum_squared_diff = 0.0;
        for (size_t i = 0; i < compare_size; i++) {
            double offset = static_cast<double>(slave_timestamps[i]) - static_cast<double>(master_timestamps[i]);
            double diff = offset - avg_offset;
            sum_squared_diff += diff * diff;
        }
        
        double std_dev = std::sqrt(sum_squared_diff / count);
        
        // Write results to file
        fs::path sync_path = fs::path(config_.output_dir) / "sync_results.txt";
        std::ofstream sync_file(sync_path);
        
        if (sync_file.is_open()) {
            sync_file << "Synchronization Results" << std::endl;
            sync_file << "======================" << std::endl;
            sync_file << "Master file: " << master_file << std::endl;
            sync_file << "Slave file: " << slave_file << std::endl;
            sync_file << "Trigger timestamp offset: " << calculated_offset_ns_ << " ns" << std::endl;
            sync_file << "Data timestamp statistics (in nanoseconds):" << std::endl;
            sync_file << "  Minimum offset: " << min_offset << std::endl;
            sync_file << "  Maximum offset: " << max_offset << std::endl;
            sync_file << "  Average offset: " << avg_offset << std::endl;
            sync_file << "  Standard deviation: " << std_dev << std::endl;
            sync_file << "  Sample count: " << count << std::endl;
            
            sync_file.close();
            
            std::cout << "Synchronization results written to " << sync_path << std::endl;
        }
        
        // Print summary to console
        std::cout << "Synchronization summary:" << std::endl;
        std::cout << "  Average offset: " << avg_offset << " ns" << std::endl;
        std::cout << "  Standard deviation: " << std_dev << " ns" << std::endl;
        
    } catch (const std::exception& e) {
        std::cerr << "Error calculating synchronization: " << e.what() << std::endl;
    }
}

void MasterController::write_timestamps_to_bin(const std::vector<uint64_t>& timestamps, const std::string& filename) {
    std::ofstream file(filename, std::ios::binary);
    
    if (!file.is_open()) {
        std::cerr << "Failed to open file for writing: " << filename << std::endl;
        return;
    }
    
    // Write number of timestamps
    uint64_t count = timestamps.size();
    file.write(reinterpret_cast<char*>(&count), sizeof(count));
    
    // Write timestamps
    file.write(reinterpret_cast<const char*>(timestamps.data()), count * sizeof(uint64_t));
    
    file.close();
    
    std::cout << "DEBUG: Wrote " << count << " timestamps to " << filename << std::endl;
}

std::vector<uint64_t> MasterController::read_timestamps_from_bin(const std::string& filename, double percentage) {
    std::vector<uint64_t> timestamps;
    std::ifstream file(filename, std::ios::binary);
    
    if (!file.is_open()) {
        std::cerr << "Failed to open file for reading: " << filename << std::endl;
        return timestamps;
    }
    
    // Read number of timestamps
    uint64_t count;
    file.read(reinterpret_cast<char*>(&count), sizeof(count));
    
    // Calculate how many timestamps to read based on percentage
    uint64_t read_count = static_cast<uint64_t>(count * percentage);
    if (read_count < 1) read_count = 1;
    if (read_count > count) read_count = count;
    
    // Read timestamps
    timestamps.resize(read_count);
    file.read(reinterpret_cast<char*>(timestamps.data()), read_count * sizeof(uint64_t));
    
    file.close();
    
    std::cout << "DEBUG: Read " << read_count << " of " << count << " timestamps from " << filename << std::endl;
    
    return timestamps;
}

std::string MasterController::generate_filename(bool is_master, int file_index) {
    auto now = std::chrono::system_clock::now();
    auto now_time_t = std::chrono::system_clock::to_time_t(now);
    std::tm now_tm = *std::localtime(&now_time_t);
    
    char time_str[20];
    std::strftime(time_str, sizeof(time_str), "%Y%m%d_%H%M%S", &now_tm);
    
    std::string prefix = is_master ? "master" : "slave";
    std::string filename = prefix + "_results_" + std::string(time_str) + "_" + 
                          std::to_string(file_index) + ".bin";
    
    return fs::path(config_.output_dir) / filename;
}
