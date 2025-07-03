#include "slave_agent.hpp"
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

SlaveAgent::SlaveAgent(const SlaveConfig& config) 
    : config_(config), 
      context_(1), 
      running_(false),
      status_sequence_(0),
      file_sequence_(0) {
    
    std::cout << "Initializing Slave Agent..." << std::endl;
    std::cout << "Local Time Controller: " << config_.local_tc_address << std::endl;
    std::cout << "Master address: " << config_.master_address << std::endl;
    if (config_.local_mode) {
        std::cout << "Running in LOCAL MODE (master and slave on same machine)" << std::endl;
    }
}

SlaveAgent::~SlaveAgent() {
    stop();
}

std::string SlaveAgent::get_endpoint(const std::string& address, int port, bool bind) {
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

bool SlaveAgent::initialize() {
    try {
        // Initialize ZeroMQ sockets
        std::cout << "Setting up communication channels..." << std::endl;
        
        // Trigger subscriber
        trigger_socket_ = zmq::socket_t(context_, zmq::socket_type::sub);
        trigger_socket_.connect(get_endpoint(config_.master_address, config_.trigger_port, false));
        trigger_socket_.set(zmq::sockopt::subscribe, "");
        
        // Status sender
        status_socket_ = zmq::socket_t(context_, zmq::socket_type::push);
        status_socket_.connect(get_endpoint(config_.master_address, config_.status_port, false));
        
        // File sender
        file_socket_ = zmq::socket_t(context_, zmq::socket_type::push);
        file_socket_.connect(get_endpoint(config_.master_address, config_.file_port, false));
        
        // Command socket (for receiving commands from master)
        command_socket_ = zmq::socket_t(context_, zmq::socket_type::rep);
        command_socket_.bind(get_endpoint("", config_.command_port, true));
        
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
        
        running_ = true;
        
        // Start worker threads
        trigger_thread_ = std::thread(&SlaveAgent::trigger_listener_thread, this);
        command_thread_ = std::thread(&SlaveAgent::command_handler_thread, this);
        heartbeat_thread_ = std::thread(&SlaveAgent::heartbeat_thread, this);
        
        std::cout << "Slave Agent initialized successfully." << std::endl;
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
    
    while (running_) {
        try {
            zmq::message_t message;
            auto result = trigger_socket_.recv(message, zmq::recv_flags::dontwait);
            
            if (result.has_value()) {
                std::string trigger_str(static_cast<char*>(message.data()), message.size());
                json trigger = json::parse(trigger_str);
                
                if (trigger["type"] == "trigger") {
                    std::cout << "Received trigger command from master" << std::endl;
                    
                    // Extract parameters
                    double duration = trigger["duration"].get<double>();
                    std::vector<int> channels = trigger["channels"].get<std::vector<int>>();
                    
                    // Start acquisition in a separate thread
                    if (acquisition_thread_.joinable()) {
                        acquisition_thread_.join();
                    }
                    
                    acquisition_thread_ = std::thread(&SlaveAgent::handle_acquisition, this, 
                                                     duration, channels);
                }
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
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
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
        
        // Generate output filename
        auto now = std::chrono::system_clock::now();
        auto now_time_t = std::chrono::system_clock::to_time_t(now);
        std::tm now_tm = *std::localtime(&now_time_t);
        
        char time_str[20];
        std::strftime(time_str, sizeof(time_str), "%Y%m%d_%H%M%S", &now_tm);
        
        std::string output_filename = "slave_results_" + std::string(time_str) + ".txt";
        fs::path output_path = fs::path(config_.output_dir) / output_filename;
        
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
        
        // Update status to completed
        current_state_ = "completed";
        current_progress_ = 100.0;
        send_status_update();
        
        std::cout << "Acquisition completed. Output file: \"" << output_path.string() << "\"" << std::endl;
        
        // In local mode, create a dummy file if Time Controller doesn't have input signals
        if (config_.local_mode) {
            // Check if the file exists and has content
            bool file_exists = fs::exists(output_path) && fs::file_size(output_path) > 0;
            
            if (!file_exists) {
                std::cout << "Local mode: Creating dummy output file for testing" << std::endl;
                std::ofstream dummy_file(output_path);
                if (dummy_file.is_open()) {
                    // Write some dummy timestamp data
                    dummy_file << "# Dummy timestamp data for testing local mode" << std::endl;
                    dummy_file << "# Channels: ";
                    for (int ch : channels) {
                        dummy_file << ch << " ";
                    }
                    dummy_file << std::endl;
                    dummy_file << "# Duration: " << duration << " seconds" << std::endl;
                    dummy_file << "# Time: " << time_str << std::endl;
                    dummy_file << "# This is a placeholder file created in local mode" << std::endl;
                    
                    // Add some fake timestamp data
                    for (int i = 0; i < 100; i++) {
                        dummy_file << i * 1000000 << " " << (i % 4) + 1 << std::endl;
                    }
                    
                    dummy_file.close();
                }
            }
        }
        
        // Transfer file to master
        transfer_file(output_path.string());
        
    } catch (const std::exception& e) {
        std::cerr << "Error during acquisition: " << e.what() << std::endl;
        
        // Update status to error
        current_state_ = "error";
        current_error_ = e.what();
        send_status_update();
    }
}

void SlaveAgent::send_status_update() {
    try {
        json status;
        status["type"] = "status";
        status["state"] = current_state_;
        status["timestamp"] = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        status["sequence"] = ++status_sequence_;
        
        if (current_state_ == "running") {
            status["progress"] = current_progress_;
            
            // Calculate elapsed time
            auto now = std::chrono::steady_clock::now();
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                now - acquisition_start_time_).count() / 1000.0;
            
            status["elapsed"] = elapsed;
            status["total"] = acquisition_duration_;
        }
        
        if (!current_error_.empty()) {
            status["error"] = current_error_;
        } else {
            status["error"] = nullptr;
        }
        
        std::string status_str = status.dump();
        zmq::message_t message(status_str.size());
        memcpy(message.data(), status_str.c_str(), status_str.size());
        status_socket_.send(message, zmq::send_flags::none);
    } catch (const std::exception& e) {
        std::cerr << "Error sending status update: " << e.what() << std::endl;
    }
}

nlohmann::json SlaveAgent::get_status_data() {
    json status;
    status["state"] = current_state_;
    
    if (current_state_ == "running") {
        status["progress"] = current_progress_;
        
        // Calculate elapsed time
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - acquisition_start_time_).count() / 1000.0;
        
        status["elapsed"] = elapsed;
        status["total"] = acquisition_duration_;
    }
    
    if (!current_error_.empty()) {
        status["error"] = current_error_;
    } else {
        status["error"] = nullptr;
    }
    
    return status;
}

void SlaveAgent::transfer_file(const std::string& filepath) {
    try {
        std::cout << "Transferring file to master: " << filepath << std::endl;
        
        // Special handling for local mode
        if (config_.local_mode) {
            // In local mode, we can just copy the file directly if needed
            fs::path src_path(filepath);
            if (!fs::exists(src_path)) {
                std::cerr << "Error transferring file: File does not exist: " << filepath << std::endl;
                return;
            }
            
            // For testing, we'll still use the ZMQ transfer mechanism
            std::cout << "Local mode: Using ZMQ for file transfer (could optimize with direct copy)" << std::endl;
        }
        
        // Open the file
        std::ifstream file(filepath, std::ios::binary);
        if (!file.is_open()) {
            std::cerr << "Error transferring file: Failed to open file for transfer: " << filepath << std::endl;
            return;
        }
        
        // Get file size
        file.seekg(0, std::ios::end);
        size_t file_size = file.tellg();
        file.seekg(0, std::ios::beg);
        
        // Calculate number of chunks
        const size_t chunk_size = 65536;  // 64 KB chunks
        size_t num_chunks = (file_size + chunk_size - 1) / chunk_size;
        
        // Extract filename from path
        fs::path path(filepath);
        std::string filename = path.filename().string();
        
        // Send file header
        json header;
        header["type"] = "file_header";
        header["filename"] = filename;
        header["size"] = file_size;
        header["chunks"] = num_chunks;
        header["checksum"] = "dummy_checksum";  // TODO: Implement real checksum
        header["sequence"] = ++file_sequence_;
        
        std::string header_str = header.dump();
        zmq::message_t header_msg(header_str.size());
        memcpy(header_msg.data(), header_str.c_str(), header_str.size());
        file_socket_.send(header_msg, zmq::send_flags::none);
        
        // Send file data in chunks
        std::vector<char> buffer(chunk_size);
        size_t chunks_sent = 0;
        
        while (file && chunks_sent < num_chunks) {
            // Read chunk
            file.read(buffer.data(), chunk_size);
            std::streamsize bytes_read = file.gcount();
            
            // Send chunk
            zmq::message_t chunk(bytes_read);
            memcpy(chunk.data(), buffer.data(), bytes_read);
            file_socket_.send(chunk, zmq::send_flags::none);
            
            chunks_sent++;
            
            // Periodically report progress
            if (chunks_sent % 10 == 0 || chunks_sent == num_chunks) {
                std::cout << "File transfer progress: " << chunks_sent << "/" 
                          << num_chunks << " chunks (" 
                          << (chunks_sent * 100 / num_chunks) << "%)" << std::endl;
            }
            
            // Small delay to avoid overwhelming the receiver
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        
        // Send file footer
        json footer;
        footer["type"] = "file_footer";
        footer["filename"] = filename;
        footer["chunks_sent"] = chunks_sent;
        footer["sequence"] = file_sequence_;
        
        std::string footer_str = footer.dump();
        zmq::message_t footer_msg(footer_str.size());
        memcpy(footer_msg.data(), footer_str.c_str(), footer_str.size());
        file_socket_.send(footer_msg, zmq::send_flags::none);
        
        std::cout << "File transfer completed: " << filename << " (" 
                  << chunks_sent << "/" << num_chunks << " chunks)" << std::endl;
    } catch (const std::exception& e) {
        std::cerr << "Error transferring file: " << e.what() << std::endl;
    }
}

std::string SlaveAgent::send_tc_command(const std::string& cmd) {
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
