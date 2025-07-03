#include "fixed_updated_slave_agent.hpp"
#include <iostream>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <chrono>
#include <ctime>
#include <thread>
#include <filesystem>
#include <algorithm>
#include <numeric>
#include "working_common.hpp"
#include "streams.hpp"

namespace fs = std::filesystem;
using json = nlohmann::json;

SlaveAgent::SlaveAgent(const SlaveConfig& config)
    : config_(config), running_(false), acquisition_active_(false), command_sequence_(0) {
}

SlaveAgent::~SlaveAgent() {
    stop();
}

bool SlaveAgent::initialize() {
    try {
        log_message("Initializing Slave Agent...");
        log_message("Local Time Controller: " + config_.slave_tc_address);
        log_message("Master address: " + config_.master_address);
        
        // Initialize ZeroMQ context and sockets
        log_message("Setting up communication channels...");
        context_ = zmq::context_t(1);
        
        // Socket for receiving trigger commands from master
        log_message("Creating trigger socket (SUB)...");
        trigger_socket_ = zmq::socket_t(context_, zmq::socket_type::sub);
        std::string trigger_endpoint = "tcp://" + config_.master_address + ":" + std::to_string(config_.trigger_port);
        log_message("Connecting trigger socket to: " + trigger_endpoint);
        trigger_socket_.connect(trigger_endpoint);
        trigger_socket_.set(zmq::sockopt::subscribe, "");
        log_message("Trigger socket connected and subscribed");
        
        // Socket for sending status/heartbeat messages to master
        log_message("Creating status socket (PUSH)...");
        status_socket_ = zmq::socket_t(context_, zmq::socket_type::push);
        std::string status_endpoint = "tcp://" + config_.master_address + ":" +
                                     std::to_string(config_.status_port);
        log_message("Connecting status socket to: " + status_endpoint);
        status_socket_.connect(status_endpoint);
        log_message("Status socket connected");

        // Socket for sending files to master
        log_message("Creating file socket (PUSH)...");
        file_socket_ = zmq::socket_t(context_, zmq::socket_type::push);
        std::string file_endpoint = "tcp://" + config_.master_address + ":" + std::to_string(config_.file_port);
        log_message("Connecting file socket to: " + file_endpoint);
        file_socket_.connect(file_endpoint);
        log_message("File socket connected");

        // Socket for sending heartbeat/status messages
        log_message("Creating status socket (PUSH)...");
        status_socket_ = zmq::socket_t(context_, zmq::socket_type::push);
        std::string status_endpoint = "tcp://" + config_.master_address + ":" + std::to_string(config_.status_port);
        log_message("Connecting status socket to: " + status_endpoint);
        status_socket_.connect(status_endpoint);
        log_message("Status socket connected");
        
        // Socket for receiving commands from master
        log_message("Creating command socket (REP)...");
        command_socket_ = zmq::socket_t(context_, zmq::socket_type::rep);
        std::string command_endpoint = "tcp://*:" + std::to_string(config_.command_port);
        log_message("Binding command socket to: " + command_endpoint);
        command_socket_.bind(command_endpoint);
        log_message("Command socket bound");
        
        // Socket for synchronization handshake
        log_message("Creating sync socket (PUSH)...");
        sync_socket_ = zmq::socket_t(context_, zmq::socket_type::push);
        std::string sync_endpoint = "tcp://" + config_.master_address + ":" + std::to_string(config_.sync_port);
        log_message("Connecting sync socket to: " + sync_endpoint);
        sync_socket_.connect(sync_endpoint);
        log_message("Sync socket connected");
        
        // Set socket options for better reliability
        int linger = 1000;  // 1 second linger period
        sync_socket_.set(zmq::sockopt::linger, linger);
        status_socket_.set(zmq::sockopt::linger, linger);
        
        // Connect to local Time Controller
        log_message("Connecting to local Time Controller...");
        local_tc_socket_ = connect_zmq(config_.slave_tc_address, 5555);
        
        // Identify the Time Controller
        std::string id_response = zmq_exec(local_tc_socket_, "*IDN?");
        log_message("Local Time Controller identified: " + id_response);
        
        // Create output directory if it doesn't exist
        try {
            if (!fs::exists(config_.output_dir)) {
                fs::create_directories(config_.output_dir);
            }
        }
        catch (const std::exception& e) {
            log_message("WARNING: Failed to create output directory: " + std::string(e.what()));
            log_message("Using current directory instead.");
            config_.output_dir = ".";
        }
        
        // Start threads
        start_trigger_listener_thread();
        start_command_handler_thread();
        start_heartbeat_thread();
        
        log_message("Slave Agent initialized successfully.");
        return true;
    }
    catch (const std::exception& e) {
        log_message("ERROR: Initialization failed: " + std::string(e.what()));
        return false;
    }
}

