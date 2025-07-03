#include "fixed_slave_agent.hpp"
#include <iostream>
#include <fstream>
#include <chrono>
#include <thread>
#include <filesystem>
#include <cstring>
#include <iomanip>
#include <sstream>
#include <cmath>

using json = nlohmann::json;
namespace fs = std::filesystem;

SlaveAgent::SlaveAgent(const SlaveConfig& config) 
    : config_(config), 
      context_(1), 
      running_(false),
      current_state_("idle"),
      current_error_(""),
      current_progress_(0.0),
      trigger_timestamp_ns_(0),
      status_sequence_(0),
      file_sequence_(0),
      file_counter_(0) {
    
    log_message("Initializing Slave Agent...", true);
    log_message("Local Time Controller: " + config_.local_tc_address, true);
    log_message("Master address: " + config_.master_address, true);
}

SlaveAgent::~SlaveAgent() {
    stop();
}

void SlaveAgent::log_message(const std::string& message, bool always_show) {
    if (always_show || config_.verbose_output) {
        std::cout << message << std::endl;
    }
}

bool SlaveAgent::initialize() {
    try {
        // Initialize ZeroMQ sockets
        log_message("Setting up communication channels...", true);
        
        // Trigger subscriber
        trigger_socket_ = zmq::socket_t(context_, zmq::socket_type::sub);
        std::string trigger_endpoint = "tcp://" + config_.master_address + ":" + std::to_string(config_.trigger_port);
        log_message("Connecting trigger socket to " + trigger_endpoint);
        trigger_socket_.connect(trigger_endpoint);
        trigger_socket_.set(zmq::sockopt::subscribe, "");
        log_message("Subscribed to all messages on trigger socket");
        
        // Status sender
        status_socket_ = zmq::socket_t(context_, zmq::socket_type::push);
        std::string status_endpoint = "tcp://" + config_.master_address + ":" + std::to_string(config_.status_port);
        log_message("Connecting status socket to " + status_endpoint);
        status_socket_.connect(status_endpoint);
        
        // File sender
        file_socket_ = zmq::socket_t(context_, zmq::socket_type::push);
        std::string file_endpoint = "tcp://" + config_.master_address + ":" + std::to_string(config_.file_port);
        log_message("Connecting file socket to " + file_endpoint);
        file_socket_.connect(file_endpoint);
        
        // Command socket (for receiving commands from master)
        command_socket_ = zmq::socket_t(context_, zmq::socket_type::rep);
        std::string command_endpoint = "tcp://*:" + std::to_string(config_.command_port);
        log_message("Binding command socket to " + command_endpoint);
        command_socket_.bind(command_endpoint);
        
        // Sync socket (for subscription synchronization)
        sync_socket_ = zmq::socket_t(context_, zmq::socket_type::push);
        std::string sync_endpoint = "tcp://" + config_.master_address + ":" + std::to_string(config_.sync_port);
        log_message("Connecting sync socket to " + sync_endpoint);
        sync_socket_.connect(sync_endpoint);
        
        // Set socket options for low latency
        int linger = 0;
        trigger_socket_.set(zmq::sockopt::linger, linger);
        status_socket_.set(zmq::sockopt::linger, linger);
        file_socket_.set(zmq::sockopt::linger, linger);
        command_socket_.set(zmq::sockopt::linger, linger);
        sync_socket_.set(zmq::sockopt::linger, linger);
        
        // Set high water mark for file transfer
        int hwm = 1000;
        file_socket_.set(zmq::sockopt::sndhwm, hwm);
        
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
        
        running_ = true;
        
        // Start worker threads
        trigger_thread_ = std::thread(&SlaveAgent::trigger_listener_thread, this);
        command_thread_ = std::thread(&SlaveAgent::command_handler_thread, this);
        heartbeat_thread_ = std::thread(&SlaveAgent::heartbeat_thread, this);
        
        log_message("Slave Agent initialized successfully.", true);
        log_message("Slave agent initialized and waiting for trigger commands...", true);
        log_message("Press Ctrl+C to stop", true);
        return true;
        
    } catch (const zmq::error_t& e) {
        std::cerr << "ZeroMQ error during initialization: " << e.what() << std::endl;
        return false;
    } catch (const std::exception& e) {
        std::cerr << "Error during initialization: " << e.what() << std::endl;
        return false;
    }
}

