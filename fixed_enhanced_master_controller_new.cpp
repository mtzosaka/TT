// Fixed Master Controller with improved exception handling and thread safety
#include "fixed_updated_master_controller.hpp"
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
#include <cmath>
#include <atomic>
#include <mutex>
#include <future>

namespace fs = std::filesystem;
using json = nlohmann::json;

MasterController::MasterController(const MasterConfig& config)
    : config_(config), running_(false), acquisition_active_(false), command_sequence_(0),
      slave_trigger_timestamp_ns_(0), calculated_offset_ns_(0), file_counter_(0) {
}

MasterController::~MasterController() {
    try {
        stop();
    } catch (...) {
        // Suppress all exceptions in destructor
    }
}

bool MasterController::initialize() {
    try {
        log_message("Initializing Master Controller...");
        log_message("Local Time Controller: " + config_.master_tc_address);
        log_message("Remote Slave: " + config_.slave_address);
        
        // Initialize ZeroMQ context and sockets
        log_message("Setting up communication channels...");
        context_ = zmq::context_t(1);
        
        // Socket for sending trigger commands to slave
        log_message("Creating trigger socket (PUB)...");
        trigger_socket_ = zmq::socket_t(context_, zmq::socket_type::pub);
        std::string trigger_endpoint = "tcp://*:" + std::to_string(config_.trigger_port);
        log_message("Binding trigger socket to: " + trigger_endpoint);
        trigger_socket_.bind(trigger_endpoint);
        log_message("Trigger socket bound");
        
        // Socket for receiving files from slave
        log_message("Creating file socket (PULL)...");
        file_socket_ = zmq::socket_t(context_, zmq::socket_type::pull);
        std::string file_endpoint = "tcp://*:" + std::to_string(config_.file_port);
        log_message("Binding file socket to: " + file_endpoint);
        file_socket_.bind(file_endpoint);
        log_message("File socket bound");

        // Socket for receiving status/heartbeat messages
        log_message("Creating status socket (PULL)...");
        status_socket_ = zmq::socket_t(context_, zmq::socket_type::pull);
        std::string status_endpoint = "tcp://*:" + std::to_string(config_.status_port);
        log_message("Binding status socket to: " + status_endpoint);
        status_socket_.bind(status_endpoint);
        log_message("Status socket bound");
        
        // Socket for sending commands to slave
        log_message("Creating command socket (REQ)...");
        command_socket_ = zmq::socket_t(context_, zmq::socket_type::req);
        std::string command_endpoint = "tcp://" + config_.slave_address + ":" + std::to_string(config_.command_port);
        log_message("Connecting command socket to: " + command_endpoint);
        command_socket_.connect(command_endpoint);
        log_message("Command socket connected");
        
        // Socket for receiving ready signals from slave
        log_message("Creating sync socket (PULL)...");
        sync_socket_ = zmq::socket_t(context_, zmq::socket_type::pull);
        std::string sync_endpoint = "tcp://*:" + std::to_string(config_.sync_port);
        log_message("Binding sync socket to: " + sync_endpoint);
        sync_socket_.bind(sync_endpoint);
        log_message("Sync socket bound");
        
        // Connect to local Time Controller
        log_message("Connecting to local Time Controller...");
        local_tc_socket_ = zmq::socket_t(context_, zmq::socket_type::req);
        std::string tc_endpoint = "tcp://" + config_.master_tc_address + ":5555";
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
        
        // Check slave availability
        log_message("Checking slave availability...");
        if (!check_slave_availability()) {
            log_message("ERROR: Slave is not available");
            return false;
        }
        
        // Start monitoring threads
        start_monitor_thread();
        start_file_receiver_thread();
        
        log_message("Master Controller initialized successfully.");
        return true;
    }
    catch (const std::exception& e) {
        log_message("ERROR: Failed to initialize Master Controller: " + std::string(e.what()));
        return false;
    }
}

