#include "fixed_master_controller.hpp"
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
#include <cmath>
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
    
    log_message("Initializing Master Controller...", true);
    log_message("Local Time Controller: " + config_.local_tc_address, true);
    log_message("Remote Slave: " + config_.slave_address, true);
}

MasterController::~MasterController() {
    stop();
}

void MasterController::log_message(const std::string& message, bool always_show) {
    if (always_show || config_.verbose_output) {
        std::cout << message << std::endl;
    }
}

bool MasterController::initialize() {
    try {
        // Initialize ZeroMQ sockets
        log_message("Setting up communication channels...", true);
        
        // Trigger publisher
        trigger_socket_ = zmq::socket_t(context_, zmq::socket_type::pub);
        std::string trigger_endpoint = "tcp://*:" + std::to_string(config_.trigger_port);
        log_message("Binding trigger socket to " + trigger_endpoint);
        trigger_socket_.bind(trigger_endpoint);
        
        // Status receiver
        status_socket_ = zmq::socket_t(context_, zmq::socket_type::pull);
        std::string status_endpoint = "tcp://*:" + std::to_string(config_.status_port);
        log_message("Binding status socket to " + status_endpoint);
        status_socket_.bind(status_endpoint);
        
        // File receiver
        file_socket_ = zmq::socket_t(context_, zmq::socket_type::pull);
        std::string file_endpoint = "tcp://*:" + std::to_string(config_.file_port);
        log_message("Binding file socket to " + file_endpoint);
        file_socket_.bind(file_endpoint);
        
        // Command socket (for sending commands to slave)
        command_socket_ = zmq::socket_t(context_, zmq::socket_type::req);
        std::string command_endpoint = "tcp://" + config_.slave_address + ":" + std::to_string(config_.command_port);
        log_message("Connecting command socket to " + command_endpoint);
        command_socket_.connect(command_endpoint);
        
        // Sync socket (for subscription synchronization)
        sync_socket_ = zmq::socket_t(context_, zmq::socket_type::pull);
        std::string sync_endpoint = "tcp://*:" + std::to_string(config_.sync_port);
        log_message("Binding sync socket to " + sync_endpoint);
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
            log_message("Created output directory: " + config_.output_dir);
        }
        
        // Connect to local Time Controller
        log_message("Connecting to local Time Controller...", true);
        local_tc_socket_ = zmq::socket_t(context_, zmq::socket_type::req);
        std::string tc_endpoint = "tcp://" + config_.local_tc_address + ":5555";
        log_message("Connecting to Time Controller at " + tc_endpoint);
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
        log_message("Local Time Controller identified: " + idn, true);
        
        // Check slave availability
        log_message("Checking slave availability...", true);
        json response;
        if (!send_command_to_slave("status", response)) {
            log_message("WARNING: Slave not responding. Will continue to try during operation.", true);
            log_message("Make sure slave is running and reachable at " + config_.slave_address);
            log_message("Check network connectivity with: ping " + config_.slave_address);
        } else {
            log_message("Slave responded successfully!");
            if (config_.verbose_output) {
                log_message("Slave status: " + response.dump(2));
            }
        }
        
        running_ = true;
        
        // Start worker threads
        status_thread_ = std::thread(&MasterController::status_monitor_thread, this);
        file_thread_ = std::thread(&MasterController::file_receiver_thread, this);
        
        log_message("Master Controller initialized successfully.", true);
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
        
        log_message("Master Controller stopped.", true);
    }
}