void SlaveAgent::stop() {
    if (running_) {
        running_ = false;
        
        // Join threads
        if (trigger_thread_.joinable()) {
            trigger_thread_.join();
        }
        if (command_thread_.joinable()) {
            command_thread_.join();
        }
        if (heartbeat_thread_.joinable()) {
            heartbeat_thread_.join();
        }
        if (acquisition_thread_.joinable()) {
            acquisition_thread_.join();
        }
        
        // Close sockets
        trigger_socket_.close();
        status_socket_.close();
        file_socket_.close();
        command_socket_.close();
        sync_socket_.close();
        local_tc_socket_.close();
        
        log_message("Slave Agent stopped.", true);
    }
}

void SlaveAgent::trigger_listener_thread() {
    log_message("Trigger listener thread started");
    
    // Set a shorter timeout for more responsive trigger detection
    int timeout = 100; // 100ms
    trigger_socket_.set(zmq::sockopt::rcvtimeo, timeout);
    
    while (running_) {
        try {
            log_message("Waiting for trigger message...");
            
            zmq::message_t message;
            auto result = trigger_socket_.recv(message);
            
            if (result.has_value()) {
                std::string trigger_str(static_cast<char*>(message.data()), message.size());
                log_message("Received message: " + trigger_str);
                
                try {
                    json trigger = json::parse(trigger_str);
                    
                    if (trigger["type"] == "trigger") {
                        log_message("Received trigger command from master", true);
                        
                        // Record trigger reception timestamp
                        trigger_timestamp_ns_ = std::chrono::duration_cast<std::chrono::nanoseconds>(
                            std::chrono::system_clock::now().time_since_epoch()).count();
                        
                        // Extract parameters
                        double duration = trigger["duration"].get<double>();
                        std::vector<int> channels = trigger["channels"].get<std::vector<int>>();
                        
                        // Store active channels for text output
                        active_channels_ = channels;
                        
                        std::stringstream ch_str;
                        ch_str << "Trigger parameters - duration: " << duration << ", channels: ";
                        for (size_t i = 0; i < channels.size(); ++i) {
                            ch_str << channels[i];
                            if (i < channels.size() - 1) ch_str << ", ";
                        }
                        log_message(ch_str.str());
                        
                        // Extract master trigger timestamp if present
                        if (trigger.contains("timestamp")) {
                            uint64_t master_timestamp = trigger["timestamp"].get<uint64_t>();
                            log_message("Master trigger timestamp: " + std::to_string(master_timestamp) + " ns", true);
                            log_message("Slave trigger timestamp: " + std::to_string(trigger_timestamp_ns_) + " ns", true);
                            log_message("Estimated offset: " + std::to_string(trigger_timestamp_ns_ - master_timestamp) + " ns", true);
                        }
                        
                        // Start acquisition in a separate thread
                        if (acquisition_thread_.joinable()) {
                            acquisition_thread_.join();
                        }
                        
                        log_message("Starting acquisition thread");
                        acquisition_thread_ = std::thread(&SlaveAgent::handle_acquisition, this, 
                                                         duration, channels);
                    } else {
                        log_message("Received non-trigger message type: " + trigger["type"].get<std::string>());
                    }
                } catch (const json::parse_error& e) {
                    std::cerr << "ERROR: Failed to parse trigger message: " << e.what() << std::endl;
                    std::cerr << "Message content: " << trigger_str << std::endl;
                }
            } else {
                // No message received within timeout, just continue loop
            }
        } catch (const zmq::error_t& e) {
            // Ignore EAGAIN errors (no message available)
            if (e.num() != EAGAIN) {
                std::cerr << "ZeroMQ error in trigger listener: " << e.what() << std::endl;
            }
        } catch (const std::exception& e) {
            std::cerr << "Error in trigger listener: " << e.what() << std::endl;
        }
        
        // Sleep to avoid busy waiting
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    
    log_message("Trigger listener thread stopped");
}

void SlaveAgent::command_handler_thread() {
    log_message("Command handler thread started");
    
    while (running_) {
        try {
            zmq::message_t message;
            auto result = command_socket_.recv(message, zmq::recv_flags::dontwait);
            
            if (result.has_value()) {
                std::string cmd_str(static_cast<char*>(message.data()), message.size());
                log_message("Received command: " + cmd_str);
                
                json cmd = json::parse(cmd_str);
                
                json response;
                response["type"] = "response";
                response["command"] = cmd["command"];
                response["sequence"] = cmd["sequence"];
                response["success"] = true;
                response["error"] = nullptr;
                
                if (cmd["type"] == "command") {
                    std::string command = cmd["command"].get<std::string>();
                    
                    if (command == "status") {
                        // Return current status
                        response["data"] = get_status_data();
                    }
                    else if (command == "prepare_trigger") {
                        // Prepare for trigger reception
                        try {
                            log_message("Preparing for trigger reception...", true);
                            
                            // Send sync message to master to indicate we're ready for trigger
                            json sync_msg;
                            sync_msg["type"] = "sync";
                            sync_msg["state"] = "ready_for_trigger";
                            sync_msg["timestamp"] = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::system_clock::now().time_since_epoch()).count();
                            
                            std::string sync_str = sync_msg.dump();
                            zmq::message_t sync_message(sync_str.size());
                            memcpy(sync_message.data(), sync_str.c_str(), sync_str.size());
                            
                            log_message("Sending sync message: " + sync_str);
                            sync_socket_.send(sync_message, zmq::send_flags::none);
                            
                            // Wait a moment to ensure subscription is established
                            std::this_thread::sleep_for(std::chrono::milliseconds(100));
                            
                            log_message("Ready for trigger", true);
                        } catch (const std::exception& e) {
                            response["success"] = false;
                            response["error"] = e.what();
                        }
                    }
                    else if (command == "stop") {
                        // Stop any ongoing acquisition
                        try {
                            send_tc_command("REC:STOP");
                            log_message("Acquisition stopped by master command", true);
                        } catch (const std::exception& e) {
                            response["success"] = false;
                            response["error"] = e.what();
                        }
                    }
                    else if (command == "reset") {
                        // Reset the agent
                        try {
                            send_tc_command("REC:STOP");
                            log_message("Agent reset by master command", true);
                        } catch (const std::exception& e) {
                            response["success"] = false;
                            response["error"] = e.what();
                        }
                    }
                    else {
                        response["success"] = false;
                        response["error"] = "Unknown command: " + command;
                    }
                }
                else {
                    response["success"] = false;
                    response["error"] = "Invalid message type";
                }
                
                // Send response
                std::string response_str = response.dump();
                log_message("Sending response: " + response_str);
                
                zmq::message_t reply(response_str.size());
                memcpy(reply.data(), response_str.c_str(), response_str.size());
                command_socket_.send(reply, zmq::send_flags::none);
            }
        } catch (const zmq::error_t& e) {
            // Ignore EAGAIN errors (no message available)
            if (e.num() != EAGAIN) {
                std::cerr << "ZeroMQ error in command handler: " << e.what() << std::endl;
            }
        } catch (const std::exception& e) {
            std::cerr << "Error in command handler: " << e.what() << std::endl;
            
            // Send error response
            try {
                json response;
                response["type"] = "response";
                response["command"] = "unknown";
                response["sequence"] = 0;
                response["success"] = false;
                response["error"] = e.what();
                
                std::string response_str = response.dump();
                zmq::message_t reply(response_str.size());
                memcpy(reply.data(), response_str.c_str(), response_str.size());
                command_socket_.send(reply, zmq::send_flags::none);
            } catch (...) {
                // Ignore errors in error handling
            }
        }
        
        // Sleep to avoid busy waiting
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    
    log_message("Command handler thread stopped");
}

void SlaveAgent::heartbeat_thread() {
    log_message("Heartbeat thread started");
    
    while (running_) {
        try {
            // Send heartbeat status update
            send_status_update();
            
            // Sleep for heartbeat interval
            std::this_thread::sleep_for(std::chrono::milliseconds(config_.heartbeat_interval_ms));
        } catch (const std::exception& e) {
            std::cerr << "Error in heartbeat thread: " << e.what() << std::endl;
        }
    }
    
    log_message("Heartbeat thread stopped");
}

void SlaveAgent::handle_acquisition(double duration, const std::vector<int>& channels) {
    try {
        // Update status to starting
        current_state_ = "starting";
        send_status_update();
        
        std::stringstream ch_str;
        ch_str << "Starting acquisition for " << duration << " seconds on channels: ";
        for (size_t i = 0; i < channels.size(); ++i) {
            ch_str << channels[i];
            if (i < channels.size() - 1) ch_str << ", ";
        }
        log_message(ch_str.str(), true);
        
        // Configure local Time Controller
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
        current_state_ = "running";
        current_progress_ = 0.0;
        acquisition_start_time_ = std::chrono::steady_clock::now();
        acquisition_duration_ = duration;
        send_status_update();
        
        send_tc_command("REC:PLAY");
        
        // Wait for specified duration
        const int update_interval_ms = 100;
        int total_updates = static_cast<int>(duration * 1000 / update_interval_ms);
        
        for (int i = 0; i < total_updates && running_; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(update_interval_ms));
            
            // Update progress
            current_progress_ = static_cast<double>(i) / total_updates * 100.0;
            send_status_update();
        }
        
        // Stop acquisition
        send_tc_command("REC:STOP");
        
        // Wait for data processing to complete
        std::this_thread::sleep_for(std::chrono::seconds(2));
        
        // Turn off sending for all channels
        for (int ch : channels) {
            send_tc_command("RAW" + std::to_string(ch) + ":SEND OFF");
        }
        
        // Generate output filenames and save data
        if (config_.streaming_mode) {
            // In streaming mode, we'll have multiple files
            for (int i = 0; i < config_.max_files; i++) {
                std::string filename = generate_filename(i);
                
                // Generate dummy data for now - this would be replaced with actual data from TC
                std::vector<uint64_t> timestamps;
                for (int j = 0; j < 1000; j++) {
                    timestamps.push_back(trigger_timestamp_ns_ + j * 1000000);
                }
                
                // Write to binary file
                write_timestamps_to_bin(timestamps, filename);
                
                // Write to text file if enabled
                if (config_.text_output) {
                    std::string text_filename = generate_filename(i, true);
                    write_timestamps_to_txt(timestamps, channels, text_filename);
                }
                
                log_message("Saved slave timestamps to " + filename, true);
                
                // For the first file, transfer partial data for sync
                if (i == 0) {
                    transfer_partial_file(filename, config_.sync_percentage);
                }
                else {
                    // For other files, transfer the whole file
                    transfer_file(filename);
                }
            }
        } else {
            // Single file mode
            auto now = std::chrono::system_clock::now();
            auto now_time_t = std::chrono::system_clock::to_time_t(now);
            std::tm now_tm = *std::localtime(&now_time_t);
            
            char time_str[20];
            std::strftime(time_str, sizeof(time_str), "%Y%m%d_%H%M%S", &now_tm);
            
            std::string output_path = fs::path(config_.output_dir) / ("slave_results_" + std::string(time_str) + ".bin");
            
            // Generate dummy data for now - this would be replaced with actual data from TC
            std::vector<uint64_t> timestamps;
            for (int i = 0; i < 10000; i++) {
                timestamps.push_back(trigger_timestamp_ns_ + i * 1000000);
            }
            
            // Write to binary file
            write_timestamps_to_bin(timestamps, output_path);
            
            // Write to text file if enabled
            if (config_.text_output) {
                std::string text_filename = fs::path(config_.output_dir) / ("slave_results_" + std::string(time_str) + ".txt");
                write_timestamps_to_txt(timestamps, channels, text_filename);
            }
            
            log_message("Acquisition completed. Output file: \"" + output_path + "\"", true);
            
            // Transfer partial file for synchronization
            log_message("Transferring partial file for synchronization");
            transfer_partial_file(output_path, config_.sync_percentage);
        }
        
        // Update status to completed
        current_state_ = "completed";
        current_progress_ = 100.0;
        send_status_update();
        
        log_message("Acquisition and data transfer completed successfully", true);
        
    } catch (const std::exception& e) {
        std::cerr << "Error during acquisition: " << e.what() << std::endl;
        
        // Update status to error
        current_state_ = "error";
        current_error_ = e.what();
        current_progress_ = 0.0;
        send_status_update();
    }
}