void MasterController::stop() {
    try {
        if (running_) {
            log_message("Stopping Master Controller...");
            running_ = false;
            acquisition_active_ = false;
            
            // Add a longer delay to ensure threads see the updated running_ flag
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            
            // Stop threads with timeout
            if (monitor_thread_.joinable()) {
                try {
                    // Use a future to implement timeout for thread join
                    auto future = std::async(std::launch::async, [this]() {
                        monitor_thread_.join();
                    });
                    
                    if (future.wait_for(std::chrono::seconds(2)) == std::future_status::timeout) {
                        log_message("WARNING: Monitor thread join timed out");
                        monitor_thread_.detach();
                    } else {
                        log_message("Status monitor thread stopped");
                    }
                } catch (const std::exception& e) {
                    log_message("ERROR: Failed to join monitor thread: " + std::string(e.what()));
                    try {
                        monitor_thread_.detach();
                    } catch (...) {}
                }
            }
            
            if (file_receiver_thread_.joinable()) {
                try {
                    auto future = std::async(std::launch::async, [this]() {
                        file_receiver_thread_.join();
                    });
                    
                    if (future.wait_for(std::chrono::seconds(2)) == std::future_status::timeout) {
                        log_message("WARNING: File receiver thread join timed out");
                        file_receiver_thread_.detach();
                    } else {
                        log_message("File receiver thread stopped");
                    }
                } catch (const std::exception& e) {
                    log_message("ERROR: Failed to join file receiver thread: " + std::string(e.what()));
                    try {
                        file_receiver_thread_.detach();
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
            
            log_message("Master Controller stopped.");
        }
    } catch (const std::exception& e) {
        log_message("ERROR: Exception during stop: " + std::string(e.what()));
    } catch (...) {
        log_message("ERROR: Unknown exception during stop");
    }
}

        if (!resp_result.has_value()) {
            log_message("ERROR: No response from slave for request_ready command");
            acquisition_active_ = false;
            return false;
        }
        
        std::string resp_str(static_cast<char*>(response.data()), response.size());
        log_message("Slave response to request_ready: " + resp_str, true);
        
        // Now wait for the ready signal on the sync socket
        log_message("Waiting for slave to be ready on sync socket...", true);
        
        // Set a longer timeout for the sync socket
        int timeout = 10000; // 10 seconds timeout
        sync_socket_.set(zmq::sockopt::rcvtimeo, timeout);
        
        // Try multiple times to receive the ready signal
        bool ready_received = false;
        for (int retry = 0; retry < 5 && !ready_received; retry++) {
            try {
                zmq::message_t sync_msg;
                auto result = sync_socket_.recv(sync_msg);
                
                if (result.has_value()) {
                    std::string sync_data(static_cast<char*>(sync_msg.data()), sync_msg.size());
                    log_message("Received from slave on sync socket: " + sync_data, true);
                    
                    if (sync_data == "ready_for_trigger") {
                        log_message("Ready signal received successfully", true);
                        ready_received = true;
                        break;
                    } else {
                        log_message("WARNING: Unexpected message from slave: " + sync_data, true);
                    }
                } else {
                    log_message("WARNING: No message received on sync socket, retry " + std::to_string(retry+1), true);
                    
                    // Check slave status before retrying
                    json status_cmd;
                    status_cmd["command"] = "status";
                    status_cmd["sequence"] = command_sequence_++;
                    
                    std::string status_str = status_cmd.dump();
                    zmq::message_t status_msg(status_str.size());
                    memcpy(status_msg.data(), status_str.c_str(), status_str.size());
                    command_socket_.send(status_msg, zmq::send_flags::none);
                    
                    zmq::message_t status_resp;
                    auto status_result = command_socket_.recv(status_resp);
                    if (status_result.has_value()) {
                        std::string status_resp_str(static_cast<char*>(status_resp.data()), status_resp.size());
                        log_message("Slave status: " + status_resp_str, true);
                    }
                }
            }
            catch (const std::exception& e) {
                log_message("ERROR during sync: " + std::string(e.what()), true);
            }
            
            if (!ready_received && retry < 4) {
                log_message("Retrying ready signal request...", true);
                
                // Send another request_ready command
                json retry_cmd;
                retry_cmd["command"] = "request_ready";
                retry_cmd["sequence"] = command_sequence_++;
                retry_cmd["retry"] = retry + 1;
                
                std::string retry_str = retry_cmd.dump();
                zmq::message_t retry_msg(retry_str.size());
                memcpy(retry_msg.data(), retry_str.c_str(), retry_str.size());
                command_socket_.send(retry_msg, zmq::send_flags::none);
                
                zmq::message_t retry_resp;
                auto recv_result = command_socket_.recv(retry_resp);
                if (!recv_result.has_value()) {
                    log_message("WARNING: No response to retry request", true);
                }
                
                // Brief pause before retrying
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
            }
        }
        
        if (!ready_received) {
            log_message("ERROR: Timeout waiting for slave to be ready");
            acquisition_active_ = false;
            return false;
        }
        
        // Send trigger to slave
        log_message("Sending trigger to slave...");
        
        // Get current timestamp for synchronization
        auto now = std::chrono::high_resolution_clock::now();
        auto now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
            now.time_since_epoch()).count();
        
        // Prepare trigger message with timestamp
        json trigger_msg;
        trigger_msg["command"] = "trigger";
        trigger_msg["timestamp"] = now_ns;
        trigger_msg["sequence"] = command_sequence_++;
        trigger_msg["duration"] = duration;
        trigger_msg["channels"] = channels;
        
        std::string trigger_str = trigger_msg.dump();
        zmq::message_t trigger(trigger_str.size());
        memcpy(trigger.data(), trigger_str.c_str(), trigger_str.size());
        trigger_socket_.send(trigger, zmq::send_flags::none);
        
        // Store the trigger timestamp for later synchronization
        slave_trigger_timestamp_ns_ = now_ns;
        
        // Start local acquisition
        log_message("Starting local acquisition...");
        
        // Configure the Time Controller for acquisition
        configure_timestamps_references(local_tc_socket_, channels);
        
        // Set up acquisition parameters
        for (int ch : channels) {
            zmq_exec(local_tc_socket_, "RAW" + std::to_string(ch) + ":SEND ON");
        }
        
        // Start acquisition for specified duration
        log_message("Acquisition in progress for " + std::to_string(duration) + " seconds...");
        zmq_exec(local_tc_socket_, "REC:STARt");
        
        // Wait for the specified duration
        std::this_thread::sleep_for(std::chrono::milliseconds(static_cast<int>(duration * 1000)));
        
        // Stop acquisition
        log_message("Stopping local acquisition...");
        zmq_exec(local_tc_socket_, "REC:STOP");
        
        // Process and save the data
        log_message("Waiting for data processing to complete...");
        
        // Generate output filename based on current timestamp
        fs::path master_output_base = fs::path(config_.output_dir) / ("master_results_" + get_current_timestamp_str());
        
        // Collect timestamps from local Time Controller
        std::map<int, std::vector<uint64_t>> master_timestamps;
        std::map<int, std::vector<int>> master_channels;
        
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
                master_timestamps[ch] = timestamps;
                master_channels[ch] = channel_ids;
                
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
            for (const auto& [ch, timestamps] : master_timestamps) {
                if (timestamps.empty()) {
                    log_message("WARNING: No timestamps collected for channel " + std::to_string(ch));
                    continue;
                }
                
                all_timestamps.insert(all_timestamps.end(), timestamps.begin(), timestamps.end());
                
                // Ensure we have matching channel IDs
                if (master_channels[ch].size() != timestamps.size()) {
                    log_message("WARNING: Channel ID count mismatch for channel " + std::to_string(ch) + 
                               ". Using default channel ID.");
                    
                    // Use default channel ID
                    std::vector<int> default_channels(timestamps.size(), ch);
                    all_channels.insert(all_channels.end(), default_channels.begin(), default_channels.end());
                } else {
                    all_channels.insert(all_channels.end(), master_channels[ch].begin(), master_channels[ch].end());
                }
            }
            
            if (all_timestamps.empty()) {
                log_message("WARNING: No timestamps collected from any channel");
                acquisition_active_ = false;
                return false;
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
            std::string bin_filename = master_output_base.string() + ".bin";
            
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
                log_message("Saved master timestamps to " + bin_filename);
                
                // If text output is enabled, also save as text
                if (config_.text_output) {
                    try {
                        write_timestamps_to_txt(sorted_timestamps, sorted_channels, master_output_base.string() + ".txt");
                    }
                    catch (const std::exception& e) {
                        log_message("ERROR: Failed to write text output: " + std::string(e.what()));
                    }
                }
            }
            catch (const std::exception& e) {
                log_message("ERROR: Failed to save binary file: " + std::string(e.what()));
            }
            
            // Request partial data from slave for synchronization
            log_message("Requesting partial data from slave for synchronization...", true);
            
            try {
                json sync_request;
                sync_request["command"] = "get_partial_data";
                sync_request["sequence"] = command_sequence_++;
                
                std::string sync_req_str = sync_request.dump();
                zmq::message_t sync_req(sync_req_str.size());
                memcpy(sync_req.data(), sync_req_str.c_str(), sync_req_str.size());
                command_socket_.send(sync_req, zmq::send_flags::none);
                
                // Wait for response
                zmq::message_t sync_response;
                auto sync_result = command_socket_.recv(sync_response, zmq::recv_flags::none);
                
                if (!sync_result.has_value()) {
                    log_message("ERROR: No response from slave for partial data request");
                } else {
                    std::string sync_resp_str(static_cast<char*>(sync_response.data()), sync_response.size());
                    json sync_resp_json = json::parse(sync_resp_str);
                    
                    if (!sync_resp_json.contains("status") || sync_resp_json["status"] != "ok") {
                        log_message("ERROR: Failed to get partial data from slave");
                    } else {
                        // Calculate synchronization offset
                        if (sync_resp_json.contains("timestamps") && sync_resp_json["timestamps"].is_array() && 
                            !sync_resp_json["timestamps"].empty()) {
                            
                            std::vector<uint64_t> slave_timestamps = sync_resp_json["timestamps"].get<std::vector<uint64_t>>();
                            
                            if (!slave_timestamps.empty() && !sorted_timestamps.empty()) {
                                // Calculate offset between master and slave timestamps
                                // Use first 10% of timestamps for calculation
                                size_t sync_count = std::min(slave_timestamps.size(), sorted_timestamps.size());
                                sync_count = std::max(size_t(1), sync_count / 10);  // Use 10% of timestamps
                                
                                std::vector<int64_t> offsets;
                                for (size_t i = 0; i < sync_count; ++i) {
                                    int64_t offset = static_cast<int64_t>(slave_timestamps[i]) - static_cast<int64_t>(sorted_timestamps[i]);
                                    offsets.push_back(offset);
                                }
                                
                                // Calculate statistics
                                double sum = std::accumulate(offsets.begin(), offsets.end(), 0.0);
                                double mean = sum / offsets.size();
                                
                                std::vector<double> diff(offsets.size());
                                std::transform(offsets.begin(), offsets.end(), diff.begin(),
                                               [mean](double x) { return x - mean; });
                                
                                double sq_sum = std::inner_product(diff.begin(), diff.end(), diff.begin(), 0.0);
                                double std_dev = std::sqrt(sq_sum / offsets.size());
                                
                                int64_t min_offset = *std::min_element(offsets.begin(), offsets.end());
                                int64_t max_offset = *std::max_element(offsets.begin(), offsets.end());
                                
                                // Store the calculated offset
                                calculated_offset_ns_ = static_cast<int64_t>(mean);
                                
                                // Log synchronization results
                                log_message("Synchronization offset calculated:");
                                log_message("  Average offset: " + std::to_string(mean) + " ns");
                                log_message("  Min offset: " + std::to_string(min_offset) + " ns");
                                log_message("  Max offset: " + std::to_string(max_offset) + " ns");
                                log_message("  Standard deviation: " + std::to_string(std_dev) + " ns");
                                
                                try {
                                    // Create corrected master timestamps
                                    std::vector<uint64_t> corrected_timestamps(sorted_timestamps.size());
                                    for (size_t i = 0; i < sorted_timestamps.size(); ++i) {
                                        corrected_timestamps[i] = sorted_timestamps[i] + calculated_offset_ns_;
                                    }
                                    
                                    // Save corrected timestamps
                                    std::string corrected_bin_filename = master_output_base.string() + "_corrected.bin";
                                    std::ofstream corrected_bin_file(corrected_bin_filename, std::ios::binary);
                                    
                                    if (!corrected_bin_file) {
                                        throw std::runtime_error("Failed to open file for writing: " + corrected_bin_filename);
                                    }
                                    
                                    // Write header - FIX: Define num_timestamps here
                                    uint32_t corrected_num_timestamps = static_cast<uint32_t>(corrected_timestamps.size());
                                    corrected_bin_file.write(reinterpret_cast<char*>(&corrected_num_timestamps), sizeof(corrected_num_timestamps));
                                    
                                    // Write timestamps and channels
                                    for (size_t i = 0; i < corrected_timestamps.size(); ++i) {
                                        corrected_bin_file.write(reinterpret_cast<char*>(&corrected_timestamps[i]), sizeof(uint64_t));
                                        corrected_bin_file.write(reinterpret_cast<char*>(&sorted_channels[i]), sizeof(int));
                                    }
                                    
                                    corrected_bin_file.close();
                                    log_message("Saved corrected master timestamps to " + corrected_bin_filename);
                                    
                                    // If text output is enabled, also save corrected timestamps as text
                                    if (config_.text_output) {
                                        try {
                                            write_timestamps_to_txt(corrected_timestamps, sorted_channels, master_output_base.string() + "_corrected.txt");
                                        }
                                        catch (const std::exception& e) {
                                            log_message("ERROR: Failed to write corrected text output: " + std::string(e.what()));
                                        }
                                    }
                                    
                                    // Write offset report
                                    try {
                                        std::string offset_report_filename = master_output_base.string() + "_offset_report.txt";
                                        write_offset_report(offset_report_filename, mean, min_offset, max_offset, std_dev, 
                                                          (max_offset - min_offset) / std::abs(mean));
                                    }
                                    catch (const std::exception& e) {
                                        log_message("ERROR: Failed to write offset report: " + std::string(e.what()));
                                    }
                                }
                                catch (const std::exception& e) {
                                    log_message("ERROR: Failed to save corrected timestamps: " + std::string(e.what()));
                                }
                            }
                        }
                    }
                }
            }
            catch (const std::exception& e) {
                log_message("ERROR: Failed during synchronization: " + std::string(e.what()));
            }
        }
        catch (const std::exception& e) {
            log_message("ERROR: Failed during timestamp processing: " + std::string(e.what()));
        }
        
        log_message("Acquisition completed successfully.");
        acquisition_active_ = false;
        return true;
    }
    catch (const std::exception& e) {
        log_message("ERROR: Acquisition failed: " + std::string(e.what()));
        acquisition_active_ = false;
        return false;
    }
}