void SlaveAgent::stop() {
    if (running_) {
        running_ = false;
        
        // Add a small delay to ensure threads see the updated running_ flag
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        
        if (trigger_thread_.joinable()) {
            try {
                trigger_thread_.join();
                log_message("Trigger listener thread stopped");
            } catch (const std::exception& e) {
                log_message("ERROR: Failed to join trigger thread: " + std::string(e.what()));
            }
        }
        
        if (command_thread_.joinable()) {
            try {
                command_thread_.join();
                log_message("Command handler thread stopped");
            } catch (const std::exception& e) {
                log_message("ERROR: Failed to join command thread: " + std::string(e.what()));
            }
        }
        
        if (heartbeat_thread_.joinable()) {
            try {
                heartbeat_thread_.join();
                log_message("Heartbeat thread stopped");
            } catch (const std::exception& e) {
                log_message("ERROR: Failed to join heartbeat thread: " + std::string(e.what()));
            }
        }
        
        // Close sockets
        try {
            trigger_socket_.close();
            status_socket_.close();
            file_socket_.close();
            status_socket_.close();
            command_socket_.close();
            sync_socket_.close();
            local_tc_socket_.close();
        } catch (const std::exception& e) {
            log_message("ERROR: Failed to close sockets: " + std::string(e.what()));
        }
        
        log_message("Slave Agent stopped.");
    }
}

