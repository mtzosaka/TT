#include "fixed_slave_agent.hpp"
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
#include <map>
#include <algorithm>
#include "streams.hpp"
#include "common.hpp"

using json = nlohmann::json;
namespace fs = std::filesystem;

// --- Helper function to read timestamps from binary file (moved here for clarity) ---
std::vector<uint64_t> read_timestamps_from_bin_internal(const std::string& filename, double percentage = 1.0) {
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
    if (read_count < 1 && count > 0) read_count = 1; // Read at least one if available
    if (read_count > count) read_count = count;
    
    // Read timestamps
    if (read_count > 0) {
        timestamps.resize(read_count);
        file.read(reinterpret_cast<char*>(timestamps.data()), read_count * sizeof(uint64_t));
    }
    
    file.close();
    
    std::cout << "Read " << read_count << " of " << count << " timestamps from " << filename << std::endl;
    
    return timestamps;
}
// --- End Helper function ---

SlaveAgent::SlaveAgent(const SlaveConfig& config) 
    : config_(config), 
      context_(1), 
      running_(false),
      acquisition_active_(false),
      current_state_("idle"),
      current_progress_(0.0),
      trigger_timestamp_ns_(0),
      command_sequence_(0) {
    
    log_message("Initializing Slave Agent...", true);
    log_message("Local Time Controller: " + config_.local_tc_address, true);
    log_message("Master address: " + config_.master_address, true);
}

SlaveAgent::~SlaveAgent() {
    stop();
}

