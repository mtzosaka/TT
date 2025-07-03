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
#include <algorithm>
#include "json.hpp"
#include "streams.hpp" // Include streams header for BufferStreamClient and TimestampsMergerThread
#include "common.hpp"  // Include common header for ZMQ helpers

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
    
    std::vector<BufferStreamClient*> stream_clients;
    TimestampsMergerThread* merger = nullptr;
    std::map<int, std::string> acquisitions_id; // For DLT interaction if needed
    zmq::socket_t dlt_socket(context_, zmq::socket_type::req); // DLT socket if needed
    bool dlt_connected = false;

    try {
        log_message("Preparing for synchronized acquisition...", true);
        
        // Store active channels for text output
        active_channels_ = channels;
        
        // Update state
        current_state_ = "starting";
        current_progress_ = 0.0;
        acquisition_duration_ = duration;
        
        // --- Trigger Slave --- 
        log_message("Waiting for slave to be ready for trigger...", true);
        json response;
        if (!send_command_to_slave("status", response)) {
            std::cerr << "ERROR: Slave not responding to status command" << std::endl;
            return false;
        }
        if (!send_command_to_slave("prepare_trigger", response)) {
            std::cerr << "ERROR: Failed to prepare slave for trigger" << std::endl;
            return false;
        }
        if (!response["success"].get<bool>()) {
            std::cerr << "ERROR: Slave failed to prepare for trigger: " << response["error"].get<std::string>() << std::endl;
            return false;
        }
        log_message("Slave is preparing for trigger");
        log_message("Waiting for slave subscription sync message...");
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
        
        log_message("Sending trigger to slave...", true);
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
        log_message("Waiting for slave to process trigger...");
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        // Check if slave received trigger (optional, add more robust check if needed)
        // ... (existing check logic can remain)
        // --- End Trigger Slave --- 

        // --- Start Local Acquisition (Real Data) --- 
        log_message("Starting local acquisition...", true);

        // Connect to local DLT (assuming it's running or needs launching)
        // This part needs adaptation based on whether DLT is required/available
        // For now, we'll bypass DLT and assume direct streaming if possible,
        // or implement a simplified streaming mechanism.
        // If DLT is essential, the dlt_connect logic from common.cpp needs integration.
        log_message("Connecting to local DLT (or preparing direct stream)...", true);
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
        std::string master_output_base = fs::path(config_.output_dir) / ("master_results_" + get_current_timestamp_str());
        std::string master_output_file = master_output_base.string() + ".txt"; // Merger writes text directly
        log_message("Starting merger thread, output to: " + master_output_file);
        merger = new TimestampsMergerThread(stream_clients, master_output_file, static_cast<uint64_t>(pper_ps));
        merger->start();

        // Start TC recording
        acquisition_active_ = true;
        acquisition_start_time_ = std::chrono::steady_clock::now();
        current_state_ = "running";
        zmq_exec(local_tc_socket_, "REC:PLAY");
        log_message("Local acquisition started. Collecting data for " + std::to_string(duration) + " seconds...", true);
        
        // Wait for specified duration (with progress updates)
        const int update_interval_ms = 100;
        int total_updates = static_cast<int>(duration * 1000 / update_interval_ms);
        for (int i = 0; i < total_updates && running_; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(update_interval_ms));
            current_progress_ = static_cast<double>(i) / total_updates * 100.0;
            // Periodically check slave status (existing logic)
            if (i % 10 == 0) { 
                 if (send_command_to_slave("status", response)) { /* ... */ }
            }
        }
        
        // Stop TC recording
        log_message("Stopping local acquisition...", true);
        zmq_exec(local_tc_socket_, "REC:STOP");
        
        // Wait for end of acquisition (adapt from common.cpp if using DLT)
        log_message("Waiting for final data transfer...", true);
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

        // Wait for slave to complete and transfer files
        log_message("Waiting for slave to complete and transfer files...");
        // The file_receiver_thread handles receiving files and triggering calculate_sync
        std::this_thread::sleep_for(std::chrono::seconds(5)); // Adjust wait time as needed
        
        // Update state
        acquisition_active_ = false;
        current_state_ = "completed";
        current_progress_ = 100.0;
        
        log_message("Master acquisition process completed.", true);
        return success;
        
    } catch (const std::exception& e) {
        std::cerr << "Error during acquisition: " << e.what() << std::endl;
        current_state_ = "error";
        current_error_ = e.what();
        acquisition_active_ = false;
        // Cleanup resources in case of error
        if (merger) merger->join();
        for (BufferStreamClient* client : stream_clients) client->join();
        return false;
    } finally {
        // Cleanup allocated resources
        delete merger;
        for (BufferStreamClient* client : stream_clients) delete client;
        if (dlt_connected) dlt_socket.close();
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
                log_message("Received status update: " + status_str);
                json status = json::parse(status_str);
                if (status["type"] == "status") {
                    if (status.contains("trigger_timestamp")) {
                        slave_trigger_timestamp_ns_ = status["trigger_timestamp"].get<uint64_t>();
                        if (trigger_timestamp_ns_ > 0 && slave_trigger_timestamp_ns_ > 0) {
                            calculated_offset_ns_ = static_cast<double>(slave_trigger_timestamp_ns_) - 
                                                  static_cast<double>(trigger_timestamp_ns_);
                            log_message("Calculated trigger offset: " + std::to_string(calculated_offset_ns_) + " ns", true);
                        }
                    }
                    // Process other status info...
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
    log_message("Status monitor thread stopped");
}

// --- File Receiver Thread (mostly unchanged, triggers calculate_sync) --- 
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
                if (message.size() > 0 && (static_cast<char*>(message.data())[0] == '{' || static_cast<char*>(message.data())[0] == '[')) {
                    try {
                        std::string json_str(static_cast<char*>(message.data()), message.size());
                        json msg = json::parse(json_str);
                        if (msg["type"] == "file_header") {
                            current_filename = msg["filename"].get<std::string>();
                            current_file_size = msg["size"].get<size_t>();
                            current_chunks = msg["chunks"].get<size_t>();
                            chunks_received = 0;
                            log_message("File header received: " + json_str);
                            if (output_file.is_open()) output_file.close();
                            fs::path output_path = fs::path(config_.output_dir) / current_filename;
                            output_file.open(output_path, std::ios::binary);
                            if (!output_file.is_open()) {
                                std::cerr << "Failed to open output file: " << output_path << std::endl;
                                continue;
                            }
                            log_message("Receiving file: " + current_filename + " (" + std::to_string(current_file_size) + " bytes)", true);
                        } else if (msg["type"] == "file_footer") {
                            log_message("File footer received: " + json_str);
                            if (output_file.is_open()) {
                                output_file.close();
                                log_message("File transfer complete: " + current_filename, true);
                                // Check if this is a slave results file for synchronization
                                if (current_filename.find("slave_results_") != std::string::npos && current_filename.find(".bin") != std::string::npos) {
                                    // Find corresponding master file (assuming text format was primary)
                                    std::string base_name = current_filename;
                                    base_name.replace(base_name.find("slave_"), 6, "master_");
                                    base_name.replace(base_name.find(".bin"), 4, ""); // Remove extension
                                    
                                    fs::path master_bin_path = fs::path(config_.output_dir) / (base_name + ".bin");
                                    fs::path slave_bin_path = fs::path(config_.output_dir) / current_filename;
                                    
                                    if (fs::exists(master_bin_path) && fs::exists(slave_bin_path)) {
                                        calculate_sync(master_bin_path.string(), slave_bin_path.string());
                                    } else {
                                        log_message("Cannot calculate sync, binary files not found:");
                                        log_message("Master: " + master_bin_path.string() + " exists: " + std::to_string(fs::exists(master_bin_path)));
                                        log_message("Slave: " + slave_bin_path.string() + " exists: " + std::to_string(fs::exists(slave_bin_path)));
                                    }
                                }
                            }
                        }
                    } catch (const json::parse_error& e) {
                        // Not JSON, treat as binary chunk
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
            if (e.num() != EAGAIN) {
                std::cerr << "ZeroMQ error in file receiver: " << e.what() << std::endl;
            }
        } catch (const std::exception& e) {
            std::cerr << "Error in file receiver: " << e.what() << std::endl;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (output_file.is_open()) output_file.close();
    log_message("File receiver thread stopped");
}

// --- Send TC Command (uses zmq_exec from common.hpp) --- 
std::string MasterController::send_tc_command(const std::string& cmd) {
    try {
        return zmq_exec(local_tc_socket_, cmd);
    } catch (const std::exception& e) {
        return "ERROR: " + std::string(e.what());
    }
}

// --- Send Command to Slave (mostly unchanged) --- 
bool MasterController::send_command_to_slave(const std::string& command, json& response) {
    try {
        json cmd;
        cmd["type"] = "command";
        cmd["command"] = command;
        cmd["sequence"] = command_sequence_++;
        std::string cmd_str = cmd.dump();
        zmq::message_t request(cmd_str.size());
        memcpy(request.data(), cmd_str.c_str(), cmd_str.size());
        int timeout = 1000; // 1 second
        command_socket_.set(zmq::sockopt::rcvtimeo, timeout);
        log_message("Sending command to slave: " + cmd_str);
        command_socket_.send(request, zmq::send_flags::none);
        zmq::message_t reply;
        auto result = command_socket_.recv(reply, zmq::recv_flags::none);
        if (!result.has_value()) {
            log_message("No response received for command: " + command);
            return false;
        }
        std::string reply_str(static_cast<char*>(reply.data()), reply.size());
        log_message("Received response: " + reply_str);
        response = json::parse(reply_str);
        return true;
    } catch (const std::exception& e) {
        std::cerr << "Error sending command to slave: " << e.what() << std::endl;
        return false;
    }
}

// --- Calculate Sync (updated to create corrected file) --- 
void MasterController::calculate_sync(const std::string& master_bin_file, const std::string& slave_bin_file) {
    try {
        log_message("Calculating synchronization between " + master_bin_file + " and " + slave_bin_file, true);
        
        // Read timestamps from binary files (only first 10% for sync calculation)
        std::vector<uint64_t> master_timestamps = read_timestamps_from_bin_internal(master_bin_file, config_.sync_percentage);
        std::vector<uint64_t> slave_timestamps = read_timestamps_from_bin_internal(slave_bin_file, config_.sync_percentage);
        
        if (master_timestamps.empty() || slave_timestamps.empty()) {
            std::cerr << "Error: Empty timestamp data for sync calculation" << std::endl;
            return;
        }
        
        // Calculate offset statistics
        double min_offset = 1e18;
        double max_offset = -1e18;
        double sum_offset = 0.0;
        int count = 0;
        size_t compare_size = std::min(master_timestamps.size(), slave_timestamps.size());
        
        for (size_t i = 0; i < compare_size; i++) {
            double offset = static_cast<double>(slave_timestamps[i]) - static_cast<double>(master_timestamps[i]);
            min_offset = std::min(min_offset, offset);
            max_offset = std::max(max_offset, offset);
            sum_offset += offset;
            count++;
        }
        
        double avg_offset = (count > 0) ? (sum_offset / count) : 0.0;
        
        // Calculate standard deviation
        double sum_squared_diff = 0.0;
        for (size_t i = 0; i < compare_size; i++) {
            double offset = static_cast<double>(slave_timestamps[i]) - static_cast<double>(master_timestamps[i]);
            double diff = offset - avg_offset;
            sum_squared_diff += diff * diff;
        }
        double std_dev = (count > 0) ? std::sqrt(sum_squared_diff / count) : 0.0;
        
        // Write offset report file
        fs::path offset_path = fs::path(config_.output_dir) / ("offset_report_" + get_current_timestamp_str() + ".txt");
        write_offset_report(offset_path.string(), calculated_offset_ns_, min_offset, max_offset, avg_offset, std_dev);
        
        // Print summary to console
        log_message("Synchronization summary:", true);
        log_message("  Average data offset: " + std::to_string(avg_offset) + " ns", true);
        log_message("  Standard deviation: " + std::to_string(std_dev) + " ns", true);
        log_message("  Detailed report written to: " + offset_path.string(), true);

        // --- Create Corrected Master File --- 
        log_message("Creating corrected master timestamp file...", true);
        // Read the *full* original master binary file
        std::vector<uint64_t> full_master_timestamps = read_timestamps_from_bin_internal(master_bin_file, 1.0); 
        if (!full_master_timestamps.empty()) {
            // Apply the average offset
            for (uint64_t& ts : full_master_timestamps) {
                ts += static_cast<int64_t>(avg_offset); // Apply average offset
            }
            
            // Generate corrected filename
            fs::path original_path(master_bin_file);
            std::string corrected_filename = original_path.stem().string() + "_corrected" + original_path.extension().string();
            fs::path corrected_path = original_path.parent_path() / corrected_filename;
            
            // Write corrected timestamps to new binary file
            write_timestamps_to_bin(full_master_timestamps, corrected_path.string());
            log_message("Corrected master file written to: " + corrected_path.string(), true);

            // Optionally write corrected text file if text output is enabled
            if (config_.text_output) {
                 fs::path corrected_text_path = corrected_path;
                 corrected_text_path.replace_extension(".txt");
                 write_timestamps_to_txt(full_master_timestamps, active_channels_, corrected_text_path.string());
                 log_message("Corrected master text file written to: " + corrected_text_path.string(), true);
            }
        } else {
            log_message("Could not read full master file for correction.", true);
        }
        // --- End Corrected Master File --- 
        
    } catch (const std::exception& e) {
        std::cerr << "Error calculating synchronization: " << e.what() << std::endl;
    }
}

// --- Write Offset Report (mostly unchanged) --- 
void MasterController::write_offset_report(const std::string& filename, double trigger_offset_ns, double min_offset, double max_offset, double avg_offset, double std_dev) {
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
    report_file << "Master Controller: " << config_.local_tc_address << std::endl;
    report_file << "Slave Agent: " << config_.slave_address << std::endl;
    report_file << std::endl;
    report_file << "Trigger Timestamp Offset (Slave - Master): " << trigger_offset_ns << " ns" << std::endl;
    report_file << std::endl;
    report_file << "Data Timestamp Statistics (Slave - Master, nanoseconds):" << std::endl;
    report_file << "  Minimum Offset: " << min_offset << std::endl;
    report_file << "  Maximum Offset: " << max_offset << std::endl;
    report_file << "  Average Offset: " << avg_offset << std::endl;
    report_file << "  Standard Deviation: " << std_dev << std::endl;
    report_file << "  Range: " << (max_offset - min_offset) << std::endl;
    report_file << std::endl;
    report_file << "Synchronization Quality Assessment:" << std::endl;
    if (std::abs(std_dev) < 100.0) {
        report_file << "  Excellent synchronization (std dev < 100 ns)" << std::endl;
    } else if (std::abs(std_dev) < 500.0) {
        report_file << "  Good synchronization (std dev < 500 ns)" << std::endl;
    } else if (std::abs(std_dev) < 1000.0) {
        report_file << "  Acceptable synchronization (std dev < 1000 ns)" << std::endl;
    } else {
        report_file << "  Poor synchronization (std dev > 1000 ns)" << std::endl;
        report_file << "  Consider checking network conditions and Time Controller settings" << std::endl;
    }
    report_file.close();
    log_message("Offset report written to " + filename);
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

// --- Read Timestamps from Binary (uses internal helper) --- 
std::vector<uint64_t> MasterController::read_timestamps_from_bin(const std::string& filename, double percentage) {
     return read_timestamps_from_bin_internal(filename, percentage);
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
    return fs::path(config_.output_dir) / filename;
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