void SlaveAgent::start_trigger_listener_thread() {
    running_ = true;
    trigger_thread_ = std::thread([this]() {
        log_message("Trigger listener thread started");
        
        // IMPORTANT: We no longer automatically send the ready signal here
        // Instead, we wait for a specific "request_ready" command from the master
        // The ready signal will be sent in the command handler when that command is received
        
        while (running_) {
            try {
                // Set timeout for receiving triggers
                trigger_socket_.set(zmq::sockopt::rcvtimeo, 1000);
                
                zmq::message_t trigger_msg;
                auto result = trigger_socket_.recv(trigger_msg);
                
                if (result.has_value()) {
                    std::string trigger_data(static_cast<char*>(trigger_msg.data()), trigger_msg.size());
                    log_message("Received trigger: " + trigger_data, true);
                    
                    try {
                        // Parse trigger message
                        json trigger_json = json::parse(trigger_data);
                        
                        if (trigger_json.contains("command") && trigger_json["command"] == "trigger") {
                            // Extract trigger parameters
                            uint64_t trigger_timestamp = trigger_json["timestamp"].get<uint64_t>();
                            int sequence = trigger_json["sequence"].get<int>();
                            double duration = trigger_json["duration"].get<double>();
                            std::vector<int> channels = trigger_json["channels"].get<std::vector<int>>();
                            
                            // Process the trigger
                            process_trigger(trigger_timestamp, sequence, duration, channels);
                        }
                    }
                    catch (const json::exception& e) {
                        log_message("ERROR: Failed to parse trigger message: " + std::string(e.what()), true);
                    }
                }
            }
            catch (const std::exception& e) {
                // Only log if it's not a timeout
                if (std::string(e.what()).find("Resource temporarily unavailable") == std::string::npos) {
                    log_message("Trigger listener error: " + std::string(e.what()), true);
                }
            }
            
            // Brief pause to avoid busy waiting
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    });
}

void SlaveAgent::start_command_handler_thread() {
    command_thread_ = std::thread([this]() {
        log_message("Command handler thread started");
        while (running_) {
            try {
                // Set timeout for receiving commands
                command_socket_.set(zmq::sockopt::rcvtimeo, 1000);
                
                zmq::message_t command_msg;
                auto result = command_socket_.recv(command_msg);
                
                if (result.has_value()) {
                    std::string command_data(static_cast<char*>(command_msg.data()), command_msg.size());
                    log_message("Received command: " + command_data, true);
                    
                    json response;
                    
                    try {
                        // Parse command message
                        json command_json = json::parse(command_data);
                        
                        if (command_json.contains("command")) {
                            std::string command = command_json["command"].get<std::string>();
                            
                            if (command == "ping") {
                                // Simple ping command to check availability
                                response["status"] = "ok";
                                response["message"] = "Slave agent is running";
                            }
                            else if (command == "status") {
                                // Return current status
                                response["status"] = acquisition_active_ ? "running" : "idle";
                                response["message"] = "Slave agent status";
                            }
                            else if (command == "request_partial_data") {
                                // Master requests 10% partial data
                                log_message("Master requested partial data");
                                if (!latest_timestamps_.empty()) {
                                    size_t partial_count = static_cast<size_t>(latest_timestamps_.size() * 0.1);
                                    if (partial_count < 10) partial_count = std::min(static_cast<size_t>(10), latest_timestamps_.size());
                                    
                                    // First respond to confirm the request
                                    response["status"] = "ok";
                                    response["message"] = "Partial data will be sent (" + std::to_string(partial_count) + " timestamps)";
                                    
                                    std::string response_str = response.dump();
                                    zmq::message_t response_msg(response_str.size());
                                    memcpy(response_msg.data(), response_str.c_str(), response_str.size());
                                    command_socket_.send(response_msg, zmq::send_flags::none);
                                    
                                    // Give master time to prepare file receiver
                                    std::this_thread::sleep_for(std::chrono::seconds(1));
                                    
                                    // Now send the actual partial data
                                    std::vector<uint64_t> partial_timestamps(latest_timestamps_.begin(), latest_timestamps_.begin() + partial_count);
                                    std::vector<int> partial_channels(latest_channels_.begin(), latest_channels_.begin() + partial_count);
                                    
                                    send_partial_data_to_master(partial_timestamps, partial_channels, 1);
                                    
                                    log_message("Partial data sent successfully (" + std::to_string(partial_count) + " timestamps)");
                                    continue; // Skip the normal response since we already sent it
                                } else {
                                    response["status"] = "error";
                                    response["message"] = "No data available";
                                }
                            }
                            else if (command == "request_full_data") {
                                // Master requests full binary data
                                log_message("Master requested full data");
                                if (!latest_bin_filename_.empty() && fs::exists(latest_bin_filename_)) {
                                    send_file_to_master(latest_bin_filename_);
                                    response["status"] = "ok";
                                    response["message"] = "Full data sent";
                                } else {
                                    response["status"] = "error";
                                    response["message"] = "No data file available";
                                }
                            }
                            else if (command == "request_text_data") {
                                // Master requests text data
                                log_message("Master requested text data");
                                if (!latest_txt_filename_.empty() && fs::exists(latest_txt_filename_)) {
                                    send_file_to_master(latest_txt_filename_);
                                    response["status"] = "ok";
                                    response["message"] = "Text data sent";
                                } else {
                                    response["status"] = "error";
                                    response["message"] = "No text file available";
                                }
                            }
                            else if (command == "request_ready") {
                                // NEW: Master is requesting the slave to send ready signal
                                log_message("Master requested ready signal, preparing to send...", true);
                                
                                // First respond to the command
                                response["status"] = "ok";
                                response["message"] = "Ready signal will be sent";
                                
                                std::string response_str = response.dump();
                                zmq::message_t response_msg(response_str.size());
                                memcpy(response_msg.data(), response_str.c_str(), response_str.size());
                                command_socket_.send(response_msg, zmq::send_flags::none);
                                
                                // Then send ready signal to master with a brief delay to ensure master is listening
                                std::this_thread::sleep_for(std::chrono::milliseconds(500));
                                
                                // Send ready signal to master
                                std::string ready_msg = "ready_for_trigger";
                                zmq::message_t ready(ready_msg.size());
                                memcpy(ready.data(), ready_msg.c_str(), ready_msg.size());
                                
                                log_message("Sending ready signal to master via sync socket...", true);
                                bool sent = false;
                                
                                // Try multiple times to ensure the message gets through
                                for (int retry = 0; retry < 5 && !sent; retry++) {
                                    try {
                                        auto send_result = sync_socket_.send(ready, zmq::send_flags::none);
                                        if (send_result.has_value()) {
                                            log_message("Ready signal sent successfully (size: " + 
                                                       std::to_string(send_result.value()) + " bytes)", true);
                                            sent = true;
                                        } else {
                                            log_message("Failed to send ready signal, retrying...", true);
                                            std::this_thread::sleep_for(std::chrono::milliseconds(200));
                                        }
                                    }
                                    catch (const std::exception& e) {
                                        log_message("Error sending ready signal: " + std::string(e.what()) + 
                                                   ", retrying...", true);
                                        std::this_thread::sleep_for(std::chrono::milliseconds(200));
                                    }
                                }
                                
                                if (!sent) {
                                    log_message("ERROR: Failed to send ready signal after multiple attempts", true);
                                }
                                
                                // We've already sent the response, so continue to next command
                                continue;
                            }
                            else if (command == "partial_data_ack") {
                                log_message("Received partial data acknowledgment from master", true);
                                response["status"] = "ok";
                                response["message"] = "acknowledged";
                            }
                            else if (command == "finalize") {
                                log_message("Master requested finalization", true);
                                response["status"] = "ok";
                                response["message"] = "finalization complete";
                            }
                            else {
                                response["status"] = "error";
                                response["message"] = "Unknown command: " + command;
                            }
                        }
                        else {
                            response["status"] = "error";
                            response["message"] = "Invalid command format";
                        }
                    }
                    catch (const json::exception& e) {
                        response["status"] = "error";
                        response["message"] = "Failed to parse command: " + std::string(e.what());
                    }
                    catch (const std::exception& e) {
                        response["status"] = "error";
                        response["message"] = "Error processing command: " + std::string(e.what());
                    }
                    
                    // Send response
                    try {
                        std::string response_str = response.dump();
                        zmq::message_t response_msg(response_str.size());
                        memcpy(response_msg.data(), response_str.c_str(), response_str.size());
                        command_socket_.send(response_msg, zmq::send_flags::none);
                    }
                    catch (const std::exception& e) {
                        log_message("ERROR: Failed to send response: " + std::string(e.what()), true);
                    }
                }
            }
            catch (const std::exception& e) {
                // Only log if it's not a timeout
                if (std::string(e.what()).find("Resource temporarily unavailable") == std::string::npos) {
                    log_message("Command handler error: " + std::string(e.what()), true);
                }
                
                // Send error response if possible
                try {
                    json error_response;
                    error_response["status"] = "error";
                    error_response["message"] = "Error processing command: " + std::string(e.what());
                    
                    std::string response_str = error_response.dump();
                    zmq::message_t response_msg(response_str.size());
                    memcpy(response_msg.data(), response_str.c_str(), response_str.size());
                    command_socket_.send(response_msg, zmq::send_flags::none);
                }
                catch (...) {
                    // Ignore errors in error handling
                }
            }
            
            // Brief pause to avoid busy waiting
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    });
}

void SlaveAgent::start_heartbeat_thread() {
    heartbeat_thread_ = std::thread([this]() {
        log_message("Heartbeat thread started");
        while (running_) {
            // Sleep for the heartbeat interval
            std::this_thread::sleep_for(std::chrono::milliseconds(config_.heartbeat_interval_ms));
            
            // Send heartbeat to master if needed
            if (acquisition_active_) {
                try {
                    json heartbeat;
                    heartbeat["type"] = "heartbeat";
                    heartbeat["status"] = "running";
                    heartbeat["timestamp"] = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::system_clock::now().time_since_epoch()).count();
                    
                    std::string heartbeat_str = heartbeat.dump();
                    zmq::message_t heartbeat_msg(heartbeat_str.size());
                    memcpy(heartbeat_msg.data(), heartbeat_str.c_str(), heartbeat_str.size());
                    status_socket_.send(heartbeat_msg, zmq::send_flags::none);
                    
                    log_message("Sent heartbeat", true);
                }
                catch (const std::exception& e) {
                    log_message("Heartbeat error: " + std::string(e.what()), true);
                }
            }
        }
    });
}

void SlaveAgent::process_trigger(uint64_t trigger_timestamp, int sequence, double duration, const std::vector<int>& channels) {
    try {
        log_message("Processing trigger command (sequence " + std::to_string(sequence) + ")");
        log_message("Trigger timestamp: " + std::to_string(trigger_timestamp) + " ns");
        log_message("Duration: " + std::to_string(duration) + " seconds");
        log_message("Channels: " + std::to_string(channels.size()) + " channels");
        
        // Record when we received the trigger for synchronization
        auto trigger_received_time = std::chrono::high_resolution_clock::now();
        auto slave_trigger_timestamp_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
            trigger_received_time.time_since_epoch()).count();
        
        log_message("Slave trigger timestamp: " + std::to_string(slave_trigger_timestamp_ns) + " ns", true);
        
        // Send slave trigger timestamp back to master for initial offset calculation
        send_trigger_timestamp_to_master(slave_trigger_timestamp_ns, sequence);
        
        acquisition_active_ = true;
        
        // Generate output filename based on current timestamp
        fs::path slave_output_base = fs::path(config_.output_dir) / ("slave_results_" + get_current_timestamp_str());
        
        // Start local acquisition
        log_message("Starting local acquisition...");
        
        // Configure the Time Controller for acquisition
        configure_timestamps_references(local_tc_socket_, channels);
        
        // Set up acquisition parameters
        for (int ch : channels) {
            zmq_exec(local_tc_socket_, "RAW" + std::to_string(ch) + ":SEND ON");
        }
        
        // Proc        // Use the exact working data collection approach from DataLinkTargetService
        log_message("Starting working data collection approach...");
        
        try {
            // Connect to DataLinkTargetService (DLT) - using the working approach
            std::filesystem::path output_dir = fs::path(config_.output_dir);
            zmq::socket_t dlt = dlt_connect(output_dir);
            
            // Close any prior acquisitions (clean slate)
            close_active_acquisitions(dlt);
            
            // Ensure each channel's timestamps have no external reference (needed for merging)
            for (int ch : channels) {
                zmq_exec(local_tc_socket_, "RAW" + std::to_string(ch) + ":REF:LINK NONE");
                zmq_exec(local_tc_socket_, "RAW" + std::to_string(ch) + ":ERRORS:CLEAR");
            }
            
            // Compute pulse width (PWID) and period (PPER) in picoseconds for sub-acquisitions
            long long pwid_ps = static_cast<long long>(1e12 * duration);
            long long pper_ps = static_cast<long long>(1e12 * (duration + 40e-9));  // add 40 ns dead-time
            
            // Configure the Time Controller's recording settings for synchronized sub-acquisitions
            zmq_exec(local_tc_socket_, "REC:TRIG:ARM:MODE MANUal");  // manual trigger mode
            zmq_exec(local_tc_socket_, "REC:ENABle ON");             // enable the Record generator
            zmq_exec(local_tc_socket_, "REC:STOP");                 // ensure no acquisition is currently running
            zmq_exec(local_tc_socket_, "REC:NUM 1");                // single acquisition
            zmq_exec(local_tc_socket_, "REC:PWID " + std::to_string(pwid_ps) + ";PPER " + std::to_string(pper_ps));
            
            // Open streamed acquisitions on each requested channel
            std::map<int, std::string> acquisitions_id;
            std::vector<BufferStreamClient*> stream_clients;
            
            for (int ch : channels) {
                zmq_exec(local_tc_socket_, "RAW" + std::to_string(ch) + ":ERRORS:CLEAR");  // reset error counter on channel
                
                // Start a BufferStreamClient to receive timestamps for this channel
                BufferStreamClient* client = new BufferStreamClient(ch);
                stream_clients.push_back(client);
                client->start();
                
                // Instruct DLT to start streaming this channel to the given port
                std::string cmd = "start-stream --address " + config_.local_tc_address + 
                                  " --channel " + std::to_string(ch) + 
                                  " --stream-port " + std::to_string(client->port);
                json response = dlt_exec(dlt, cmd);
                if (response.contains("id")) {
                    acquisitions_id[ch] = response["id"].get<std::string>();
                }
                
                // Tell the Time Controller to send timestamps from this channel
                zmq_exec(local_tc_socket_, "RAW" + std::to_string(ch) + ":SEND ON");
            }
            
            // Create output file for merged timestamps
            std::string output_file = (output_dir / ("slave_results_" + get_current_timestamp_str() + ".txt")).string();
            
            // Start the merging thread to combine incoming timestamps on the fly
            TimestampsMergerThread merger(stream_clients, output_file, static_cast<uint64_t>(pper_ps));
            merger.start();
            
            // Start the synchronized acquisition on the Time Controller
            log_message("Starting acquisition with REC:PLAY...");
            zmq_exec(local_tc_socket_, "REC:PLAY");
            
            // Wait for the specified duration
            std::this_thread::sleep_for(std::chrono::milliseconds(static_cast<int>(duration * 1000)));
            
            // Stop the acquisition
            log_message("Stopping local acquisition...");
            zmq_exec(local_tc_socket_, "REC:STOP");
            
            // Wait for data processing to complete
            log_message("Waiting for data processing to complete...");
            wait_end_of_timestamps_acquisition(local_tc_socket_, dlt, acquisitions_id, 30.0);
            
            // Stop the merger thread
            merger.join();
            
            // Stop and clean up stream clients
            for (BufferStreamClient* client : stream_clients) {
                client->join();
                delete client;
            }
            
            // Close active acquisitions
            close_active_acquisitions(dlt);
            
            log_message("Data collection completed successfully using working approach");
            
            // Convert the text output to binary format for compatibility
            if (fs::exists(output_file)) {
                log_message("Converting merged data to binary format...");
                
                // Generate output filename based on current timestamp
                fs::path slave_output_base = fs::path(config_.output_dir) / ("slave_results_" + get_current_timestamp_str());
                
                // Read the merged text file and convert to binary
                std::ifstream infile(output_file);
                std::string bin_filename = slave_output_base.string() + ".bin";
                std::ofstream bin_file(bin_filename, std::ios::binary);
                
                std::string line;
                int total_timestamps = 0;
                std::vector<uint64_t> all_timestamps;
                std::vector<int> all_channels;
                
                while (std::getline(infile, line)) {
                    // Skip empty lines
                    if (line.empty()) continue;
                    
                    try {
                        size_t semicolon_pos = line.find(';');
                        if (semicolon_pos != std::string::npos) {
                            std::string channel_str = line.substr(0, semicolon_pos);
                            std::string timestamp_str = line.substr(semicolon_pos + 1);
                            
                            // Trim whitespace
                            channel_str.erase(0, channel_str.find_first_not_of(" \t\r\n"));
                            channel_str.erase(channel_str.find_last_not_of(" \t\r\n") + 1);
                            timestamp_str.erase(0, timestamp_str.find_first_not_of(" \t\r\n"));
                            timestamp_str.erase(timestamp_str.find_last_not_of(" \t\r\n") + 1);
                            
                            // Validate strings are not empty
                            if (!channel_str.empty() && !timestamp_str.empty()) {
                                int channel = std::stoi(channel_str);
                                uint64_t timestamp = std::stoull(timestamp_str);
                                all_timestamps.push_back(timestamp);
                                all_channels.push_back(channel);
                                total_timestamps++;
                            }
                        }
                    } catch (const std::exception& e) {
                        log_message("WARNING: Failed to parse line: '" + line + "' - " + std::string(e.what()));
                        continue; // Skip invalid lines
                    }
                }
                infile.close();
                
                // Write binary data
                for (size_t i = 0; i < all_timestamps.size(); ++i) {
                    bin_file.write(reinterpret_cast<const char*>(&all_timestamps[i]), sizeof(uint64_t));
                    bin_file.write(reinterpret_cast<const char*>(&all_channels[i]), sizeof(int));
                }
                bin_file.close();
                
                log_message("Converted " + std::to_string(total_timestamps) + " timestamps to binary format");
                log_message("Saved slave timestamps to " + bin_filename);
                
                // Store data for master requests (don't send automatically)
                latest_timestamps_ = all_timestamps;
                latest_channels_ = all_channels;
                latest_bin_filename_ = bin_filename;
                latest_txt_filename_ = output_file;
                
                log_message("Slave data collection completed successfully");
                log_message("Data ready - waiting for master requests...");
            } else {
                log_message("WARNING: Output file not found: " + output_file);
            }
            
        } catch (const std::exception& e) {
            log_message("ERROR: Working data collection failed: " + std::string(e.what()));
            log_message("This may be due to DLT not responding to commands properly.");
            log_message("Falling back to direct Time Controller data collection...");
            
            // Fallback to direct TC data collection without DLT
            try {
                log_message("Using fallback data collection method...");
                
                // Collect data directly from Time Controller
                for (int ch : channels) {
                    log_message("Collecting timestamps from channel " + std::to_string(ch) + "...");
                    
                    // Get timestamp count
                    std::string count_str = zmq_exec(local_tc_socket_, "RAW" + std::to_string(ch) + ":DATA:COUNt?");
                    
                    // Trim whitespace from count string
                    count_str.erase(0, count_str.find_first_not_of(" \t\r\n"));
                    count_str.erase(count_str.find_last_not_of(" \t\r\n") + 1);
                    
                    int count = 0;
                    try {
                        if (!count_str.empty()) {
                            count = std::stoi(count_str);
                        }
                    } catch (const std::exception& e) {
                        log_message("ERROR: Failed to parse count '" + count_str + "' for channel " + std::to_string(ch) + ": " + std::string(e.what()));
                        continue;
                    }
                    
                    log_message("Collected " + std::to_string(count) + " timestamps from channel " + std::to_string(ch));
                    
                    if (count > 0) {
                        // Get the timestamp data
                        std::string data_str = zmq_exec(local_tc_socket_, "RAW" + std::to_string(ch) + ":DATA:VALue?");
                        
                        // Parse and save the data
                        std::vector<uint64_t> timestamps;
                        std::vector<int> channels_vec;
                        
                        std::istringstream iss(data_str);
                        std::string timestamp_str;
                        while (std::getline(iss, timestamp_str, ',')) {
                            // Trim whitespace
                            timestamp_str.erase(0, timestamp_str.find_first_not_of(" \t\r\n"));
                            timestamp_str.erase(timestamp_str.find_last_not_of(" \t\r\n") + 1);
                            
                            if (!timestamp_str.empty()) {
                                try {
                                    uint64_t timestamp = std::stoull(timestamp_str);
                                    timestamps.push_back(timestamp);
                                    channels_vec.push_back(ch);
                                } catch (const std::exception& e) {
                                    log_message("WARNING: Failed to parse timestamp '" + timestamp_str + "': " + std::string(e.what()));
                                    continue;
                                }
                            }
                        }
                        
                        // Generate output filename based on current timestamp
                        fs::path slave_output_base = fs::path(config_.output_dir) / ("slave_results_" + get_current_timestamp_str());
                        
                        // Save to binary file
                        std::string bin_filename = slave_output_base.string() + ".bin";
                        std::ofstream bin_file(bin_filename, std::ios::binary);
                        for (size_t i = 0; i < timestamps.size(); ++i) {
                            bin_file.write(reinterpret_cast<const char*>(&timestamps[i]), sizeof(uint64_t));
                            bin_file.write(reinterpret_cast<const char*>(&channels_vec[i]), sizeof(int));
                        }
                        bin_file.close();
                        
                        // Save to text file if requested
                        if (config_.text_output) {
                            std::string txt_filename = slave_output_base.string() + ".txt";
                            write_timestamps_to_txt(timestamps, channels_vec, txt_filename);
                            log_message("Saved timestamps in text format to " + txt_filename);
                        }
                        
                        log_message("Saved slave timestamps to " + bin_filename);
                        
                        // Send file to master
                        send_file_to_master(bin_filename);
                        
                        break; // Only process first channel with data for now
                    }
                }
                
                log_message("Fallback data collection completed successfully.");
                
            } catch (const std::exception& fallback_e) {
                log_message("ERROR: Fallback data collection also failed: " + std::string(fallback_e.what()));
                throw;
            }
        }
        
        log_message("Acquisition completed.");
        acquisition_active_ = false;
        
    } catch (const std::exception& e) {
        log_message("ERROR: Acquisition failed: " + std::string(e.what()));
        acquisition_active_ = false;
    }
}