void MasterController::start_monitor_thread() {
    monitor_thread_ = std::thread([this]() {
        log_message("Status monitor thread started");
        running_ = true;
        
        while (running_) {
            try {
                // Check slave status periodically
                if (acquisition_active_) {
                    json status_cmd;
                    status_cmd["command"] = "status";
                    status_cmd["sequence"] = command_sequence_++;
                    
                    std::string cmd_str = status_cmd.dump();
                    zmq::message_t cmd_msg(cmd_str.size());
                    memcpy(cmd_msg.data(), cmd_str.c_str(), cmd_str.size());
                    
                    // Set timeout for sending/receiving
                    command_socket_.set(zmq::sockopt::sndtimeo, 1000);
                    command_socket_.set(zmq::sockopt::rcvtimeo, 1000);
                    
                    // Send status request
                    auto send_result = command_socket_.send(cmd_msg, zmq::send_flags::none);
                    
                    if (send_result.has_value()) {
                        // Wait for response
                        zmq::message_t response;
                        auto recv_result = command_socket_.recv(response);
                        
                        if (recv_result.has_value()) {
                            std::string resp_str(static_cast<char*>(response.data()), response.size());
                            json resp_json = json::parse(resp_str);
                            
                            if (resp_json.contains("status")) {
                                std::string status = resp_json["status"].get<std::string>();
                                log_message("Slave status: " + status, true);
                            }
                        }
                    }
                }
            }
            catch (const std::exception& e) {
                // Ignore errors in monitoring thread
            }
            
            // Sleep for a bit
            std::this_thread::sleep_for(std::chrono::milliseconds(1000));
        }
    });
}

