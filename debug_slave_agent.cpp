#include "enhanced_slave_agent.hpp"
#include <iostream>
#include <fstream>
#include <chrono>
#include <thread>
#include <filesystem>
#include <cstring>
#include <iomanip>
#include <sstream>

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
    
    std::cout << "Initializing Slave Agent..." << std::endl;
    std::cout << "Local Time Controller: " << config_.local_tc_address << std::endl;
    std::cout << "Master address: " << config_.master_address << std::endl;
}

SlaveAgent::~SlaveAgent() {
    stop();
}

bool SlaveAgent::initialize() {
    try {
        // Initialize ZeroMQ sockets
        std::cout << "Setting up communication channels..." << std::endl;
        
        // Trigger subscriber
        trigger_socket_ = zmq::socket_t(context_, zmq::socket_type::sub);
        std::string trigger_endpoint = "tcp://" + config_.master_address + ":" + std::to_string(config_.trigger_port);
        std::cout << "DEBUG: Connecting trigger socket to " << trigger_endpoint << std::endl;
        trigger_socket_.connect(trigger_endpoint);
        trigger_socket_.set(zmq::sockopt::subscribe, "");
        std::cout << "DEBUG: Subscribed to all messages on trigger socket" << std::endl;
        
        // Status sender
        status_socket_ = zmq::socket_t(context_, zmq::socket_type::push);
        std::string status_endpoint = "tcp://" + config_.master_address + ":" + std::to_string(config_.status_port);
        std::cout << "DEBUG: Connecting status socket to " << status_endpoint << std::endl;
        status_socket_.connect(status_endpoint);
        
        // File sender
        file_socket_ = zmq::socket_t(context_, zmq::socket_type::push);
        std::string file_endpoint = "tcp://" + config_.master_address + ":" + std::to_string(config_.file_port);
        std::cout << "DEBUG: Connecting file socket to " << file_endpoint << std::endl;
        file_socket_.connect(file_endpoint);
        
        // Command socket (for receiving commands from master)
        command_socket_ = zmq::socket_t(context_, zmq::socket_type::rep);
        std::string command_endpoint = "tcp://*:" + std::to_string(config_.command_port);
        std::cout << "DEBUG: Binding command socket to " << command_endpoint << std::endl;
        command_socket_.bind(command_endpoint);
        
        // Set socket options for low latency
        int linger = 0;
        trigger_socket_.set(zmq::sockopt::linger, linger);
        status_socket_.set(zmq::sockopt::linger, linger);
        file_socket_.set(zmq::sockopt::linger, linger);
        command_socket_.set(zmq::sockopt::linger, linger);
        
        // Set high water mark for file transfer
        int hwm = 1000;
        file_socket_.set(zmq::sockopt::sndhwm, hwm);
        
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
        
        running_ = true;
        
        // Start worker threads
        trigger_thread_ = std::thread(&SlaveAgent::trigger_listener_thread, this);
        command_thread_ = std::thread(&SlaveAgent::command_handler_thread, this);
        heartbeat_thread_ = std::thread(&SlaveAgent::heartbeat_thread, this);
        
        std::cout << "Slave Agent initialized successfully." << std::endl;
        std::cout << "Slave agent initialized and waiting for trigger commands..." << std::endl;
        std::cout << "Press Ctrl+C to stop" << std::endl;
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
        local_tc_socket_.close();
        
        std::cout << "Slave Agent stopped." << std::endl;
    }
}

