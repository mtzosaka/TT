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
#include <map>
#include <numeric> // Required for std::accumulate, std::inner_product
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
      acquisition_duration_(0.0), // Initialize added member
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
        std::cout << "[Master] " << message << std::endl;
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
        // Use the ZMQ context defined within the class
        local_tc_socket_ = zmq::socket_t(context_, zmq::socket_type::req);
        std::string tc_endpoint = "tcp://" + config_.local_tc_address + ":" + std::to_string(SCPI_PORT); // Use SCPI_PORT from common.hpp
        log_message("Connecting to Time Controller at " + tc_endpoint);
        local_tc_socket_.connect(tc_endpoint);
        
        // Test connection to local Time Controller
        std::string idn = zmq_exec(local_tc_socket_, "*IDN?"); // Use helper from common.hpp
        log_message("Local Time Controller identified: " + idn, true);
        
        // Check slave availability
        log_message("Checking slave availability...", true);
        json response;
        if (!send_command_to_slave("status", response)) {
            std::cerr << "Failed to connect to slave agent" << std::endl;
            return false;
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
            log_message("Status monitor thread stopped");
        }
        if (file_thread_.joinable()) {
            file_thread_.join();
            log_message("File receiver thread stopped");
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

// --- Status Monitor Thread (mostly unchanged) --- 
void MasterController::status_monitor_thread() {
    log_message("Status monitor thread started");
    while (running_) {
        try {
            zmq::message_t message;
            auto result = status_socket_.recv(message, zmq::recv_flags::dontwait);
            if (result.has_value()) {
                std::string status_str(static_cast<char*>(message.data()), message.size());
                log_message("Received status message: " + status_str);
                
                try {
                    json status = json::parse(status_str);
                    if (status["type"] == "status") {
                        std::string state = status["state"].get<std::string>();
                        if (state == "triggered") {
                            // Record slave trigger timestamp
                            slave_trigger_timestamp_ns_ = status["trigger_timestamp"].get<uint64_t>();
                            log_message("Slave trigger timestamp: " + std::to_string(slave_trigger_timestamp_ns_));
                        }
                    }
                } catch (const json::parse_error& e) {
                    std::cerr << "Error parsing status message: " << e.what() << std::endl;
                }
            }
        } catch (const zmq::error_t& e) {
            if (e.num() != EAGAIN) {
                std::cerr << "ZeroMQ error in status monitor: " << e.what() << std::endl;
            }
        } catch (const std::exception& e) {
            std::cerr << "Error in status monitor: " << e.what() << std::endl;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

// --- File Receiver Thread (mostly unchanged) --- 
void MasterController::file_receiver_thread() {
    log_message("File receiver thread started");
    while (running_) {
        try {
            zmq::message_t message;
            auto result = file_socket_.recv(message, zmq::recv_flags::dontwait);
            if (result.has_value()) {
                std::string msg_str(static_cast<char*>(message.data()), message.size());
                
                try {
                    json header = json::parse(msg_str);
                    if (header["type"] == "file_header") {
                        std::string filename = header["filename"].get<std::string>();
                        size_t file_size = header["size"].get<size_t>();
                        size_t chunks = header["chunks"].get<size_t>();
                        
                        log_message("Receiving file: " + filename + " (" + std::to_string(file_size) + " bytes in " + std::to_string(chunks) + " chunks)");
                        
                        // Create output file
                        fs::path output_path = fs::path(config_.output_dir) / filename;
                        std::ofstream file(output_path, std::ios::binary);
                        if (!file.is_open()) {
                            std::cerr << "Failed to open file for writing: " << output_path.string() << std::endl;
                            continue;
                        }
                        
                        // Receive file data in chunks
                        size_t received_chunks = 0;
                        while (received_chunks < chunks && running_) {
                            zmq::message_t data_msg;
                            auto data_result = file_socket_.recv(data_msg);
                            if (!data_result.has_value()) {
                                break;
                            }
                            
                            // Check if this is a footer message
                            if (data_msg.size() < file_size / chunks) {
                                std::string footer_str(static_cast<char*>(data_msg.data()), data_msg.size());
                                try {
                                    json footer = json::parse(footer_str);
                                    if (footer["type"] == "file_footer") {
                                        log_message("File transfer completed: " + filename);
                                        break;
                                    }
                                } catch (...) {
                                    // Not a JSON message, treat as data
                                }
                            }
                            
                            // Write data to file
                            file.write(static_cast<char*>(data_msg.data()), data_msg.size());
                            received_chunks++;
                            
                            if (chunks > 10 && received_chunks % (chunks / 10) == 0) {
                                double progress = static_cast<double>(received_chunks) / chunks * 100.0;
                                log_message("File transfer progress: " + std::to_string(progress) + "%");
                            }
                        }
                        
                        file.close();
                        
                        // Process the received file (e.g., for synchronization)
                        if (filename.find("slave_results_") != std::string::npos) {
                            log_message("Received slave results file: " + filename);
                            
                            // Generate master filename based on the same timestamp
                            std::string master_filename = "master_results_" + filename.substr(13);
                            fs::path master_path = fs::path(config_.output_dir) / master_filename;
                            
                            // Calculate synchronization if both files exist
                            if (fs::exists(master_path)) {
                                log_message("Calculating synchronization between master and slave files...");
                                calculate_sync(master_path.string(), output_path.string());
                            }
                        }
                    }
                } catch (const json::parse_error& e) {
                    std::cerr << "Error parsing file message: " << e.what() << std::endl;
                }
            }
        } catch (const zmq::error_t& e) {
            if (e.num() != EAGAIN) {
                std::cerr << "ZeroMQ error in file receiver: " << e.what() << std::endl;
            }
        } catch (const std::exception& e) {
            std::cerr << "Error in file receiver: " << e.what() << std::endl;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

// --- Start Acquisition (updated for real data collection) --- 
bool MasterController::start_acquisition(double duration, const std::vector<int>& channels) {
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
        acquisition_duration_ = duration;
        acquisition_start_time_ = std::chrono::steady_clock::now();
        
        // Check slave status
        log_message("Preparing for synchronized acquisition...", true);
        json response;
        if (!send_command_to_slave("status", response)) {
            std::cerr << "Failed to connect to slave agent" << std::endl;
            return false;
        }
        
        // Tell slave to prepare for trigger
        if (!send_command_to_slave("prepare_trigger", response)) {
            std::cerr << "Failed to prepare slave for trigger" << std::endl;
            return false;
        }
        
        // Wait for slave to be ready for trigger
        if (!wait_for_slave_ready()) {
            std::cerr << "Slave did not confirm readiness for trigger" << std::endl;
            return false;
        }
        
        // Send trigger to slave
        log_message("Sending trigger to slave...", true);
        if (!send_trigger_to_slave(duration, channels)) {
            std::cerr << "Failed to send trigger to slave" << std::endl;
            return false;
        }
        
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
        fs::path master_output_base = fs::path(config_.output_dir) / ("master_results_" + get_current_timestamp_str());
        std::string master_output_file = master_output_base.string() + ".txt"; // Merger writes text directly
        log_message("Starting merger thread, output to: " + master_output_file);
        merger = new TimestampsMergerThread(stream_clients, master_output_file, static_cast<uint64_t>(pper_ps));
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
            
            // Periodically check slave status
            if (i % 10 == 0) {
                 if (send_command_to_slave("status", response)) { /* ... */ }
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
             std::string bin_filename = master_output_base.string() + ".bin";
             log_message("Converting merged text file to binary: " + bin_filename);
             convert_text_to_binary(master_output_file, bin_filename);
             // Optionally remove the text file if only binary is needed
             // fs::remove(master_output_file);
        } else {
             log_message("Merged timestamps saved to text file: " + master_output_file);
             // Optionally create binary version as well
             std::string bin_filename = master_output_base.string() + ".bin";
             convert_text_to_binary(master_output_file, bin_filename);
        }
        
        // Save master results file path
        std::string bin_filename = master_output_base.string() + ".bin";
        log_message("Saved master timestamps to " + bin_filename, true);
        
        // Update state
        acquisition_active_ = false;
        current_state_ = "completed";
        current_progress_ = 100.0;
        
        if (config_.streaming_mode) {
            log_message("Streaming acquisition completed successfully", true);
        } else {
            log_message("Single-file acquisition completed successfully", true);
        }
        
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

// --- Send Command to Slave (mostly unchanged) --- 
bool MasterController::send_command_to_slave(const std::string& command, json& response) {
    try {
        // Prepare command message
        json cmd;
        cmd["type"] = "command";
        cmd["command"] = command;
        cmd["sequence"] = command_sequence_++;
        
        std::string cmd_str = cmd.dump();
        zmq::message_t message(cmd_str.size());
        memcpy(message.data(), cmd_str.c_str(), cmd_str.size());
        
        // Send command
        command_socket_.send(message, zmq::send_flags::none);
        
        // Receive response with timeout
        zmq::message_t reply;
        command_socket_.set(zmq::sockopt::rcvtimeo, 5000);
        auto result = command_socket_.recv(reply);
        if (!result.has_value()) {
            std::cerr << "Timeout waiting for slave response to command: " << command << std::endl;
            return false;
        }
        
        // Parse response
        std::string reply_str(static_cast<char*>(reply.data()), reply.size());
        response = json::parse(reply_str);
        
        if (!response["success"].get<bool>()) {
            std::cerr << "Slave reported error: " << response["error"].get<std::string>() << std::endl;
            return false;
        }
        
        return true;
        
    } catch (const zmq::error_t& e) {
        std::cerr << "ZeroMQ error sending command to slave: " << e.what() << std::endl;
        return false;
    } catch (const std::exception& e) {
        std::cerr << "Error sending command to slave: " << e.what() << std::endl;
        return false;
    }
}

// --- Wait for Slave Ready (new method for subscription synchronization) --- 
bool MasterController::wait_for_slave_ready() {
    log_message("Waiting for slave to be ready for trigger...");
    
    // Set timeout for waiting
    int timeout = 5000; // 5 seconds
    sync_socket_.set(zmq::sockopt::rcvtimeo, timeout);
    
    // Wait for ready message from slave
    zmq::message_t message;
    auto result = sync_socket_.recv(message);
    if (!result.has_value()) {
        std::cerr << "Timeout waiting for slave ready message" << std::endl;
        return false;
    }
    
    std::string msg_str(static_cast<char*>(message.data()), message.size());
    log_message("Received sync message from slave: " + msg_str);
    
    if (msg_str == "ready_for_trigger") {
        log_message("Slave is ready for trigger");
        return true;
    } else {
        std::cerr << "Unexpected sync message from slave: " << msg_str << std::endl;
        return false;
    }
}

// --- Send Trigger to Slave (mostly unchanged) --- 
bool MasterController::send_trigger_to_slave(double duration, const std::vector<int>& channels) {
    try {
        // Record trigger timestamp
        trigger_timestamp_ns_ = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        
        // Prepare trigger message
        json trigger;
        trigger["type"] = "trigger";
        trigger["timestamp"] = trigger_timestamp_ns_;
        trigger["duration"] = duration;
        trigger["channels"] = channels;
        
        std::string trigger_str = trigger.dump();
        zmq::message_t message(trigger_str.size());
        memcpy(message.data(), trigger_str.c_str(), trigger_str.size());
        
        // Send trigger
        trigger_socket_.send(message, zmq::send_flags::none);
        
        // Wait a bit to ensure the message is sent
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        
        // Check if slave received the trigger
        int retry_count = 0;
        const int max_retries = 3;
        bool trigger_received = false;
        
        while (!trigger_received && retry_count < max_retries) {
            json response;
            if (send_command_to_slave("status", response)) {
                if (response.contains("state") && response["state"].get<std::string>() == "running") {
                    trigger_received = true;
                    break;
                }
            }
            
            if (!trigger_received) {
                std::cerr << "WARNING: Slave may not have received trigger command! Retrying..." << std::endl;
                trigger_socket_.send(message, zmq::send_flags::none);
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                retry_count++;
            }
        }
        
        if (!trigger_received) {
            std::cerr << "Failed to confirm slave received trigger after " << max_retries << " attempts" << std::endl;
            return false;
        }
        
        return true;
        
    } catch (const std::exception& e) {
        std::cerr << "Error sending trigger to slave: " << e.what() << std::endl;
        return false;
    }
}

// --- Calculate Sync (updated for better reporting) --- 
void MasterController::calculate_sync(const std::string& master_file, const std::string& slave_file) {
    try {
        log_message("Calculating synchronization between master and slave files...");
        
        // Read timestamps from files (using only the first X% for sync calculation)
        double sync_percentage = config_.sync_percentage;
        std::vector<uint64_t> master_timestamps = read_timestamps_from_bin_internal(master_file, sync_percentage);
        std::vector<uint64_t> slave_timestamps = read_timestamps_from_bin_internal(slave_file, sync_percentage);
        
        if (master_timestamps.empty() || slave_timestamps.empty()) {
            std::cerr << "Cannot calculate sync: One or both timestamp files are empty" << std::endl;
            return;
        }
        
        // Calculate time differences between consecutive timestamps in each file
        std::vector<int64_t> master_diffs, slave_diffs;
        for (size_t i = 1; i < master_timestamps.size(); ++i) {
            master_diffs.push_back(static_cast<int64_t>(master_timestamps[i] - master_timestamps[i-1]));
        }
        for (size_t i = 1; i < slave_timestamps.size(); ++i) {
            slave_diffs.push_back(static_cast<int64_t>(slave_timestamps[i] - slave_timestamps[i-1]));
        }
        
        // Calculate offset between master and slave timestamps
        std::vector<double> offsets;
        size_t min_size = std::min(master_diffs.size(), slave_diffs.size());
        for (size_t i = 0; i < min_size; ++i) {
            double ratio = static_cast<double>(slave_diffs[i]) / master_diffs[i];
            if (ratio > 0.9 && ratio < 1.1) { // Filter out outliers
                offsets.push_back(static_cast<double>(slave_timestamps[i]) - master_timestamps[i]);
            }
        }
        
        if (offsets.empty()) {
            std::cerr << "Cannot calculate sync: No valid offset measurements" << std::endl;
            return;
        }
        
        // Calculate statistics
        double min_offset = *std::min_element(offsets.begin(), offsets.end());
        double max_offset = *std::max_element(offsets.begin(), offsets.end());
        
        double sum = std::accumulate(offsets.begin(), offsets.end(), 0.0);
        double avg_offset = sum / offsets.size();
        
        double sq_sum = std::inner_product(offsets.begin(), offsets.end(), offsets.begin(), 0.0);
        double std_dev = std::sqrt(sq_sum / offsets.size() - avg_offset * avg_offset);
        
        // Calculate quality metric (0-100%)
        double quality = 100.0 * (1.0 - std::min(std_dev / std::abs(avg_offset), 1.0));
        
        // Store calculated offset
        calculated_offset_ns_ = avg_offset;
        
        // Write offset report
        fs::path offset_path = fs::path(config_.output_dir) / ("offset_report_" + get_current_timestamp_str() + ".txt");
        write_offset_report(offset_path.string(), min_offset, max_offset, avg_offset, std_dev, quality);
        
        // Create corrected master file
        write_corrected_master_file(master_file, avg_offset);
        
        log_message("Synchronization calculation completed. Average offset: " + 
                   std::to_string(avg_offset) + " ns, Quality: " + 
                   std::to_string(quality) + "%", true);
        
    } catch (const std::exception& e) {
        std::cerr << "Error calculating sync: " << e.what() << std::endl;
    }
}

// --- Write Offset Report (new method) --- 
void MasterController::write_offset_report(const std::string& filename, double min_offset, 
                                         double max_offset, double avg_offset, 
                                         double std_dev, double quality) {
    std::ofstream report_file(filename);
    if (!report_file.is_open()) {
        std::cerr << "Failed to open offset report file: " << filename << std::endl;
        return;
    }
    
    auto now = std::chrono::system_clock::now();
    std::time_t now_time_t = std::chrono::system_clock::to_time_t(now);
    std::tm* now_tm = std::localtime(&now_time_t);
    
    report_file << "Distributed Timestamp System - Synchronization Report" << std::endl;
    report_file << "=================================================" << std::endl;
    report_file << "Generated: " << std::put_time(now_tm, "%Y-%m-%d %H:%M:%S") << std::endl;
    report_file << std::endl;
    report_file << "Master Time Controller: " << config_.local_tc_address << std::endl;
    report_file << "Slave Time Controller: " << config_.slave_address << std::endl;
    report_file << std::endl;
    report_file << "Synchronization Statistics:" << std::endl;
    report_file << "--------------------------" << std::endl;
    report_file << "Minimum Offset: " << min_offset << " ns" << std::endl;
    report_file << "Maximum Offset: " << max_offset << " ns" << std::endl;
    report_file << "Average Offset: " << avg_offset << " ns" << std::endl;
    report_file << "Standard Deviation: " << std_dev << " ns" << std::endl;
    report_file << "Offset Range: " << (max_offset - min_offset) << " ns" << std::endl;
    report_file << std::endl;
    report_file << "Synchronization Quality: " << quality << "%" << std::endl;
    report_file << std::endl;
    
    // Quality assessment
    report_file << "Quality Assessment: ";
    if (quality >= 95.0) {
        report_file << "Excellent - Very stable synchronization";
    } else if (quality >= 85.0) {
        report_file << "Good - Reliable synchronization";
    } else if (quality >= 70.0) {
        report_file << "Acceptable - Usable synchronization";
    } else if (quality >= 50.0) {
        report_file << "Poor - High variability in synchronization";
    } else {
        report_file << "Unreliable - Synchronization may not be accurate";
    }
    report_file << std::endl;
    
    report_file.close();
    log_message("Wrote synchronization report to " + filename);
}

// --- Write Corrected Master File (updated to fix ends_with) --- 
void MasterController::write_corrected_master_file(const std::string& master_file, double offset) {
    // Read original master timestamps
    std::vector<uint64_t> master_timestamps = read_timestamps_from_bin_internal(master_file);
    
    if (master_timestamps.empty()) {
        std::cerr << "Cannot create corrected file: Master timestamp file is empty" << std::endl;
        return;
    }
    
    // Apply offset to each timestamp
    std::vector<uint64_t> corrected_timestamps;
    for (uint64_t ts : master_timestamps) {
        // Add offset to align with slave timestamps
        int64_t corrected = static_cast<int64_t>(ts) + static_cast<int64_t>(offset);
        if (corrected < 0) corrected = 0; // Ensure no negative timestamps
        corrected_timestamps.push_back(static_cast<uint64_t>(corrected));
    }
    
    // Write corrected timestamps to new file
    fs::path master_path(master_file);
    fs::path corrected_path = master_path.parent_path() / 
                             ("corrected_" + master_path.filename().string());
    
    write_timestamps_to_bin(corrected_timestamps, corrected_path.string());
    
    // If text output is enabled, also write text version
    if (config_.text_output) {
        std::string text_path_str = corrected_path.string();
        std::string bin_suffix = ".bin";
        // Replace ends_with with C++17 compatible check
        if (text_path_str.size() >= bin_suffix.size() && 
            text_path_str.compare(text_path_str.size() - bin_suffix.size(), bin_suffix.size(), bin_suffix) == 0) {
            text_path_str = text_path_str.substr(0, text_path_str.length() - bin_suffix.size()) + ".txt";
        } else {
            text_path_str += ".txt";
        }
        write_timestamps_to_txt(corrected_timestamps, active_channels_, text_path_str);
    }
    
    log_message("Created corrected master file: " + corrected_path.string());
}

// --- Send TC Command (uses zmq_exec from common.hpp) --- 
std::string MasterController::send_tc_command(const std::string& cmd) {
    try {
        return zmq_exec(local_tc_socket_, cmd);
    } catch (const std::exception& e) {
        return "ERROR: " + std::string(e.what());
    }
}

// --- Write Timestamps to Binary --- 
void MasterController::write_timestamps_to_bin(const std::vector<uint64_t>& timestamps, const std::string& filename) {
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
void MasterController::write_timestamps_to_txt(const std::vector<uint64_t>& timestamps, const std::vector<int>& channels, const std::string& filename) {
    std::ofstream file(filename);
    if (!file.is_open()) {
        std::cerr << "Failed to open text file for writing: " << filename << std::endl;
        return;
    }
    
    auto now = std::chrono::system_clock::now();
    std::time_t now_time_t = std::chrono::system_clock::to_time_t(now);
    std::tm* now_tm = std::localtime(&now_time_t);
    
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
    for (size_t i = 0; i < timestamps.size(); ++i) {
        int channel = (channels.empty()) ? 0 : channels[i % channels.size()]; // Handle empty channels case
        file << i << ", " << timestamps[i] << ", " << channel << std::endl;
    }
    file.close();
    log_message("Wrote " + std::to_string(timestamps.size()) + " timestamps to text file " + filename);
}

// --- Generate Filename (mostly unchanged) --- 
std::string MasterController::generate_filename(bool is_master, int file_index, bool text_format) {
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
std::string MasterController::get_current_timestamp_str() {
     auto now = std::chrono::system_clock::now();
     auto now_time_t = std::chrono::system_clock::to_time_t(now);
     std::tm now_tm = *std::localtime(&now_time_t);
     char time_str[20];
     std::strftime(time_str, sizeof(time_str), "%Y%m%d_%H%M%S", &now_tm);
     return std::string(time_str);
}

// --- Convert Text Timestamps to Binary --- 
void MasterController::convert_text_to_binary(const std::string& text_filename, const std::string& bin_filename) {
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