json SlaveAgent::handle_partial_data_request() {
    json response;
    
    try {
        log_message("Handling partial data request for synchronization");
        
        if (latest_timestamps_.empty()) {
            response["status"] = "error";
            response["message"] = "No timestamp data available";
            return response;
        }
        
        // Calculate how many timestamps to send (10% of total)
        size_t count = std::max(size_t(1), latest_timestamps_.size() / 10);
        count = std::min(count, latest_timestamps_.size());
        
        // Extract the first 'count' timestamps
        std::vector<uint64_t> partial_timestamps(latest_timestamps_.begin(), 
                                               latest_timestamps_.begin() + count);
        
        response["status"] = "ok";
        response["timestamps"] = partial_timestamps;
        response["count"] = count;
        response["total"] = latest_timestamps_.size();
        
        log_message("Sending " + std::to_string(count) + " timestamps for synchronization");
    }
    catch (const std::exception& e) {
        response["status"] = "error";
        response["message"] = "Error processing partial data request: " + std::string(e.what());
    }
    
    return response;
}

void SlaveAgent::send_file_to_master(const std::string& filename) {
    try {
        log_message("Sending file to master: " + filename);
        
        // Read the file
        std::ifstream file(filename, std::ios::binary);
        if (!file) {
            throw std::runtime_error("Could not open file: " + filename);
        }
        
        // Get file size
        file.seekg(0, std::ios::end);
        size_t file_size = file.tellg();
        file.seekg(0, std::ios::beg);
        
        if (file_size == 0) {
            throw std::runtime_error("File is empty: " + filename);
        }
        
        // Read file content as binary
        std::vector<uint8_t> file_content(file_size);
        file.read(reinterpret_cast<char*>(file_content.data()), file_size);
        
        if (!file) {
            throw std::runtime_error("Failed to read file content: " + filename);
        }
        
        file.close();
        
        // Send the raw binary data directly (no JSON encoding)
        zmq::message_t msg(file_content.data(), file_size);
        auto result = file_socket_.send(msg, zmq::send_flags::none);
        
        if (result.has_value()) {
            log_message("File sent successfully");
        } else {
            throw std::runtime_error("Failed to send file to master");
        }
        
    } catch (const std::exception& e) {
        log_message("ERROR sending file to master: " + std::string(e.what()));
    }
}