void SlaveAgent::trigger_listener_thread() {
    std::cout << "Trigger listener thread started" << std::endl;
    
    // Set a shorter timeout for more responsive trigger detection
    int timeout = 100; // 100ms
    trigger_socket_.set(zmq::sockopt::rcvtimeo, timeout);
    
    while (running_) {
        try {
            std::cout << "DEBUG: Waiting for trigger message..." << std::endl;
            
            zmq::message_t message;
            auto result = trigger_socket_.recv(message);
            
            if (result.has_value()) {
                std::string trigger_str(static_cast<char*>(message.data()), message.size());
                std::cout << "DEBUG: Received message: " << trigger_str << std::endl;
                
                try {
                    json trigger = json::parse(trigger_str);
                    
                    if (trigger["type"] == "trigger") {
                        std::cout << "Received trigger command from master" << std::endl;
                        
                        // Record trigger reception timestamp
                        trigger_timestamp_ns_ = std::chrono::duration_cast<std::chrono::nanoseconds>(
                            std::chrono::system_clock::now().time_since_epoch()).count();
                        
                        // Extract parameters
                        double duration = trigger["duration"].get<double>();
                        std::vector<int> channels = trigger["channels"].get<std::vector<int>>();
                        
                        std::cout << "DEBUG: Trigger parameters - duration: " << duration 
                                  << ", channels: ";
                        for (int ch : channels) {
                            std::cout << ch << " ";
                        }
                        std::cout << std::endl;
                        
                        // Extract master trigger timestamp if present
                        if (trigger.contains("timestamp")) {
                            uint64_t master_timestamp = trigger["timestamp"].get<uint64_t>();
                            std::cout << "Master trigger timestamp: " << master_timestamp << " ns" << std::endl;
                            std::cout << "Slave trigger timestamp: " << trigger_timestamp_ns_ << " ns" << std::endl;
                            std::cout << "Estimated offset: " << (trigger_timestamp_ns_ - master_timestamp) << " ns" << std::endl;
                        }
                        
                        // Start acquisition in a separate thread
                        if (acquisition_thread_.joinable()) {
                            acquisition_thread_.join();
                        }
                        
                        std::cout << "DEBUG: Starting acquisition thread" << std::endl;
                        acquisition_thread_ = std::thread(&SlaveAgent::handle_acquisition, this, 
                                                         duration, channels);
                    } else {
                        std::cout << "DEBUG: Received non-trigger message type: " << trigger["type"].get<std::string>() << std::endl;
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
    
    std::cout << "Trigger listener thread stopped" << std::endl;
}

void SlaveAgent::command_handler_thread() {
    std::cout << "Command handler thread started" << std::endl;
    
    while (running_) {
        try {
            zmq::message_t message;
            auto result = command_socket_.recv(message, zmq::recv_flags::dontwait);
            
            if (result.has_value()) {
                std::string cmd_str(static_cast<char*>(message.data()), message.size());
                std::cout << "DEBUG: Received command: " << cmd_str << std::endl;
                
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
                    else if (command == "stop") {
                        // Stop any ongoing acquisition
                        try {
                            send_tc_command("REC:STOP");
                            std::cout << "Acquisition stopped by master command" << std::endl;
                        } catch (const std::exception& e) {
                            response["success"] = false;
                            response["error"] = e.what();
                        }
                    }
                    else if (command == "reset") {
                        // Reset the agent
                        try {
                            send_tc_command("REC:STOP");
                            std::cout << "Agent reset by master command" << std::endl;
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
                std::cout << "DEBUG: Sending response: " << response_str << std::endl;
                
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
    
    std::cout << "Command handler thread stopped" << std::endl;
}

void SlaveAgent::heartbeat_thread() {
    std::cout << "Heartbeat thread started" << std::endl;
    
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
    
    std::cout << "Heartbeat thread stopped" << std::endl;
}

void SlaveAgent::handle_acquisition(double duration, const std::vector<int>& channels) {
    try {
        // Update status to starting
        current_state_ = "starting";
        send_status_update();
        
        std::cout << "Starting acquisition for " << duration << " seconds on channels: ";
        for (int ch : channels) {
            std::cout << ch << " ";
        }
        std::cout << std::endl;
        
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
                
                std::cout << "Saved slave timestamps to " << filename << std::endl;
                
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
            
            std::cout << "Acquisition completed. Output file: \"" << output_path << "\"" << std::endl;
            
            // Transfer partial file for synchronization
            std::cout << "DEBUG: Transferring partial file for synchronization" << std::endl;
            transfer_partial_file(output_path, config_.sync_percentage);
        }
        
        // Update status to completed
        current_state_ = "completed";
        current_progress_ = 100.0;
        send_status_update();
        
        std::cout << "DEBUG: Acquisition and data transfer completed successfully" << std::endl;
        
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
        if (status_sequence_ % 10 == 0) {
            std::cout << "DEBUG: Sending status update: " << status_str << std::endl;
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
    std::cout << "Transferring file to master: " << filepath << std::endl;
    
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
        
        std::cout << "DEBUG: Sending file header: " << header_str << std::endl;
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
            if (chunks > 10 && i % (chunks / 10) == 0) {
                std::cout << "DEBUG: File transfer progress: " << (i * 100 / chunks) << "%" << std::endl;
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
        
        std::cout << "DEBUG: Sending file footer: " << footer_str << std::endl;
        file_socket_.send(footer_msg, zmq::send_flags::none);
        
        std::cout << "File transfer completed: " << filename << " (" << file_size << " bytes)" << std::endl;
        
    } catch (const std::exception& e) {
        std::cerr << "Error transferring file: " << e.what() << std::endl;
    }
}

void SlaveAgent::transfer_partial_file(const std::string& filepath, double percentage) {
    std::cout << "Transferring partial file to master: " << filepath << " (" << (percentage * 100) << "%)" << std::endl;
    
    try {
        // Read timestamps from file
        std::vector<uint64_t> timestamps = read_timestamps_from_bin(filepath, percentage);
        
        if (timestamps.empty()) {
            std::cerr << "No timestamps to transfer" << std::endl;
            return;
        }
        
        std::cout << "DEBUG: Read " << timestamps.size() << " timestamps for partial transfer" << std::endl;
        
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
    
    std::cout << "DEBUG: Wrote " << count << " timestamps to " << filename << std::endl;
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
    
    std::cout << "DEBUG: Read " << read_count << " of " << count << " timestamps from " << filename << std::endl;
    
    return timestamps;
}

std::string SlaveAgent::generate_filename(int file_index) {
    auto now = std::chrono::system_clock::now();
    auto now_time_t = std::chrono::system_clock::to_time_t(now);
    std::tm now_tm = *std::localtime(&now_time_t);
    
    char time_str[20];
    std::strftime(time_str, sizeof(time_str), "%Y%m%d_%H%M%S", &now_tm);
    
    std::string filename = "slave_results_" + std::string(time_str) + "_" + 
                          std::to_string(file_index) + ".bin";
    
    return fs::path(config_.output_dir) / filename;
}