void MasterController::start_file_receiver_thread() {
    file_receiver_thread_ = std::thread([this]() {
        log_message("File receiver thread started");
        running_ = true;
        
        while (running_) {
            try {
                // Set timeout for receiving files
                file_socket_.set(zmq::sockopt::rcvtimeo, 1000);
                
                zmq::message_t file_msg;
                auto result = file_socket_.recv(file_msg);
                
                if (result.has_value() && file_msg.size() > 0) {
                    // Save the raw binary message as a file
                    std::string filename = "slave_file_" + std::to_string(file_counter_++) + ".bin";
                    fs::path output_path = fs::path(config_.output_dir) / filename;
                    try {
                        std::ofstream outfile(output_path, std::ios::binary);
                        if (!outfile) {
                            throw std::runtime_error("Failed to open file for writing: " + output_path.string());
                        }
                        outfile.write(static_cast<char*>(file_msg.data()), file_msg.size());
                        outfile.close();
                        log_message("Received file from slave: " + filename);
                    } catch (const std::exception& e) {
                        log_message("ERROR: Failed to save received file: " + std::string(e.what()));
                    }
                }
            }
            catch (const std::exception& e) {
                // Only log if it's not a timeout
                if (std::string(e.what()).find("Resource temporarily unavailable") == std::string::npos) {
                    log_message("File receiver error: " + std::string(e.what()), true);
                }
            }
            
            // Brief pause to avoid busy waiting
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    });
}

bool MasterController::check_slave_availability() {
    try {
        json ping_cmd;
        ping_cmd["command"] = "ping";
        ping_cmd["sequence"] = 0;
        
        std::string cmd_str = ping_cmd.dump();
        zmq::message_t cmd_msg(cmd_str.size());
        memcpy(cmd_msg.data(), cmd_str.c_str(), cmd_str.size());
        
        // Set timeout for sending/receiving
        command_socket_.set(zmq::sockopt::sndtimeo, 2000);
        command_socket_.set(zmq::sockopt::rcvtimeo, 2000);
        
        // Send ping request
        auto send_result = command_socket_.send(cmd_msg, zmq::send_flags::none);
        
        if (!send_result.has_value()) {
            log_message("ERROR: Failed to send ping to slave");
            return false;
        }
        
        // Wait for response
        zmq::message_t response;
        auto recv_result = command_socket_.recv(response);
        
        if (!recv_result.has_value()) {
            log_message("ERROR: No response from slave");
            return false;
        }
        
        std::string resp_str(static_cast<char*>(response.data()), response.size());
        json resp_json = json::parse(resp_str);
        
        if (!resp_json.contains("status") || resp_json["status"] != "ok") {
            log_message("ERROR: Invalid response from slave");
            return false;
        }
        
        return true;
    }
    catch (const std::exception& e) {
        log_message("ERROR: Failed to check slave availability: " + std::string(e.what()));
        return false;
    }
}

void MasterController::log_message(const std::string& message, bool verbose_only) {
    if (verbose_only && !config_.verbose_output) {
        return;
    }
    
    std::cout << message << std::endl;
}

std::string MasterController::get_current_timestamp_str() {
    auto now = std::chrono::system_clock::now();
    auto now_time_t = std::chrono::system_clock::to_time_t(now);
    std::tm now_tm;
    localtime_r(&now_time_t, &now_tm);

    std::stringstream ss;
    ss << std::put_time(&now_tm, "%Y%m%d_%H%M%S");
    return ss.str();
}

void MasterController::write_timestamps_to_txt(const std::vector<uint64_t>& timestamps, 
                                             const std::vector<int>& channels,
                                             const std::string& filename) {
    try {
        std::ofstream txt_file(filename);
        if (!txt_file) {
            throw std::runtime_error("Failed to open file for writing: " + filename);
        }
        
        // Write header
        txt_file << "# Distributed Timestamp System - Master Results" << std::endl;
        txt_file << "# Generated: ";
        auto now = std::chrono::system_clock::now();
        std::time_t now_time_t = std::chrono::system_clock::to_time_t(now);
        std::tm* now_tm = std::localtime(&now_time_t);
        txt_file << std::put_time(now_tm, "%Y-%m-%d %H:%M:%S") << std::endl;
        txt_file << "# Time Controller: " << config_.master_tc_address << std::endl;
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

void MasterController::write_offset_report(const std::string& filename, double mean_offset, 
                                          double min_offset, double max_offset, 
                                          double std_dev, double relative_spread) {
    try {
        std::ofstream report_file(filename);
        if (!report_file) {
            throw std::runtime_error("Failed to open file for writing: " + filename);
        }
        
        // Write header
        report_file << "# Distributed Timestamp System - Synchronization Offset Report" << std::endl;
        report_file << "# Generated: ";
        auto now = std::chrono::system_clock::now();
        std::time_t now_time_t = std::chrono::system_clock::to_time_t(now);
        std::tm* now_tm = std::localtime(&now_time_t);
        report_file << std::put_time(now_tm, "%Y-%m-%d %H:%M:%S") << std::endl;
        report_file << "# Master Time Controller: " << config_.master_tc_address << std::endl;
        report_file << "# Slave Time Controller: " << config_.slave_address << std::endl;
        report_file << "#" << std::endl;
        report_file << "# Synchronization Offset Statistics:" << std::endl;
        report_file << "# Mean Offset: " << mean_offset << " ns" << std::endl;
        report_file << "# Min Offset: " << min_offset << " ns" << std::endl;
        report_file << "# Max Offset: " << max_offset << " ns" << std::endl;
        report_file << "# Standard Deviation: " << std_dev << " ns" << std::endl;
        report_file << "# Relative Spread: " << relative_spread << std::endl;
        report_file << "#" << std::endl;
        report_file << "# Quality Assessment:" << std::endl;
        
        // Assess synchronization quality
        std::string quality;
        if (std_dev < 10.0) {
            quality = "Excellent";
        }
        else if (std_dev < 50.0) {
            quality = "Good";
        }
        else if (std_dev < 100.0) {
            quality = "Fair";
        }
        else {
            quality = "Poor";
        }
        
        report_file << "# Synchronization Quality: " << quality << std::endl;
        
        report_file.close();
        log_message("Saved offset report to " + filename);
    }
    catch (const std::exception& e) {
        throw std::runtime_error("Failed to write offset report: " + std::string(e.what()));
    }
}
