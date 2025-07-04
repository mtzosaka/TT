// Fixed Slave Agent with improved exception handling and thread safety
#include "fixed_updated_slave_agent.hpp"
#include "common.hpp"
#include "streams.hpp"
#include <iostream>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <chrono>
#include <thread>
#include <filesystem>
#include <algorithm>
#include <numeric>
#include <vector>
#include <atomic>
#include <mutex>
#include <future>

namespace fs = std::filesystem;
using json = nlohmann::json;

SlaveAgent::SlaveAgent(const SlaveConfig& config)
    : config_(config), running_(false), acquisition_active_(false), command_sequence_(0) {
}

SlaveAgent::~SlaveAgent() {
    try {
        stop();
    } catch (...) {
        // Suppress all exceptions in destructor
    }
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
        
        // Socket for sending ready signals to master
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
        local_tc_socket_ = zmq::socket_t(context_, zmq::socket_type::req);
        std::string tc_endpoint = "tcp://" + config_.slave_tc_address + ":5555";
        local_tc_socket_.connect(tc_endpoint);
        
        // Identify the Time Controller
        std::string id_command = "id";
        zmq::message_t id_msg(id_command.size());
        memcpy(id_msg.data(), id_command.c_str(), id_command.size());
        local_tc_socket_.send(id_msg, zmq::send_flags::none);
        
        zmq::message_t id_response;
        local_tc_socket_.recv(id_response, zmq::recv_flags::none);
        std::string id_resp(static_cast<char*>(id_response.data()), id_response.size());
        log_message("Local Time Controller identified: " + id_resp);
        
        // Start threads
        start_command_handler_thread();
        start_trigger_listener_thread();
        start_heartbeat_thread();
        
        log_message("Slave Agent initialized successfully.");
        return true;
    }
    catch (const std::exception& e) {
        log_message("ERROR: Failed to initialize Slave Agent: " + std::string(e.what()));
        return false;
    }
}