void SlaveAgent::log_message(const std::string& message, bool verbose_only) {
    if (verbose_only && !config_.verbose_output) {
        return;
    }
    
    std::cout << message << std::endl;
}

std::string SlaveAgent::get_current_timestamp_str() {
    auto now = std::chrono::system_clock::now();
    auto now_time_t = std::chrono::system_clock::to_time_t(now);

    std::tm now_tm;
    localtime_r(&now_time_t, &now_tm);
    

    std::tm now_tm{};
    localtime_r(&now_time_t, &now_tm);

    std::stringstream ss;
    ss << std::put_time(&now_tm, "%Y%m%d_%H%M%S");
    return ss.str();
}

void SlaveAgent::write_timestamps_to_txt(const std::vector<uint64_t>& timestamps, 
                                        const std::vector<int>& channels,
                                        const std::string& filename) {
    try {
        std::ofstream txt_file(filename);
        if (!txt_file) {
            throw std::runtime_error("Failed to open file for writing: " + filename);
        }
        
        // Write header
        txt_file << "# Distributed Timestamp System - Slave Results" << std::endl;
        txt_file << "# Generated: ";
        auto now = std::chrono::system_clock::now();
        std::time_t now_time_t = std::chrono::system_clock::to_time_t(now);
        std::tm now_tm{};
        localtime_r(&now_time_t, &now_tm);
        txt_file << std::put_time(&now_tm, "%Y-%m-%d %H:%M:%S") << std::endl;
        txt_file << "# Time Controller: " << config_.slave_tc_address << std::endl;
        txt_file << "# Total timestamps: " << timestamps.size() << std::endl;
        txt_file << "#" << std::endl;
        txt_file << "# Index\tTimestamp (ns)\tChannel" << std::endl;
        
        // Write data
        for (size_t i = 0; i < timestamps.size(); ++i) {
            txt_file << i << "\t" << timestamps[i] << "\t" << channels[i] << std::endl;
        }
        
        txt_file.close();
        log_message("Saved timestamps in text format to " + filename);
    }
    catch (const std::exception& e) {
        throw std::runtime_error("Failed to write timestamps to text file: " + std::string(e.what()));
    }
}