bool MasterController::start_acquisition(double duration, const std::vector<int>& channels) {
    if (acquisition_active_) {
        std::cerr << "Acquisition already in progress" << std::endl;
        return false;
    }
    
    try {
        log_message("Preparing for synchronized acquisition...", true);
        
        // Store active channels for text output
        active_channels_ = channels;
        
        // Update state
        current_state_ = "starting";
        current_progress_ = 0.0;
        acquisition_duration_ = duration;
        
        // Wait for slave to be ready to receive triggers
        log_message("Waiting for slave to be ready for trigger...", true);
        
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
        
        log_message("Slave is preparing for trigger");
        
        // Wait for sync message from slave indicating it's ready for trigger
        log_message("Waiting for slave subscription sync message...");
        
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
        log_message("Received sync message: " + sync_str);
        
        // Send trigger to slave
        log_message("Sending trigger to slave...", true);
        
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
        
        log_message("Sending trigger message: " + trigger_str);
        trigger_socket_.send(message, zmq::send_flags::none);
        
        // Wait a moment to ensure trigger is received
        log_message("Waiting for slave to process trigger...");
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        
        // Check if slave received trigger
        if (send_command_to_slave("status", response)) {
            if (response.contains("data") && response["data"].contains("state")) {
                std::string slave_state = response["data"]["state"].get<std::string>();
                log_message("Slave state after trigger: " + slave_state);
                
                if (slave_state != "running" && slave_state != "starting") {
                    log_message("WARNING: Slave may not have received trigger command!", true);
                    
                    // Try sending trigger again
                    log_message("Attempting to resend trigger...");
                    trigger_socket_.send(message, zmq::send_flags::none);
                    
                    // Check again after a short delay
                    std::this_thread::sleep_for(std::chrono::milliseconds(500));
                    if (send_command_to_slave("status", response)) {
                        if (response.contains("data") && response["data"].contains("state")) {
                            slave_state = response["data"]["state"].get<std::string>();
                            log_message("Slave state after resending trigger: " + slave_state);
                            
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
        log_message("Starting local acquisition...", true);
        
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
        log_message("Acquisition in progress for " + std::to_string(duration) + " seconds...", true);
        
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
                        log_message("Slave progress: " + std::to_string(slave_progress) + "%");
                    }
                }
            }
        }
        
        // Stop acquisition
        log_message("Stopping local acquisition...", true);
        send_tc_command("REC:STOP");
        
        // Wait for data processing to complete
        log_message("Waiting for data processing to complete...", true);
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
                
                // Write to text file if enabled
                if (config_.text_output) {
                    std::string text_filename = generate_filename(true, i, true);
                    write_timestamps_to_txt(timestamps, channels, text_filename);
                }
                
                log_message("Saved master timestamps to " + master_filename, true);
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
            
            // Write to text file if enabled
            if (config_.text_output) {
                std::string text_filename = fs::path(config_.output_dir) / ("master_results_" + std::string(time_str) + ".txt");
                write_timestamps_to_txt(timestamps, channels, text_filename);
            }
            
            log_message("Saved master timestamps to " + master_filename, true);
        }
        
        // Wait for slave to complete and transfer files
        log_message("Waiting for slave to complete and transfer files...");
        std::this_thread::sleep_for(std::chrono::seconds(5));
        
        // Update state
        acquisition_active_ = false;
        current_state_ = "completed";
        current_progress_ = 100.0;
        
        log_message("Acquisition completed.", true);
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
    log_message("Status monitor thread started");
    
    while (running_) {
        try {
            zmq::message_t message;
            auto result = status_socket_.recv(message, zmq::recv_flags::dontwait);
            
            if (result.has_value()) {
                std::string status_str(static_cast<char*>(message.data()), message.size());
                log_message("Received status update: " + status_str);
                
                json status = json::parse(status_str);
                
                if (status["type"] == "status") {
                    // Extract slave trigger timestamp if present
                    if (status.contains("trigger_timestamp")) {
                        slave_trigger_timestamp_ns_ = status["trigger_timestamp"].get<uint64_t>();
                        
                        // Calculate offset between master and slave trigger timestamps
                        if (trigger_timestamp_ns_ > 0 && slave_trigger_timestamp_ns_ > 0) {
                            calculated_offset_ns_ = static_cast<double>(slave_trigger_timestamp_ns_) - 
                                                  static_cast<double>(trigger_timestamp_ns_);
                            
                            log_message("Calculated trigger offset: " + std::to_string(calculated_offset_ns_) + " ns", true);
                        }
                    }
                    
                    // Process other status information as needed
                    std::string slave_state = status["state"].get<std::string>();
                    
                    if (slave_state == "running" && status.contains("progress")) {
                        double slave_progress = status["progress"].get<double>();
                        if (config_.verbose_output) {
                            log_message("Slave progress: " + std::to_string(slave_progress) + "%");
                        }
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
    
    log_message("Status monitor thread stopped");
}

void MasterController::file_receiver_thread() {
    log_message("File receiver thread started");
    
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
                log_message("Received file data of size " + std::to_string(message.size()) + " bytes");
                
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
                            
                            log_message("File header received: " + json_str);
                            
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
                            
                            log_message("Receiving file: " + current_filename + " (" + std::to_string(current_file_size) + " bytes)", true);
                        }
                        else if (msg["type"] == "file_footer") {
                            // End of file
                            log_message("File footer received: " + json_str);
                            
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
                                        log_message("Cannot calculate sync, files not found:");
                                        log_message("Master: " + master_path.string() + " exists: " + std::to_string(fs::exists(master_path)));
                                        log_message("Slave: " + slave_path.string() + " exists: " + std::to_string(fs::exists(slave_path)));
                                    }
                                }
                                
                                log_message("File transfer complete: " + current_filename, true);
                            }
                        }
                    } catch (const json::parse_error& e) {
                        // Not valid JSON, treat as binary chunk
                        log_message("Received binary chunk (not JSON)");
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
                            log_message("File transfer progress: " + std::to_string(progress) + "%");
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
    
    log_message("File receiver thread stopped");
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
        
        log_message("Sending command to slave: " + cmd_str);
        
        // Send command
        command_socket_.send(request, zmq::send_flags::none);
        
        // Wait for response
        zmq::message_t reply;
        auto result = command_socket_.recv(reply, zmq::recv_flags::none);
        if (!result.has_value()) {
            log_message("No response received for command: " + command);
            return false;
        }
        
        // Parse response
        std::string reply_str(static_cast<char*>(reply.data()), reply.size());
        log_message("Received response: " + reply_str);
        response = json::parse(reply_str);
        
        return true;
    } catch (const std::exception& e) {
        std::cerr << "Error sending command to slave: " << e.what() << std::endl;
        return false;
    }
}

void MasterController::calculate_sync(const std::string& master_file, const std::string& slave_file) {
    try {
        log_message("Calculating synchronization between master and slave...", true);
        
        // Read timestamps from files (only first 10% for sync calculation)
        std::vector<uint64_t> master_timestamps = read_timestamps_from_bin(master_file, config_.sync_percentage);
        std::vector<uint64_t> slave_timestamps = read_timestamps_from_bin(slave_file, config_.sync_percentage);
        
        if (master_timestamps.empty() || slave_timestamps.empty()) {
            std::cerr << "Error: Empty timestamp data" << std::endl;
            return;
        }
        
        log_message("Read " + std::to_string(master_timestamps.size()) + " timestamps from master file");
        log_message("Read " + std::to_string(slave_timestamps.size()) + " timestamps from slave file");
        
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
        
        // Generate timestamp for offset report file
        auto now = std::chrono::system_clock::now();
        auto now_time_t = std::chrono::system_clock::to_time_t(now);
        std::tm now_tm = *std::localtime(&now_time_t);
        
        char time_str[20];
        std::strftime(time_str, sizeof(time_str), "%Y%m%d_%H%M%S", &now_tm);
        
        // Write results to separate offset file
        fs::path offset_path = fs::path(config_.output_dir) / ("offset_report_" + std::string(time_str) + ".txt");
        write_offset_report(offset_path.string(), calculated_offset_ns_, min_offset, max_offset, avg_offset, std_dev);
        
        // Print summary to console
        log_message("Synchronization summary:", true);
        log_message("  Average offset: " + std::to_string(avg_offset) + " ns", true);
        log_message("  Standard deviation: " + std::to_string(std_dev) + " ns", true);
        log_message("  Detailed report written to: " + offset_path.string(), true);
        
    } catch (const std::exception& e) {
        std::cerr << "Error calculating synchronization: " << e.what() << std::endl;
    }
}

void MasterController::write_offset_report(const std::string& filename, double offset_ns, double min_offset, double max_offset, double avg_offset, double std_dev) {
    std::ofstream report_file(filename);
    
    if (!report_file.is_open()) {
        std::cerr << "Failed to open offset report file: " << filename << std::endl;
        return;
    }
    
    // Fix: Store time_t in a variable before passing to localtime
    auto now = std::chrono::system_clock::now();
    std::time_t now_time_t = std::chrono::system_clock::to_time_t(now);
    std::tm* now_tm = std::localtime(&now_time_t);
    
    report_file << "Distributed Timestamp System - Synchronization Report" << std::endl;
    report_file << "=================================================" << std::endl;
    report_file << "Generated: " << std::put_time(now_tm, "%Y-%m-%d %H:%M:%S") << std::endl;
    report_file << std::endl;
    
    report_file << "Master Controller: " << config_.local_tc_address << std::endl;
    report_file << "Slave Agent: " << config_.slave_address << std::endl;
    report_file << std::endl;
    
    report_file << "Trigger Timestamp Offset: " << offset_ns << " ns" << std::endl;
    report_file << std::endl;
    
    report_file << "Data Timestamp Statistics (nanoseconds):" << std::endl;
    report_file << "  Minimum Offset: " << min_offset << std::endl;
    report_file << "  Maximum Offset: " << max_offset << std::endl;
    report_file << "  Average Offset: " << avg_offset << std::endl;
    report_file << "  Standard Deviation: " << std_dev << std::endl;
    report_file << "  Range: " << (max_offset - min_offset) << std::endl;
    report_file << std::endl;
    
    report_file << "Synchronization Quality Assessment:" << std::endl;
    if (std_dev < 100.0) {
        report_file << "  Excellent synchronization (std dev < 100 ns)" << std::endl;
    } else if (std_dev < 500.0) {
        report_file << "  Good synchronization (std dev < 500 ns)" << std::endl;
    } else if (std_dev < 1000.0) {
        report_file << "  Acceptable synchronization (std dev < 1000 ns)" << std::endl;
    } else {
        report_file << "  Poor synchronization (std dev > 1000 ns)" << std::endl;
        report_file << "  Consider checking network conditions and Time Controller settings" << std::endl;
    }
    
    report_file.close();
    
    log_message("Offset report written to " + filename);
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
    
    log_message("Wrote " + std::to_string(count) + " timestamps to " + filename);
}

void MasterController::write_timestamps_to_txt(const std::vector<uint64_t>& timestamps, const std::vector<int>& channels, const std::string& filename) {
    std::ofstream file(filename);
    
    if (!file.is_open()) {
        std::cerr << "Failed to open text file for writing: " << filename << std::endl;
        return;
    }
    
    // Fix: Store time_t in a variable before passing to localtime
    auto now = std::chrono::system_clock::now();
    std::time_t now_time_t = std::chrono::system_clock::to_time_t(now);
    std::tm* now_tm = std::localtime(&now_time_t);
    
    // Write header
    file << "# Distributed Timestamp System - Master Controller Data" << std::endl;
    file << "# Generated: " << std::put_time(now_tm, "%Y-%m-%d %H:%M:%S") << std::endl;
    file << "# Channels: ";
    for (size_t i = 0; i < channels.size(); ++i) {
        file << channels[i];
        if (i < channels.size() - 1) file << ", ";
    }
    file << std::endl;
    file << "# Total timestamps: " << timestamps.size() << std::endl;
    file << "# Format: index, timestamp_ns, channel" << std::endl;
    file << "#----------------------------------------" << std::endl;
    
    // Write timestamps with channel information
    for (size_t i = 0; i < timestamps.size(); ++i) {
        // For demonstration, we'll assign channels in a round-robin fashion
        // In a real implementation, each timestamp would have its actual channel
        int channel = channels[i % channels.size()];
        
        file << i << ", " << timestamps[i] << ", " << channel << std::endl;
    }
    
    file.close();
    
    log_message("Wrote " + std::to_string(timestamps.size()) + " timestamps to text file " + filename);
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
    
    log_message("Read " + std::to_string(read_count) + " of " + std::to_string(count) + " timestamps from " + filename);
    
    return timestamps;
}

std::string MasterController::generate_filename(bool is_master, int file_index, bool text_format) {
    auto now = std::chrono::system_clock::now();
    auto now_time_t = std::chrono::system_clock::to_time_t(now);
    std::tm now_tm = *std::localtime(&now_time_t);
    
    char time_str[20];
    std::strftime(time_str, sizeof(time_str), "%Y%m%d_%H%M%S", &now_tm);
    
    std::string prefix = is_master ? "master" : "slave";
    std::string extension = text_format ? ".txt" : ".bin";
    std::string filename = prefix + "_results_" + std::string(time_str) + "_" + 
                          std::to_string(file_index) + extension;
    
    return fs::path(config_.output_dir) / filename;
}