void SlaveAgent::stop() {
    try {
        if (running_) {
            log_message("Stopping Slave Agent...");
            running_ = false;
            acquisition_active_ = false;
            
            // Add a longer delay to ensure threads see the updated running_ flag
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            
            // Stop threads with timeout
            if (trigger_thread_.joinable()) {
                try {
                    auto future = std::async(std::launch::async, [this]() {
                        trigger_thread_.join();
                    });
                    
                    if (future.wait_for(std::chrono::seconds(2)) == std::future_status::timeout) {
                        log_message("WARNING: Trigger thread join timed out");
                        trigger_thread_.detach();
                    } else {
                        log_message("Trigger listener thread stopped");
                    }
                } catch (const std::exception& e) {
                    log_message("ERROR: Failed to join trigger thread: " + std::string(e.what()));
                    try {
                        trigger_thread_.detach();
                    } catch (...) {}
                }
            }
            
            if (command_thread_.joinable()) {
                try {
                    auto future = std::async(std::launch::async, [this]() {
                        command_thread_.join();
                    });
                    
                    if (future.wait_for(std::chrono::seconds(2)) == std::future_status::timeout) {
                        log_message("WARNING: Command thread join timed out");
                        command_thread_.detach();
                    } else {
                        log_message("Command handler thread stopped");
                    }
                } catch (const std::exception& e) {
                    log_message("ERROR: Failed to join command thread: " + std::string(e.what()));
                    try {
                        command_thread_.detach();
                    } catch (...) {}
                }
            }
            
            if (heartbeat_thread_.joinable()) {
                try {
                    auto future = std::async(std::launch::async, [this]() {
                        heartbeat_thread_.join();
                    });
                    
                    if (future.wait_for(std::chrono::seconds(2)) == std::future_status::timeout) {
                        log_message("WARNING: Heartbeat thread join timed out");
                        heartbeat_thread_.detach();
                    } else {
                        log_message("Heartbeat thread stopped");
                    }
                } catch (const std::exception& e) {
                    log_message("ERROR: Failed to join heartbeat thread: " + std::string(e.what()));
                    try {
                        heartbeat_thread_.detach();
                    } catch (...) {}
                }
            }
            
            // Close sockets with proper error handling
            try {
                if (trigger_socket_.handle() != nullptr) {
                    trigger_socket_.set(zmq::sockopt::linger, 0);
                    trigger_socket_.close();
                }
            } catch (...) {}
            
            try {
                if (file_socket_.handle() != nullptr) {
                    file_socket_.set(zmq::sockopt::linger, 0);
                    file_socket_.close();
                }
            } catch (...) {}

            try {
                if (status_socket_.handle() != nullptr) {
                    status_socket_.set(zmq::sockopt::linger, 0);
                    status_socket_.close();
                }
            } catch (...) {}
            
            try {
                if (command_socket_.handle() != nullptr) {
                    command_socket_.set(zmq::sockopt::linger, 0);
                    command_socket_.close();
                }
            } catch (...) {}
            
            try {
                if (sync_socket_.handle() != nullptr) {
                    sync_socket_.set(zmq::sockopt::linger, 0);
                    sync_socket_.close();
                }
            } catch (...) {}
            
            try {
                if (local_tc_socket_.handle() != nullptr) {
                    local_tc_socket_.set(zmq::sockopt::linger, 0);
                    local_tc_socket_.close();
                }
            } catch (...) {}
            
            log_message("Slave Agent stopped.");
        }
    } catch (const std::exception& e) {
        log_message("ERROR: Exception during stop: " + std::string(e.what()));
    } catch (...) {
        log_message("ERROR: Unknown exception during stop");
    }
}

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
                            else if (command == "get_partial_data") {
                                // Send partial data for synchronization
                                response = handle_partial_data_request();
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
        
        // Start acquisition for specified duration
        zmq_exec(local_tc_socket_, "REC:STARt");
        
        // Wait for the specified duration
        std::this_thread::sleep_for(std::chrono::milliseconds(static_cast<int>(duration * 1000)));
        
        // Stop acquisition
        log_message("Stopping local acquisition...");
        zmq_exec(local_tc_socket_, "REC:STOP");
        
        // Process and save the data
        log_message("Processing acquired data...");
        
        // Collect timestamps from local Time Controller
        std::map<int, std::vector<uint64_t>> slave_timestamps;
        std::map<int, std::vector<int>> slave_channels;
        
        // Process each channel
        for (int ch : channels) {
            try {
                // Get timestamps from Time Controller
                std::vector<uint64_t> timestamps;
                std::vector<int> channel_ids;
                
                // Use the BufferStreamClient to get real data
                BufferStreamClient client(local_tc_socket_, ch);
                client.start();
                
                // Wait for data collection to complete
                while (client.is_running()) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                }
                
                // Get the collected timestamps
                timestamps = client.get_timestamps();
                channel_ids = client.get_channels();
                
                // Store the timestamps for this channel
                slave_timestamps[ch] = timestamps;
                slave_channels[ch] = channel_ids;
                
                log_message("Collected " + std::to_string(timestamps.size()) + " timestamps from channel " + std::to_string(ch), true);
            }
            catch (const std::exception& e) {
                log_message("ERROR: Failed to collect timestamps from channel " + std::to_string(ch) + ": " + std::string(e.what()));
            }
        }
        
        // Combine all timestamps into a single vector for saving
        std::vector<uint64_t> all_timestamps;
        std::vector<int> all_channels;
        
        try {
            for (const auto& [ch, timestamps] : slave_timestamps) {
                if (timestamps.empty()) {
                    log_message("WARNING: No timestamps collected for channel " + std::to_string(ch));
                    continue;
                }
                
                all_timestamps.insert(all_timestamps.end(), timestamps.begin(), timestamps.end());
                
                // Ensure we have matching channel IDs
                if (slave_channels[ch].size() != timestamps.size()) {
                    log_message("WARNING: Channel ID count mismatch for channel " + std::to_string(ch) + 
                               ". Using default channel ID.");
                    
                    // Use default channel ID
                    std::vector<int> default_channels(timestamps.size(), ch);
                    all_channels.insert(all_channels.end(), default_channels.begin(), default_channels.end());
                } else {
                    all_channels.insert(all_channels.end(), slave_channels[ch].begin(), slave_channels[ch].end());
                }
            }
            
            if (all_timestamps.empty()) {
                log_message("WARNING: No timestamps collected from any channel");
                acquisition_active_ = false;
                return;
            }
            
            // Sort timestamps by time
            std::vector<size_t> indices(all_timestamps.size());
            std::iota(indices.begin(), indices.end(), 0);
            std::sort(indices.begin(), indices.end(),
                     [&all_timestamps](size_t i1, size_t i2) {
                         return all_timestamps[i1] < all_timestamps[i2];
                     });
            
            std::vector<uint64_t> sorted_timestamps(all_timestamps.size());
            std::vector<int> sorted_channels(all_channels.size());
            
            for (size_t i = 0; i < indices.size(); ++i) {
                sorted_timestamps[i] = all_timestamps[indices[i]];
                sorted_channels[i] = all_channels[indices[i]];
            }
            
            // Save timestamps to binary file
            std::string bin_filename = slave_output_base.string() + ".bin";
            
            try {
                std::ofstream bin_file(bin_filename, std::ios::binary);
                if (!bin_file) {
                    throw std::runtime_error("Failed to open file for writing: " + bin_filename);
                }
                
                // Write header
                uint32_t num_timestamps = static_cast<uint32_t>(sorted_timestamps.size());
                bin_file.write(reinterpret_cast<char*>(&num_timestamps), sizeof(num_timestamps));
                
                // Write timestamps and channels
                for (size_t i = 0; i < sorted_timestamps.size(); ++i) {
                    bin_file.write(reinterpret_cast<char*>(&sorted_timestamps[i]), sizeof(uint64_t));
                    bin_file.write(reinterpret_cast<char*>(&sorted_channels[i]), sizeof(int));
                }
                
                bin_file.close();
                log_message("Saved slave timestamps to " + bin_filename);
                
                // If text output is enabled, also save as text
                if (config_.text_output) {
                    try {
                        write_timestamps_to_txt(sorted_timestamps, sorted_channels, slave_output_base.string() + ".txt");
                    }
                    catch (const std::exception& e) {
                        log_message("ERROR: Failed to write text output: " + std::string(e.what()));
                    }
                }
                
                // Store the timestamps for partial data requests
                latest_timestamps_ = sorted_timestamps;
                latest_channels_ = sorted_channels;
                
                // Send the file to master
                try {
                    send_file_to_master(bin_filename);
                }
                catch (const std::exception& e) {
                    log_message("ERROR: Failed to send file to master: " + std::string(e.what()));
                }
            }
            catch (const std::exception& e) {
                log_message("ERROR: Failed to save binary file: " + std::string(e.what()));
            }
        }
        catch (const std::exception& e) {
            log_message("ERROR: Failed during timestamp processing: " + std::string(e.what()));
        }
        
        log_message("Acquisition completed.");
        acquisition_active_ = false;
    }
    catch (const std::exception& e) {
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
        
        // Send the raw binary file content directly
        zmq::message_t message(file_size);
        memcpy(message.data(), file_content.data(), file_size);
        
        auto send_result = file_socket_.send(message, zmq::send_flags::none);
        if (!send_result.has_value()) {
            throw std::runtime_error("Failed to send file data");
        }
        
        log_message("File sent successfully");
    }
    catch (const std::exception& e) {
        throw std::runtime_error("Failed to send file: " + std::string(e.what()));
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
        std::tm now_tm;
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