void SlaveAgent::send_partial_data_to_master(const std::vector<uint64_t>& timestamps, const std::vector<int>& channels, int sequence) {
    try {
        log_message("Sending partial data to master (sequence " + std::to_string(sequence) + ")...");
        
        // Create a temporary partial data file
        std::string partial_filename = fs::path(config_.output_dir) / ("partial_data_" + std::to_string(sequence) + ".bin");
        std::ofstream partial_file(partial_filename, std::ios::binary);
        
        // Write partial data in binary format
        for (size_t i = 0; i < timestamps.size(); ++i) {
            partial_file.write(reinterpret_cast<const char*>(&timestamps[i]), sizeof(uint64_t));
            partial_file.write(reinterpret_cast<const char*>(&channels[i]), sizeof(int));
        }
        partial_file.close();
        
        log_message("Created partial data file: " + partial_filename + " (" + std::to_string(timestamps.size()) + " timestamps)");
        
        // Send the partial file to master
        send_file_to_master(partial_filename);
        
        // Clean up temporary file
        std::filesystem::remove(partial_filename);
        
        log_message("Partial data sent successfully (sequence " + std::to_string(sequence) + ")");
        
    } catch (const std::exception& e) {
        log_message("ERROR: Failed to send partial data: " + std::string(e.what()));
    }
}


void SlaveAgent::send_trigger_timestamp_to_master(uint64_t slave_trigger_timestamp, int sequence) {
    try {
        log_message("Sending trigger timestamp to master for synchronization...", true);
        
        // Create trigger timestamp message
        json trigger_ts_msg;
        trigger_ts_msg["command"] = "trigger_timestamp";
        trigger_ts_msg["slave_trigger_timestamp"] = slave_trigger_timestamp;
        trigger_ts_msg["sequence"] = sequence;
        
        std::string msg_str = trigger_ts_msg.dump();
        zmq::message_t msg(msg_str.size());
        memcpy(msg.data(), msg_str.c_str(), msg_str.size());
        
        // Send via sync socket (same as ready signal)
        sync_socket_.send(msg, zmq::send_flags::none);
        
        log_message("Trigger timestamp sent to master: " + std::to_string(slave_trigger_timestamp) + " ns", true);
        
    } catch (const std::exception& e) {
        log_message("ERROR: Failed to send trigger timestamp to master: " + std::string(e.what()));
    }
}