void SlaveAgent::send_status_update() {
    try {
        json status = get_status_data();
        
        std::string status_str = status.dump();
        zmq::message_t message(status_str.size());
        memcpy(message.data(), status_str.c_str(), status_str.size());
        
        // Only print status updates occasionally to avoid flooding the console
        if (status_sequence_ % 10 == 0 && config_.verbose_output) {
            log_message("Sending status update: " + status_str);
        }
        
        status_socket_.send(message, zmq::send_flags::dontwait);
    } catch (const std::exception& e) {
        std::cerr << "Error sending status update: " << e.what() << std::endl;
    }
}

json SlaveAgent::get_status_data() {
    json status;
    status["type"] = "status";
    status["sequence"] = status_sequence_++;
    status["timestamp"] = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    status["state"] = current_state_;
    status["trigger_timestamp"] = trigger_timestamp_ns_;
    
    if (current_state_ == "running") {
        status["progress"] = current_progress_;
        
        // Calculate elapsed time
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - acquisition_start_time_).count();
        status["elapsed_ms"] = elapsed;
        
        // Calculate remaining time
        if (acquisition_duration_ > 0) {
            auto total_ms = static_cast<int64_t>(acquisition_duration_ * 1000);
            auto remaining = total_ms - elapsed;
            if (remaining < 0) remaining = 0;
            status["remaining_ms"] = remaining;
        }
    }
    else if (current_state_ == "error") {
        status["error"] = current_error_;
    }
    
    return status;
}