void SlaveAgent::log_message(const std::string& message, bool always_show) {
    if (always_show || config_.verbose_output) {
        std::cout << "[Slave] " << message << std::endl;
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
        // Use the ZMQ context defined within the class
        local_tc_socket_ = zmq::socket_t(context_, zmq::socket_type::req);
        std::string tc_endpoint = "tcp://" + config_.local_tc_address + ":" + std::to_string(SCPI_PORT); // Use SCPI_PORT from common.hpp
        log_message("Connecting to Time Controller at " + tc_endpoint);
        local_tc_socket_.connect(tc_endpoint);
        
        // Test connection to local Time Controller
        std::string idn = zmq_exec(local_tc_socket_, "*IDN?"); // Use helper from common.hpp
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
            log_message("Trigger listener thread stopped");
        }
        if (command_thread_.joinable()) {
            command_thread_.join();
            log_message("Command handler thread stopped");
        }
        if (heartbeat_thread_.joinable()) {
            heartbeat_thread_.join();
            log_message("Heartbeat thread stopped");
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

// --- Trigger Listener Thread (updated for subscription synchronization) --- 
void SlaveAgent::trigger_listener_thread() {
    log_message("Trigger listener thread started");
    
    // Send ready message to master to confirm subscription is established
    std::this_thread::sleep_for(std::chrono::milliseconds(500)); // Wait for connection to establish
    
    std::string ready_msg = "ready_for_trigger";
    zmq::message_t ready_message(ready_msg.size());
    memcpy(ready_message.data(), ready_msg.c_str(), ready_msg.size());
    
    log_message("Sending ready_for_trigger message to master");
    sync_socket_.send(ready_message, zmq::send_flags::none);
    
    while (running_) {
        try {
            zmq::message_t message;
            auto result = trigger_socket_.recv(message, zmq::recv_flags::dontwait);
            if (result.has_value()) {
                std::string trigger_str(static_cast<char*>(message.data()), message.size());
                log_message("Received trigger message: " + trigger_str);
                
                try {
                    json trigger = json::parse(trigger_str);
                    if (trigger["type"] == "trigger") {
                        // Extract trigger parameters
                        trigger_timestamp_ns_ = trigger["timestamp"].get<uint64_t>();
                        double duration = trigger["duration"].get<double>();
                        std::vector<int> channels = trigger["channels"].get<std::vector<int>>();
                        
                        log_message("Received trigger command. Duration: " + std::to_string(duration) + 
                                   " seconds, Channels: " + std::to_string(channels.size()), true);
                        
                        // Update state
                        current_state_ = "triggered";
                        
                        // Send status update
                        json status;
                        status["type"] = "status";
                        status["state"] = "triggered";
                        status["trigger_timestamp"] = trigger_timestamp_ns_;
                        
                        std::string status_str = status.dump();
                        zmq::message_t status_message(status_str.size());
                        memcpy(status_message.data(), status_str.c_str(), status_str.size());
                        status_socket_.send(status_message, zmq::send_flags::none);
                        
                        // Start acquisition
                        start_acquisition(duration, channels);
                    }
                } catch (const json::parse_error& e) {
                    std::cerr << "Error parsing trigger message: " << e.what() << std::endl;
                }
            }
        } catch (const zmq::error_t& e) {
            if (e.num() != EAGAIN) {
                std::cerr << "ZeroMQ error in trigger listener: " << e.what() << std::endl;
            }
        } catch (const std::exception& e) {
            std::cerr << "Error in trigger listener: " << e.what() << std::endl;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

// --- Command Handler Thread (mostly unchanged) --- 
void SlaveAgent::command_handler_thread() {
    log_message("Command handler thread started");
    while (running_) {
        try {
            zmq::message_t message;
            auto result = command_socket_.recv(message, zmq::recv_flags::dontwait);
            if (result.has_value()) {
                std::string cmd_str(static_cast<char*>(message.data()), message.size());
                log_message("Received command message: " + cmd_str);
                
                try {
                    json cmd = json::parse(cmd_str);
                    if (cmd["type"] == "command") {
                        std::string command = cmd["command"].get<std::string>();
                        
                        // Prepare response
                        json response;
                        response["success"] = true;
                        
                        if (command == "status") {
                            response["state"] = current_state_;
                            response["progress"] = current_progress_;
                            if (!current_error_.empty()) {
                                response["error"] = current_error_;
                            }
                        } else if (command == "prepare_trigger") {
                            log_message("Preparing for trigger...");
                            // Any pre-trigger setup can be done here
                        } else {
                            response["success"] = false;
                            response["error"] = "Unknown command: " + command;
                        }
                        
                        // Send response
                        std::string response_str = response.dump();
                        zmq::message_t response_message(response_str.size());
                        memcpy(response_message.data(), response_str.c_str(), response_str.size());
                        command_socket_.send(response_message, zmq::send_flags::none);
                    }
                } catch (const json::parse_error& e) {
                    std::cerr << "Error parsing command message: " << e.what() << std::endl;
                    
                    // Send error response
                    json error_response;
                    error_response["success"] = false;
                    error_response["error"] = "Invalid command format";
                    
                    std::string error_str = error_response.dump();
                    zmq::message_t error_message(error_str.size());
                    memcpy(error_message.data(), error_str.c_str(), error_str.size());
                    command_socket_.send(error_message, zmq::send_flags::none);
                }
            }
        } catch (const zmq::error_t& e) {
            if (e.num() != EAGAIN) {
                std::cerr << "ZeroMQ error in command handler: " << e.what() << std::endl;
            }
        } catch (const std::exception& e) {
            std::cerr << "Error in command handler: " << e.what() << std::endl;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

// --- Heartbeat Thread (mostly unchanged) --- 
void SlaveAgent::heartbeat_thread() {
    log_message("Heartbeat thread started");
    while (running_) {
        try {
            // Send heartbeat message
            json heartbeat;
            heartbeat["type"] = "heartbeat";
            heartbeat["state"] = current_state_;
            heartbeat["timestamp"] = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
            
            std::string heartbeat_str = heartbeat.dump();
            zmq::message_t message(heartbeat_str.size());
            memcpy(message.data(), heartbeat_str.c_str(), heartbeat_str.size());
            status_socket_.send(message, zmq::send_flags::none);
            
        } catch (const std::exception& e) {
            std::cerr << "Error in heartbeat thread: " << e.what() << std::endl;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(config_.heartbeat_interval_ms));
    }
}

// --- Start Acquisition (updated for real data collection) --- 
bool SlaveAgent::start_acquisition(double duration, const std::vector<int>& channels) {
    if (acquisition_active_) {
        std::cerr << "Acquisition already in progress" << std::endl;
        return false;
    }
    
    std::vector<BufferStreamClient*> stream_clients;
    TimestampsMergerThread* merger = nullptr;
    std::map<int, std::string> acquisitions_id; // For DLT interaction if needed
    zmq::socket_t dlt_socket(context_, zmq::socket_type::req); // DLT socket if needed
    bool dlt_connected = false;
    
    try {
        // Store active channels for text output
        active_channels_ = channels;
        
        // Update state
        current_state_ = "running";
        current_progress_ = 0.0;
        acquisition_active_ = true;
        acquisition_start_time_ = std::chrono::steady_clock::now();
        
        // --- Start Local Acquisition (Real Data) --- 
        log_message("Starting local acquisition...", true);

        // Connect to local DLT (assuming it's running or needs launching)
        // This part needs adaptation based on whether DLT is required/available
        // For now, we'll bypass DLT and assume direct streaming if possible,
        // or implement a simplified streaming mechanism.
        // If DLT is essential, the dlt_connect logic from common.cpp needs integration.
        log_message("Connecting to local DLT (or preparing direct stream)...");
        // dlt_socket = dlt_connect(fs::path(config_.output_dir)); // Example if DLT is used
        // dlt_connected = true;
        // close_active_acquisitions(dlt_socket); // Clean up previous DLT state

        // Configure TC
        configure_timestamps_references(local_tc_socket_, channels);
        long long pwid_ps = static_cast<long long>(1e12 * config_.sub_duration);
        long long pper_ps = static_cast<long long>(1e12 * (config_.sub_duration + 40e-9));
        zmq_exec(local_tc_socket_, "REC:TRIG:ARM:MODE MANUal");
        zmq_exec(local_tc_socket_, "REC:ENABle ON");
        zmq_exec(local_tc_socket_, "REC:STOP");
        zmq_exec(local_tc_socket_, "REC:NUM INF");
        zmq_exec(local_tc_socket_, "REC:PWID " + std::to_string(pwid_ps) + ";PPER " + std::to_string(pper_ps));

        // Open stream clients for each channel
        log_message("Opening stream clients for local channels...");
        for (int ch : channels) {
            zmq_exec(local_tc_socket_, "RAW" + std::to_string(ch) + ":ERRORS:CLEAR");
            BufferStreamClient* client = new BufferStreamClient(ch); // Uses ports 4241+ch
            stream_clients.push_back(client);
            client->start();
            
            // If using DLT:
            // std::string cmd = "start-stream --address " + config_.local_tc_address +
            //                   " --channel " + std::to_string(ch) +
            //                   " --stream-port " + std::to_string(client->port);
            // json dlt_response = dlt_exec(dlt_socket, cmd);
            // if (dlt_response.contains("id")) {
            //     acquisitions_id[ch] = dlt_response["id"].get<std::string>();
            // }
            
            // Tell TC to send data
            zmq_exec(local_tc_socket_, "RAW" + std::to_string(ch) + ":SEND ON");
        }

        // Start the merger thread
        fs::path slave_output_base = fs::path(config_.output_dir) / ("slave_results_" + get_current_timestamp_str());
        std::string slave_output_file = slave_output_base.string() + ".txt"; // Merger writes text directly
        log_message("Starting merger thread, output to: " + slave_output_file);
        merger = new TimestampsMergerThread(stream_clients, slave_output_file, static_cast<uint64_t>(pper_ps));
        merger->start();

        // Start TC recording
        zmq_exec(local_tc_socket_, "REC:PLAY");
        log_message("Acquisition in progress for " + std::to_string(duration) + " seconds...", true);
        
        // Wait for specified duration (with progress updates)
        const int update_interval_ms = 100;
        int total_updates = static_cast<int>(duration * 1000 / update_interval_ms);
        for (int i = 0; i < total_updates && running_ && acquisition_active_; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(update_interval_ms));
            current_progress_ = static_cast<double>(i) / total_updates * 100.0;
            
            // Send status update every second
            if (i % 10 == 0) {
                json status;
                status["type"] = "status";
                status["state"] = "running";
                status["progress"] = current_progress_;
                
                std::string status_str = status.dump();
                zmq::message_t status_message(status_str.size());
                memcpy(status_message.data(), status_str.c_str(), status_str.size());
                status_socket_.send(status_message, zmq::send_flags::none);
            }
        }
        
        // Stop TC recording
        log_message("Stopping local acquisition...", true);
        zmq_exec(local_tc_socket_, "REC:STOP");
        
        // Wait for end of acquisition (adapt from common.cpp if using DLT)
        log_message("Waiting for data processing to complete...", true);
        // wait_end_of_timestamps_acquisition(local_tc_socket_, dlt_socket, acquisitions_id); // If using DLT
        std::this_thread::sleep_for(std::chrono::seconds(5)); // Simple wait otherwise

        // Close acquisitions (adapt from common.cpp if using DLT)
        // bool success = close_timestamps_acquisition(local_tc_socket_, dlt_socket, acquisitions_id); // If using DLT
        bool success = true; // Assume success if not using DLT error checking
        for (int ch : channels) { // Ensure SEND OFF is called
             zmq_exec(local_tc_socket_, "RAW" + std::to_string(ch) + ":SEND OFF");
        }

        // Stop stream clients and merger
        log_message("Stopping stream clients and merger...");
        for (BufferStreamClient* client : stream_clients) {
            client->join();
        }
        if (merger) {
             merger->join();
        }

        log_message("Local acquisition completed.", true);
        
        // --- Post-processing --- 
        // Convert merged text file to binary format if needed (or keep text)
        // The merger now writes text directly. If binary is needed:
        if (!config_.text_output) { // If only binary is desired
             std::string bin_filename = slave_output_base.string() + ".bin";
             log_message("Converting merged text file to binary: " + bin_filename);
             convert_text_to_binary(slave_output_file, bin_filename);
             // Optionally remove the text file if only binary is needed
             // fs::remove(slave_output_file);
        } else {
             log_message("Merged timestamps saved to text file: " + slave_output_file);
             // Optionally create binary version as well
             std::string bin_filename = slave_output_base.string() + ".bin";
             convert_text_to_binary(slave_output_file, bin_filename);
        }
        
        // Save slave results file path
        std::string bin_filename = slave_output_base.string() + ".bin";
        log_message("Saved slave timestamps to " + bin_filename, true);
        
        // Send first portion of data to master for synchronization
        log_message("Sending first " + std::to_string(config_.sync_percentage * 100) + "% of data to master for synchronization...");
        send_file_to_master(bin_filename);
        
        // Update state
        acquisition_active_ = false;
        current_state_ = "completed";
        current_progress_ = 100.0;
        
        // Send final status update
        json status;
        status["type"] = "status";
        status["state"] = "completed";
        status["progress"] = 100.0;
        
        std::string status_str = status.dump();
        zmq::message_t status_message(status_str.size());
        memcpy(status_message.data(), status_str.c_str(), status_str.size());
        status_socket_.send(status_message, zmq::send_flags::none);
        
        log_message("Acquisition completed successfully.", true);
        
        return success;
        
    } catch (const std::exception& e) {
        std::cerr << "Error during acquisition: " << e.what() << std::endl;
        current_state_ = "error";
        current_error_ = e.what();
        acquisition_active_ = false;
        // Cleanup resources in case of error
        if (merger) delete merger;
        for (BufferStreamClient* client : stream_clients) delete client;
        if (dlt_connected) dlt_socket.close();
        return false;
    }
    
    // Cleanup allocated resources
    if (merger) delete merger;
    for (BufferStreamClient* client : stream_clients) delete client;
    if (dlt_connected) dlt_socket.close();
    
    return true;
}

// --- Send File to Master --- 
bool SlaveAgent::send_file_to_master(const std::string& filename) {
    try {
        // Open file
        std::ifstream file(filename, std::ios::binary);
        if (!file.is_open()) {
            std::cerr << "Failed to open file for sending: " << filename << std::endl;
            return false;
        }
        
        // Get file size
        file.seekg(0, std::ios::end);
        size_t file_size = file.tellg();
        file.seekg(0, std::ios::beg);
        
        // Extract filename from path
        fs::path file_path(filename);
        std::string base_filename = file_path.filename().string();
        
        // Calculate number of chunks
        const size_t chunk_size = 1024 * 1024; // 1MB chunks
        size_t chunks = (file_size + chunk_size - 1) / chunk_size;
        
        log_message("Sending file to master: " + base_filename + " (" + std::to_string(file_size) + " bytes in " + std::to_string(chunks) + " chunks)");
        
        // Send file header
        json header;
        header["type"] = "file_header";
        header["filename"] = base_filename;
        header["size"] = file_size;
        header["chunks"] = chunks;
        
        std::string header_str = header.dump();
        zmq::message_t header_message(header_str.size());
        memcpy(header_message.data(), header_str.c_str(), header_str.size());
        file_socket_.send(header_message, zmq::send_flags::none);
        
        // Send file data in chunks
        std::vector<char> buffer(chunk_size);
        size_t sent_chunks = 0;
        while (file && sent_chunks < chunks) {
            size_t bytes_to_read = std::min(chunk_size, file_size - sent_chunks * chunk_size);
            file.read(buffer.data(), bytes_to_read);
            size_t bytes_read = file.gcount();
            
            zmq::message_t data_message(bytes_read);
            memcpy(data_message.data(), buffer.data(), bytes_read);
            file_socket_.send(data_message, zmq::send_flags::none);
            
            sent_chunks++;
            
            if (chunks > 10 && sent_chunks % (chunks / 10) == 0) {
                double progress = static_cast<double>(sent_chunks) / chunks * 100.0;
                log_message("File transfer progress: " + std::to_string(progress) + "%");
            }
        }
        
        file.close();
        
        // Send file footer
        json footer;
        footer["type"] = "file_footer";
        footer["filename"] = base_filename;
        footer["chunks_sent"] = sent_chunks;
        
        std::string footer_str = footer.dump();
        zmq::message_t footer_message(footer_str.size());
        memcpy(footer_message.data(), footer_str.c_str(), footer_str.size());
        file_socket_.send(footer_message, zmq::send_flags::none);
        
        log_message("File transfer completed: " + base_filename);
        
        return true;
        
    } catch (const std::exception& e) {
        std::cerr << "Error sending file to master: " << e.what() << std::endl;
        return false;
    }
}

// --- Send TC Command (uses zmq_exec from common.hpp) --- 
std::string SlaveAgent::send_tc_command(const std::string& cmd) {
    try {
        return zmq_exec(local_tc_socket_, cmd);
    } catch (const std::exception& e) {
        return "ERROR: " + std::string(e.what());
    }
}

// --- Write Timestamps to Binary --- 
void SlaveAgent::write_timestamps_to_bin(const std::vector<uint64_t>& timestamps, const std::string& filename) {
    std::ofstream file(filename, std::ios::binary);
    if (!file.is_open()) {
        std::cerr << "Failed to open file for writing: " << filename << std::endl;
        return;
    }
    uint64_t count = timestamps.size();
    file.write(reinterpret_cast<char*>(&count), sizeof(count));
    if (count > 0) {
        file.write(reinterpret_cast<const char*>(timestamps.data()), count * sizeof(uint64_t));
    }
    file.close();
    log_message("Wrote " + std::to_string(count) + " timestamps to binary file " + filename);
}

// --- Write Timestamps to Text --- 
void SlaveAgent::write_timestamps_to_txt(const std::vector<uint64_t>& timestamps, const std::vector<int>& channels, const std::string& filename) {
    std::ofstream file(filename);
    if (!file.is_open()) {
        std::cerr << "Failed to open text file for writing: " << filename << std::endl;
        return;
    }
    
    auto now = std::chrono::system_clock::now();
    std::time_t now_time_t = std::chrono::system_clock::to_time_t(now);
    std::tm* now_tm = std::localtime(&now_time_t);
    
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
    for (size_t i = 0; i < timestamps.size(); ++i) {
        int channel = (channels.empty()) ? 0 : channels[i % channels.size()]; // Handle empty channels case
        file << i << ", " << timestamps[i] << ", " << channel << std::endl;
    }
    file.close();
    log_message("Wrote " + std::to_string(timestamps.size()) + " timestamps to text file " + filename);
}

// --- Generate Filename (mostly unchanged) --- 
std::string SlaveAgent::generate_filename(bool is_master, int file_index, bool text_format) {
    std::string timestamp_str = get_current_timestamp_str();
    std::string prefix = is_master ? "master" : "slave";
    std::string extension = text_format ? ".txt" : ".bin";
    std::string filename = prefix + "_results_" + timestamp_str;
    if (config_.streaming_mode) {
         filename += "_" + std::to_string(file_index);
    }
    filename += extension;
    return (fs::path(config_.output_dir) / filename).string(); // Return string representation
}

// --- Get Current Timestamp String --- 
std::string SlaveAgent::get_current_timestamp_str() {
     auto now = std::chrono::system_clock::now();
     auto now_time_t = std::chrono::system_clock::to_time_t(now);
     std::tm now_tm = *std::localtime(&now_time_t);
     char time_str[20];
     std::strftime(time_str, sizeof(time_str), "%Y%m%d_%H%M%S", &now_tm);
     return std::string(time_str);
}

// --- Convert Text Timestamps to Binary --- 
void SlaveAgent::convert_text_to_binary(const std::string& text_filename, const std::string& bin_filename) {
    std::ifstream text_file(text_filename);
    if (!text_file.is_open()) {
        std::cerr << "Failed to open text file for conversion: " << text_filename << std::endl;
        return;
    }

    std::vector<uint64_t> timestamps;
    std::string line;
    while (std::getline(text_file, line)) {
        if (line.empty() || line[0] == '#') continue; // Skip comments and empty lines
        
        std::stringstream ss(line);
        std::string segment;
        std::vector<std::string> seglist;
        
        while(std::getline(ss, segment, ',')) {
           seglist.push_back(segment);
        }
        
        if (seglist.size() >= 2) { // Need at least index and timestamp
            try {
                // Trim whitespace from timestamp string
                std::string ts_str = seglist[1];
                ts_str.erase(0, ts_str.find_first_not_of(" \t\n\r\f\v"));
                ts_str.erase(ts_str.find_last_not_of(" \t\n\r\f\v") + 1);
                timestamps.push_back(std::stoull(ts_str));
            } catch (const std::invalid_argument& ia) {
                std::cerr << "Invalid timestamp format in line: " << line << std::endl;
            } catch (const std::out_of_range& oor) {
                std::cerr << "Timestamp out of range in line: " << line << std::endl;
            }
        }
    }
    text_file.close();

    write_timestamps_to_bin(timestamps, bin_filename);
    log_message("Converted " + text_filename + " to " + bin_filename);
}