std::string SlaveAgent::send_tc_command(const std::string& cmd) {
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

void SlaveAgent::transfer_file(const std::string& filepath) {
    log_message("Transferring file to master: " + filepath, true);
    
    try {
        // Open file
        std::ifstream file(filepath, std::ios::binary);
        if (!file.is_open()) {
            throw std::runtime_error("Failed to open file for transfer: " + filepath);
        }
        
        // Get file size
        file.seekg(0, std::ios::end);
        size_t file_size = file.tellg();
        file.seekg(0, std::ios::beg);
        
        // Extract filename from path
        fs::path path(filepath);
        std::string filename = path.filename().string();
        
        // Calculate number of chunks
        const size_t chunk_size = 65536;  // 64 KB chunks
        size_t chunks = (file_size + chunk_size - 1) / chunk_size;
        
        // Send file header
        json header;
        header["type"] = "file_header";
        header["filename"] = filename;
        header["size"] = file_size;
        header["chunks"] = chunks;
        header["sequence"] = file_sequence_++;
        
        std::string header_str = header.dump();
        zmq::message_t header_msg(header_str.size());
        memcpy(header_msg.data(), header_str.c_str(), header_str.size());
        
        log_message("Sending file header: " + header_str);
        file_socket_.send(header_msg, zmq::send_flags::none);
        
        // Send file data in chunks
        std::vector<char> buffer(chunk_size);
        for (size_t i = 0; i < chunks; i++) {
            // Read chunk
            size_t bytes_to_read = std::min(chunk_size, file_size - i * chunk_size);
            file.read(buffer.data(), bytes_to_read);
            
            // Send chunk
            zmq::message_t chunk_msg(bytes_to_read);
            memcpy(chunk_msg.data(), buffer.data(), bytes_to_read);
            file_socket_.send(chunk_msg, zmq::send_flags::none);
            
            // Print progress occasionally
            if (chunks > 10 && i % (chunks / 10) == 0 && config_.verbose_output) {
                log_message("File transfer progress: " + std::to_string(i * 100 / chunks) + "%");
            }
            
            // Small delay to avoid overwhelming the receiver
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        
        // Send file footer
        json footer;
        footer["type"] = "file_footer";
        footer["filename"] = filename;
        footer["size"] = file_size;
        footer["chunks"] = chunks;
        footer["sequence"] = file_sequence_++;
        
        std::string footer_str = footer.dump();
        zmq::message_t footer_msg(footer_str.size());
        memcpy(footer_msg.data(), footer_str.c_str(), footer_str.size());
        
        log_message("Sending file footer: " + footer_str);
        file_socket_.send(footer_msg, zmq::send_flags::none);
        
        log_message("File transfer completed: " + filename + " (" + std::to_string(file_size) + " bytes)", true);
        
    } catch (const std::exception& e) {
        std::cerr << "Error transferring file: " << e.what() << std::endl;
    }
}

void SlaveAgent::transfer_partial_file(const std::string& filepath, double percentage) {
    log_message("Transferring partial file to master: " + filepath + " (" + std::to_string(percentage * 100) + "%)", true);
    
    try {
        // Read timestamps from file
        std::vector<uint64_t> timestamps = read_timestamps_from_bin(filepath, percentage);
        
        if (timestamps.empty()) {
            std::cerr << "No timestamps to transfer" << std::endl;
            return;
        }
        
        log_message("Read " + std::to_string(timestamps.size()) + " timestamps for partial transfer");
        
        // Extract filename from path
        fs::path path(filepath);
        std::string filename = path.filename().string();
        
        // Create temporary file with partial data
        fs::path temp_dir = fs::temp_directory_path();
        fs::path temp_file = temp_dir / ("partial_" + filename);
        
        // Write partial data to temporary file
        write_timestamps_to_bin(timestamps, temp_file.string());
        
        // Transfer the temporary file
        transfer_file(temp_file.string());
        
        // Delete temporary file
        fs::remove(temp_file);
        
    } catch (const std::exception& e) {
        std::cerr << "Error transferring partial file: " << e.what() << std::endl;
    }
}

void SlaveAgent::write_timestamps_to_bin(const std::vector<uint64_t>& timestamps, const std::string& filename) {
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

void SlaveAgent::write_timestamps_to_txt(const std::vector<uint64_t>& timestamps, const std::vector<int>& channels, const std::string& filename) {
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
    file << "# Distributed Timestamp System - Slave Agent Data" << std::endl;
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

std::vector<uint64_t> SlaveAgent::read_timestamps_from_bin(const std::string& filename, double percentage) {
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

std::string SlaveAgent::generate_filename(int file_index, bool text_format) {
    auto now = std::chrono::system_clock::now();
    auto now_time_t = std::chrono::system_clock::to_time_t(now);
    std::tm now_tm = *std::localtime(&now_time_t);
    
    char time_str[20];
    std::strftime(time_str, sizeof(time_str), "%Y%m%d_%H%M%S", &now_tm);
    
    std::string extension = text_format ? ".txt" : ".bin";
    std::string filename = "slave_results_" + std::string(time_str) + "_" + 
                          std::to_string(file_index) + extension;
    
    return fs::path(config_.output_dir) / filename;
}
