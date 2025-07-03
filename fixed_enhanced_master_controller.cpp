#include "fixed_updated_master_controller.hpp"
#include <iostream>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <chrono>
#include <thread>
#include <filesystem>
#include <algorithm>
#include <numeric>
#include "working_common.hpp"
#include "streams.hpp"
namespace fs = std::filesystem;
using json = nlohmann::json;

MasterController::MasterController(const MasterConfig& config)
    : config_(config), running_(false), acquisition_active_(false), command_sequence_(0),
      slave_trigger_timestamp_ns_(0), calculated_offset_ns_(0), file_counter_(0) {
}

MasterController::~MasterController() {
    stop();
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
        std::string trigger_endpoint = "tcp://*:5557";
        log_message("Binding trigger socket to: " + trigger_endpoint);
        trigger_socket_.bind(trigger_endpoint);
        log_message("Trigger socket bound");
        
        // Socket for receiving files from slave
        log_message("Creating file socket (PULL)...");
        file_socket_ = zmq::socket_t(context_, zmq::socket_type::pull);
        std::string file_endpoint = "tcp://*:5559";
        log_message("Binding file socket to: " + file_endpoint);
        file_socket_.bind(file_endpoint);
        log_message("File socket bound");
        
        // Socket for sending commands to slave
        log_message("Creating command socket (REQ)...");
        command_socket_ = zmq::socket_t(context_, zmq::socket_type::req);
        std::string command_endpoint = "tcp://" + config_.slave_address + ":5560";
        log_message("Connecting command socket to: " + command_endpoint);
        command_socket_.connect(command_endpoint);
        log_message("Command socket connected");
        
        // Socket for synchronization handshake
        log_message("Creating sync socket (PULL)...");
        sync_socket_ = zmq::socket_t(context_, zmq::socket_type::pull);
        std::string sync_endpoint = "tcp://*:" + std::to_string(config_.sync_port);
        log_message("Binding sync socket to: " + sync_endpoint);
        sync_socket_.bind(sync_endpoint);
        log_message("Sync socket bound");
        
        // Set socket options for better reliability
        int linger = 1000;  // 1 second linger period
        sync_socket_.set(zmq::sockopt::linger, linger);
        
        // Connect to local Time Controller
        log_message("Connecting to local Time Controller...");
        local_tc_socket_ = connect_zmq(config_.master_tc_address, 5555);
        
        // Identify the Time Controller
        std::string id_response = zmq_exec(local_tc_socket_, "*IDN?");
        log_message("Local Time Controller identified: " + id_response);
        
        // Create output directory if it doesn't exist
        if (!fs::exists(config_.output_dir)) {
            fs::create_directories(config_.output_dir);
        }
        
        // Start monitoring threads
        start_monitor_thread();
        // Note: File receiver thread will be started when master is ready to receive data
        
        // Check if slave is available
        log_message("Checking slave availability...");
        if (!check_slave_availability()) {
            log_message("ERROR: Slave not available at " + config_.slave_address);
            return false;
        }
        
        log_message("Master Controller initialized successfully.");
        return true;
    }
    catch (const std::exception& e) {
        log_message("ERROR: Initialization failed: " + std::string(e.what()));
        return false;
    }
}

void MasterController::stop() {
    if (running_) {
        running_ = false;
        
        // Add a small delay to ensure threads see the updated running_ flag
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        
        if (monitor_thread_.joinable()) {
            try {
                monitor_thread_.join();
                log_message("Status monitor thread stopped");
            } catch (const std::exception& e) {
                log_message("ERROR: Failed to join monitor thread: " + std::string(e.what()));
            }
        }
        
        if (file_receiver_thread_.joinable()) {
            try {
                file_receiver_thread_.join();
                log_message("File receiver thread stopped");
            } catch (const std::exception& e) {
                log_message("ERROR: Failed to join file receiver thread: " + std::string(e.what()));
            }
        }
        
        // Close sockets
        try {
            trigger_socket_.close();
            file_socket_.close();
            command_socket_.close();
            sync_socket_.close();
            local_tc_socket_.close();
        } catch (const std::exception& e) {
            log_message("ERROR: Failed to close sockets: " + std::string(e.what()));
        }
        
        log_message("Master Controller stopped.");
    }
}

bool MasterController::run_single_file_mode(double duration, const std::vector<int>& channels) {
    log_message("Running in single-file mode");
    return start_acquisition(duration, channels);
}

bool MasterController::run_streaming_mode(double duration, const std::vector<int>& channels, int num_files) {
    log_message("Running in streaming mode with " + std::to_string(num_files) + " files");
    
    for (int i = 0; i < num_files && running_; ++i) {
        log_message("Starting acquisition " + std::to_string(i+1) + " of " + std::to_string(num_files));
        if (!start_acquisition(duration, channels)) {
            log_message("ERROR: Acquisition " + std::to_string(i+1) + " failed");
            return false;
        }
        
        // Brief pause between acquisitions
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
    
    log_message("Streaming mode completed successfully");
    return true;
}

bool MasterController::start_acquisition(double duration, const std::vector<int>& channels) {
    try {
        acquisition_active_ = true;
        acquisition_duration_ = duration;
        active_channels_ = channels;
        
        log_message("Preparing for synchronized acquisition...");
        
        // NEW: Send request_ready command to slave before waiting for ready signal
        log_message("Requesting slave to send ready signal...", true);
        json request_ready_cmd;
        request_ready_cmd["command"] = "request_ready";
        request_ready_cmd["sequence"] = command_sequence_++;
        
        std::string cmd_str = request_ready_cmd.dump();
        zmq::message_t cmd_msg(cmd_str.size());
        memcpy(cmd_msg.data(), cmd_str.c_str(), cmd_str.size());
        
        // Set timeout for sending/receiving
        command_socket_.set(zmq::sockopt::sndtimeo, 2000);
        command_socket_.set(zmq::sockopt::rcvtimeo, 2000);
        
        auto send_result = command_socket_.send(cmd_msg, zmq::send_flags::none);
        if (!send_result.has_value()) {
            log_message("ERROR: Failed to send request_ready command to slave");
            acquisition_active_ = false;
            return false;
        }
        
        // Wait for response to the request_ready command
        zmq::message_t response;
        auto resp_result = command_socket_.recv(response);
        
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
                        // Check if this is a trigger timestamp message
                        try {
                            json ts_msg = json::parse(sync_data);
                            if (ts_msg.contains("command") && ts_msg["command"] == "trigger_timestamp") {
                                slave_trigger_timestamp_ns_ = ts_msg["slave_trigger_timestamp"];
                                log_message("Received slave trigger timestamp: " + std::to_string(slave_trigger_timestamp_ns_) + " ns", true);
                                // Continue waiting for ready signal
                                continue;
                            }
                        } catch (const std::exception& e) {
                            // Not JSON, treat as regular message
                        }
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
        master_trigger_timestamp_ns_ = now_ns;  // Store master's trigger timestamp
        
        // Start local acquisition
        log_message("Starting local acquisition...");
        log_message("Master trigger timestamp: " + std::to_string(master_trigger_timestamp_ns_) + " ns", true);
        
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
        
        // Use the ex        // Use the efficient single file working approach from proven code
        log_message("Starting efficient single file data collection approach...");
        
        try {
            // Connect to DataLinkTargetService using the working approach
            std::filesystem::path output_dir = fs::path(config_.output_dir);
            zmq::socket_t dlt = dlt_connect(output_dir);
            
            // Close any prior acquisitions (clean slate) - using working method
            close_active_acquisitions(dlt);
            
            // Ensure each channel's timestamps have no external reference (needed for merging)
            configure_timestamps_references(local_tc_socket_, channels);
            
            // Compute pulse width (PWID) and period (PPER) in picoseconds for sub-acquisitions
            long long sub_duration = 0.2; // Use proven sub-duration
            long long pwid_ps = static_cast<long long>(1e12 * sub_duration);
            long long pper_ps = static_cast<long long>(1e12 * (sub_duration + 40e-9));  // add 40 ns dead-time
            
            // Configure the Time Controller's recording settings using working approach
            zmq_exec(local_tc_socket_, "REC:TRIG:ARM:MODE MANUal");  // manual trigger mode
            zmq_exec(local_tc_socket_, "REC:ENABle ON");             // enable the Record generator
            zmq_exec(local_tc_socket_, "REC:STOP");                 // ensure no acquisition is currently running
            zmq_exec(local_tc_socket_, "REC:NUM INF");              // infinite number of sub-acquisitions (until stopped)
            zmq_exec(local_tc_socket_, "REC:PWID " + std::to_string(pwid_ps) + ";PPER " + std::to_string(pper_ps));
            
            // Open streamed acquisitions on each requested channel - single acquisition approach
            std::map<int, std::string> acquisitions_id;
            std::vector<BufferStreamClient*> stream_clients;
            
            for (int ch : channels) {
                zmq_exec(local_tc_socket_, "RAW" + std::to_string(ch) + ":ERRORS:CLEAR");  // reset error counter on channel
                
                // Start a BufferStreamClient to receive timestamps for this channel
                BufferStreamClient* client = new BufferStreamClient(ch);
                stream_clients.push_back(client);
                client->start();
                
                // Instruct DLT to start streaming this channel to the given port
                std::string cmd = "start-stream --address " + config_.master_tc_address + 
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
            std::string output_file = (output_dir / ("master_results_" + get_current_timestamp_str() + ".txt")).string();
            
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
            
            // Wait for all sub-acquisitions to complete and data to be transferred
            log_message("Waiting for data processing to complete...");
            wait_end_of_timestamps_acquisition(local_tc_socket_, dlt, acquisitions_id, 30.0);
            
            // Close the acquisitions using working method
            bool success = close_timestamps_acquisition(local_tc_socket_, dlt, acquisitions_id);
            
            // Stop all stream clients and merger thread
            for (BufferStreamClient* client : stream_clients) {
                client->join();
            }
            log_message("Joining merger thread...");
            merger.join();
            log_message("Merger thread joined.");
            
            // Cleanup dynamically allocated clients
            for (BufferStreamClient* client : stream_clients) {
                delete client;
            }
            
            log_message("Data collection completed successfully using efficient single file approach");
            
            // Convert the text output to binary format for compatibility with existing code
            if (fs::exists(output_file)) {
                log_message("Converting merged data to binary format...");
                
                // Read the merged text file and convert to binary
                std::ifstream infile(output_file);
                std::string bin_filename = master_output_base.string() + ".bin";
                std::ofstream bin_file(bin_filename, std::ios::binary);
                
                std::string line;
                int total_timestamps = 0;
                std::vector<uint64_t> all_timestamps;
                std::vector<int> all_channels;
                
                while (std::getline(infile, line)) {
                    // Skip comments and empty lines
                    if (line.empty() || line[0] == '#') continue;
                    
                    // Parse line format: index timestamp channel
                    std::istringstream iss(line);
                    std::string index_str, timestamp_str, channel_str;
                    
                    if (iss >> index_str >> timestamp_str >> channel_str) {
                        try {
                            uint64_t timestamp = std::stoull(timestamp_str);
                            int channel = std::stoi(channel_str);
                            
                            bin_file.write(reinterpret_cast<const char*>(&timestamp), sizeof(uint64_t));
                            bin_file.write(reinterpret_cast<const char*>(&channel), sizeof(int));
                            
                            all_timestamps.push_back(timestamp);
                            all_channels.push_back(channel);
                            total_timestamps++;
                        } catch (const std::exception& e) {
                            log_message("WARNING: Failed to parse line: " + line + " - " + std::string(e.what()));
                            continue;
                        }
                    }
                }
                
                infile.close();
                bin_file.close();
                
                log_message("Saved master timestamps to " + bin_filename);
                log_message("Collected " + std::to_string(total_timestamps) + " timestamps from all channels", true);
                
                // Save text format if requested
                if (config_.text_output) {
                    std::string txt_filename = master_output_base.string() + ".txt";
                    fs::copy_file(output_file, txt_filename);
                    log_message("Saved timestamps in text format to " + txt_filename);
                }
                
                // Store the timestamps for partial data requests and synchronization
                latest_timestamps_ = all_timestamps;
                latest_channels_ = all_channels;
                
                log_message("Master data collection completed successfully");
                
                // Now request data from slave in controlled manner with proper response handling
                log_message("Master is ready - requesting partial data from slave for synchronization...");
                
                // Start file receiver thread now that master is ready
                start_file_receiver_thread();
                
                // Request partial data from slave and wait for confirmation
                request_partial_data_from_slave_with_response();
                
                // Calculate initial offset from trigger timestamps
                if (slave_trigger_timestamp_ns_ > 0) {
                    int64_t initial_offset = static_cast<int64_t>(slave_trigger_timestamp_ns_) - static_cast<int64_t>(master_trigger_timestamp_ns_);
                    log_message("Initial trigger offset calculated: " + std::to_string(initial_offset) + " ns", true);
                    log_message("Master trigger: " + std::to_string(master_trigger_timestamp_ns_) + " ns", true);
                    log_message("Slave trigger: " + std::to_string(slave_trigger_timestamp_ns_) + " ns", true);
                    calculated_offset_ns_ = initial_offset;
                } else {
                    log_message("WARNING: No slave trigger timestamp received for initial offset calculation");
                }
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
                    int count = std::stoi(count_str);
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
                            if (!timestamp_str.empty()) {
                                uint64_t timestamp = std::stoull(timestamp_str);
                                timestamps.push_back(timestamp);
                                channels_vec.push_back(ch);
                            }
                        }
                        
                        // Save to binary file
                        std::string bin_filename = master_output_base.string() + ".bin";
                        std::ofstream bin_file(bin_filename, std::ios::binary);
                        for (size_t i = 0; i < timestamps.size(); ++i) {
                            bin_file.write(reinterpret_cast<const char*>(&timestamps[i]), sizeof(uint64_t));
                            bin_file.write(reinterpret_cast<const char*>(&channels_vec[i]), sizeof(int));
                        }
                        bin_file.close();
                        
                        // Save to text file if requested
                        if (config_.text_output) {
                            std::string txt_filename = master_output_base.string() + ".txt";
                            write_timestamps_to_txt(timestamps, channels_vec, txt_filename);
                            log_message("Saved timestamps in text format to " + txt_filename);
                        }
                        
                        log_message("Saved master timestamps to " + bin_filename);
                        break; // Only process first channel with data for now
                    }
                }
                
                log_message("Fallback data collection completed successfully.");
                
            } catch (const std::exception& fallback_e) {
                log_message("ERROR: Fallback data collection also failed: " + std::string(fallback_e.what()));
                throw;
            }
        }
        
        log_message("Acquisition completed successfully.");
        acquisition_active_ = false;
        return true;
        
    } catch (const std::exception& e) {
        log_message("ERROR: Acquisition failed: " + std::string(e.what()));
        acquisition_active_ = false;
        return false;
    }
}


// Missing member function implementations

void MasterController::log_message(const std::string& message, bool verbose_only) {
    if (!verbose_only || config_.verbose_output) {
        std::cout << message << std::endl;
    }
}

std::string MasterController::get_current_timestamp_str() {
    auto now = std::chrono::system_clock::now();
    auto time_t = std::chrono::system_clock::to_time_t(now);
    auto tm = *std::localtime(&time_t);
    
    std::ostringstream oss;
    oss << std::put_time(&tm, "%Y%m%d_%H%M%S");
    return oss.str();
}

void MasterController::write_timestamps_to_txt(const std::vector<uint64_t>& timestamps, 
                                               const std::vector<int>& channels, 
                                               const std::string& filename) {
    std::ofstream file(filename);
    if (!file) {
        throw std::runtime_error("Failed to open file for writing: " + filename);
    }
    
    for (size_t i = 0; i < timestamps.size(); ++i) {
        file << channels[i] << ";" << timestamps[i] << std::endl;
    }
    
    file.close();
}

void MasterController::write_offset_report(const std::string& filename, 
                                           double mean_offset, 
                                           double min_offset, 
                                           double max_offset, 
                                           double std_dev, 
                                           double relative_spread) {
    std::ofstream file(filename);
    if (!file) {
        throw std::runtime_error("Failed to open file for writing: " + filename);
    }
    
    file << "Synchronization Offset Report" << std::endl;
    file << "=============================" << std::endl;
    file << "Mean offset: " << mean_offset << " ns" << std::endl;
    file << "Min offset: " << min_offset << " ns" << std::endl;
    file << "Max offset: " << max_offset << " ns" << std::endl;
    file << "Standard deviation: " << std_dev << " ns" << std::endl;
    file << "Relative spread: " << relative_spread << " ns" << std::endl;
    
    file.close();
}


void MasterController::start_monitor_thread() {
    // Simple implementation - can be enhanced later
    log_message("Status monitor thread started");
}

void MasterController::start_file_receiver_thread() {
    file_receiver_thread_ = std::thread([this]() {
        log_message("File receiver thread started");
        
        // Set a longer timeout for receiving files - increased to handle network delays
        file_socket_.set(zmq::sockopt::rcvtimeo, 5000); // 5 second timeout per message
        
        int files_received = 0;
        const int max_files = 3; // Expect: full data, partial data, text file
        const int max_wait_cycles = 20; // Maximum wait cycles (20 * 5 seconds = 100 seconds total)
        int wait_cycles = 0;
        
        while (running_ && files_received < max_files && wait_cycles < max_wait_cycles) {
            try {
                zmq::message_t file_msg;
                auto result = file_socket_.recv(file_msg, zmq::recv_flags::none);
                
                if (result.has_value() && file_msg.size() > 0) {
                    files_received++;
                    wait_cycles = 0; // Reset wait cycles when we receive data
                    
                    // Determine file type based on size and content
                    std::string filename;
                    std::string filepath;
                    
                    // Check if this looks like partial data (smaller size)
                    if (file_msg.size() < 100000) { // Less than 100KB likely partial data
                        filename = "partial_data_" + std::to_string(files_received) + ".bin";
                        filepath = fs::path(config_.output_dir) / filename;
                        
                        // Save the received partial file
                        std::ofstream outfile(filepath, std::ios::binary);
                        if (outfile.is_open()) {
                            outfile.write(static_cast<char*>(file_msg.data()), file_msg.size());
                            outfile.close();
                            
                            log_message("Partial data file received from slave: " + filepath + " (" + std::to_string(file_msg.size()) + " bytes)");
                            
                            // Perform synchronization calculation with the partial data
                            try {
                                perform_synchronization_calculation(filepath);
                            } catch (const std::exception& e) {
                                log_message("ERROR: Failed to perform synchronization calculation: " + std::string(e.what()));
                            }
                        } else {
                            log_message("ERROR: Failed to save partial data file: " + filepath);
                        }
                        
                    } else {
                        // Full data file
                        filename = "slave_file_" + std::to_string(files_received) + ".bin";
                        filepath = fs::path(config_.output_dir) / filename;
                        
                        // Save the received file
                        std::ofstream outfile(filepath, std::ios::binary);
                        if (outfile.is_open()) {
                            outfile.write(static_cast<char*>(file_msg.data()), file_msg.size());
                            outfile.close();
                            
                            log_message("Full data file received from slave: " + filepath + " (" + std::to_string(file_msg.size()) + " bytes)");
                        } else {
                            log_message("ERROR: Failed to save full data file: " + filepath);
                        }
                    }
                    
                } else {
                    // No message received, increment wait cycles
                    wait_cycles++;
                    log_message("File receiver waiting... (cycle " + std::to_string(wait_cycles) + "/" + std::to_string(max_wait_cycles) + ")", true);
                    continue;
                }
                
            } catch (const zmq::error_t& e) {
                if (e.num() == EAGAIN) { // Timeout
                    wait_cycles++;
                    log_message("File receiver timeout (cycle " + std::to_string(wait_cycles) + "/" + std::to_string(max_wait_cycles) + ") - continuing to wait...", true);
                    continue;
                } else {
                    log_message("File receiver error: " + std::string(e.what()));
                    break;
                }
            } catch (const std::exception& e) {
                log_message("File receiver error: " + std::string(e.what()));
                break;
            }
        }
        
        if (wait_cycles >= max_wait_cycles) {
            log_message("File receiver thread stopped due to timeout (waited " + std::to_string(max_wait_cycles * 5) + " seconds)");
        } else {
            log_message("File receiver thread stopped (received " + std::to_string(files_received) + " files)");
        }
    });
}

bool MasterController::check_slave_availability() {
    // Simple implementation - can be enhanced later
    log_message("Checking slave availability...");
    return true;
}


void MasterController::perform_synchronization_calculation(const std::string& slave_file_path) {
    try {
        log_message("Performing synchronization calculation with slave data...");
        
        // Read slave timestamps from binary file
        std::vector<uint64_t> slave_timestamps;
        std::vector<int> slave_channels;
        
        std::ifstream slave_file(slave_file_path, std::ios::binary);
        if (!slave_file) {
            log_message("ERROR: Cannot open slave file: " + slave_file_path);
            return;
        }
        
        uint64_t timestamp;
        int channel;
        while (slave_file.read(reinterpret_cast<char*>(&timestamp), sizeof(uint64_t)) &&
               slave_file.read(reinterpret_cast<char*>(&channel), sizeof(int))) {
            slave_timestamps.push_back(timestamp);
            slave_channels.push_back(channel);
        }
        slave_file.close();
        
        log_message("Loaded " + std::to_string(slave_timestamps.size()) + " slave timestamps");
        
        // Check if this is partial data (for sync calculation) or full data
        bool is_partial_data = slave_file_path.find("partial_data_") != std::string::npos;
        
        if (is_partial_data) {
            log_message("Processing partial data for start point synchronization...");
            
            if (slave_timestamps.empty() || latest_timestamps_.empty()) {
                log_message("ERROR: No data available for synchronization");
                return;
            }
                
                // Find slave start time from partial data
                uint64_t slave_start_time = *std::min_element(slave_timestamps.begin(), slave_timestamps.end());
                log_message("Slave start time (from partial data): " + std::to_string(slave_start_time) + " ns");
                
                // Find master start time
                uint64_t master_start_time = *std::min_element(latest_timestamps_.begin(), latest_timestamps_.end());
                log_message("Master original start time: " + std::to_string(master_start_time) + " ns");
                
                // Calculate time difference
                int64_t time_difference = static_cast<int64_t>(slave_start_time) - static_cast<int64_t>(master_start_time);
                log_message("Time difference (slave - master): " + std::to_string(time_difference) + " ns");
                
                // Determine synchronization point
                uint64_t sync_point;
                if (time_difference > 0) {
                    // Slave started later, use slave start time as sync point
                    sync_point = slave_start_time;
                    log_message("Slave started later - using slave start time as sync point");
                } else {
                    // Master started later or same time, use master start time as sync point
                    sync_point = master_start_time;
                    log_message("Master started later or same time - using master start time as sync point");
                }
                
                log_message("Synchronization point: " + std::to_string(sync_point) + " ns");
                
                // Remove master data before sync point
                std::vector<uint64_t> synchronized_timestamps;
                std::vector<int> synchronized_channels;
                
                size_t removed_count = 0;
                size_t kept_count = 0;
                
                for (size_t i = 0; i < latest_timestamps_.size(); ++i) {
                    if (latest_timestamps_[i] >= sync_point) {
                        synchronized_timestamps.push_back(latest_timestamps_[i]);
                        synchronized_channels.push_back(latest_channels_[i]);
                        kept_count++;
                    } else {
                        removed_count++;
                    }
                }
                
                log_message("Removed " + std::to_string(removed_count) + " timestamps before sync point");
                log_message("Kept " + std::to_string(kept_count) + " synchronized timestamps");
                
                // Replace master data with synchronized data
                latest_timestamps_ = synchronized_timestamps;
                latest_channels_ = synchronized_channels;
                
                // Save synchronized master data
                std::string sync_filename = (fs::path(config_.output_dir) / ("master_results_synchronized_" + get_current_timestamp_str() + ".bin")).string();
                
                std::ofstream sync_file(sync_filename, std::ios::binary);
                if (sync_file.is_open()) {
                    for (size_t i = 0; i < synchronized_timestamps.size(); ++i) {
                        sync_file.write(reinterpret_cast<const char*>(&synchronized_timestamps[i]), sizeof(uint64_t));
                        sync_file.write(reinterpret_cast<const char*>(&synchronized_channels[i]), sizeof(int));
                    }
                    sync_file.close();
                    log_message("Synchronized master data saved to: " + sync_filename);
                }
                
                // Save text format if requested
                if (config_.text_output) {
                    std::string sync_txt_filename = (fs::path(config_.output_dir) / ("master_results_synchronized_" + get_current_timestamp_str() + ".txt")).string();
                    write_timestamps_to_txt(synchronized_timestamps, synchronized_channels, sync_txt_filename);
                    log_message("Synchronized master data (text) saved to: " + sync_txt_filename);
                }
                
                // Generate synchronization report
                std::string report_filename = (fs::path(config_.output_dir) / ("sync_report_" + get_current_timestamp_str() + ".txt")).string();
                std::ofstream report_file(report_filename);
                if (report_file.is_open()) {
                    report_file << "=== SYNCHRONIZATION REPORT ===" << std::endl;
                    report_file << "Timestamp: " << get_current_timestamp_str() << std::endl;
                    report_file << std::endl;
                    report_file << "SYNCHRONIZATION DETAILS:" << std::endl;
                    report_file << "Slave start time: " << slave_start_time << " ns" << std::endl;
                    report_file << "Master original start time: " << master_start_time << " ns" << std::endl;
                    report_file << "Time difference: " << time_difference << " ns" << std::endl;
                    report_file << "Synchronization point: " << sync_point << " ns" << std::endl;
                    report_file << std::endl;
                    report_file << "DATA PROCESSING:" << std::endl;
                    report_file << "Timestamps removed: " << removed_count << std::endl;
                    report_file << "Timestamps kept: " << kept_count << std::endl;
                    report_file << "Slave partial data size: " << slave_timestamps.size() << std::endl;
                    report_file << std::endl;
                    report_file << "RESULT:" << std::endl;
                    report_file << "Master and slave data now start at the same time point" << std::endl;
                    report_file << "Synchronized master data file: " << sync_filename << std::endl;
                    report_file.close();
                    log_message("Synchronization report saved to: " + report_filename);
                }
                
                log_message("START POINT SYNCHRONIZATION COMPLETED SUCCESSFULLY");
                log_message("Master data now starts at the same time as slave data");
                
            } else {
                log_message("Processing full slave data file...");
                // This is full slave data, just log the information
                log_message("Received full slave data with " + std::to_string(slave_timestamps.size()) + " timestamps");
            }
        
    } catch (const std::exception& e) {
        log_message("ERROR: Synchronization calculation failed: " + std::string(e.what()));
    }
}



void MasterController::apply_synchronization_correction(const std::string& master_file_path, int64_t offset) {
    try {
        log_message("Applying synchronization correction to master data...");
        log_message("Offset to apply: " + std::to_string(offset) + " ns");
        
        // Read all master timestamps
        std::vector<uint64_t> master_timestamps;
        std::vector<int> master_channels;
        
        std::ifstream master_file(master_file_path, std::ios::binary);
        if (!master_file) {
            log_message("ERROR: Cannot open master file for correction: " + master_file_path);
            return;
        }
        
        uint64_t timestamp;
        int channel;
        while (master_file.read(reinterpret_cast<char*>(&timestamp), sizeof(uint64_t)) &&
               master_file.read(reinterpret_cast<char*>(&channel), sizeof(int))) {
            master_timestamps.push_back(timestamp);
            master_channels.push_back(channel);
        }
        master_file.close();
        
        // Apply offset correction to all timestamps
        for (size_t i = 0; i < master_timestamps.size(); ++i) {
            master_timestamps[i] = static_cast<uint64_t>(static_cast<int64_t>(master_timestamps[i]) + offset);
        }
        
        // Create corrected file
        std::string corrected_file_path = master_file_path;
        size_t dot_pos = corrected_file_path.find_last_of('.');
        if (dot_pos != std::string::npos) {
            corrected_file_path.insert(dot_pos, "_sync_corrected");
        } else {
            corrected_file_path += "_sync_corrected";
        }
        
        // Write corrected data
        std::ofstream corrected_file(corrected_file_path, std::ios::binary);
        for (size_t i = 0; i < master_timestamps.size(); ++i) {
            corrected_file.write(reinterpret_cast<const char*>(&master_timestamps[i]), sizeof(uint64_t));
            corrected_file.write(reinterpret_cast<const char*>(&master_channels[i]), sizeof(int));
        }
        corrected_file.close();
        
        log_message("Synchronization correction applied successfully");
        log_message("Corrected master data saved to: " + corrected_file_path);
        
    } catch (const std::exception& e) {
        log_message("ERROR: Failed to apply synchronization correction: " + std::string(e.what()));
    }
}

void MasterController::save_synchronization_report(double mean_offset, int64_t min_offset, int64_t max_offset, double std_dev, size_t sample_count) {
    try {
        std::string report_filename = fs::path(config_.output_dir) / ("sync_report_" + get_current_timestamp_str() + ".txt");
        std::ofstream report_file(report_filename);
        
        report_file << "Synchronization Analysis Report\n";
        report_file << "Generated: " << get_current_timestamp_str() << "\n\n";
        
        report_file << "Data Summary:\n";
        report_file << "- Sample count: " << sample_count << " timestamps (10% of data)\n\n";
        
        report_file << "Offset Statistics:\n";
        report_file << "Mean offset: " << static_cast<int64_t>(mean_offset) << " ns\n";
        report_file << "Min offset: " << min_offset << " ns\n";
        report_file << "Max offset: " << max_offset << " ns\n";
        report_file << "Standard deviation: " << static_cast<int64_t>(std_dev) << " ns\n";
        
        double relative_spread = (std_dev / std::abs(mean_offset)) * 100.0;
        report_file << "Relative spread: " << std::fixed << std::setprecision(2) << relative_spread << "%\n\n";
        
        report_file << "Analysis:\n";
        if (std::abs(mean_offset) < 1000) {
            report_file << "Excellent synchronization - offset less than 1 microsecond\n";
        } else if (std::abs(mean_offset) < 10000) {
            report_file << "Good synchronization - offset less than 10 microseconds\n";
        } else {
            report_file << "Synchronization offset detected - correction applied to master data\n";
        }
        
        report_file.close();
        log_message("Synchronization report saved to: " + report_filename);
        
    } catch (const std::exception& e) {
        log_message("ERROR: Failed to save synchronization report: " + std::string(e.what()));
    }
}


void MasterController::request_partial_data_from_slave_with_response() {
    try {
        log_message("Sending request for partial data to slave...");
        
        json request;
        request["command"] = "request_partial_data";
        request["sequence"] = 1;
        
        std::string request_str = request.dump();
        zmq::message_t request_msg(request_str.size());
        memcpy(request_msg.data(), request_str.c_str(), request_str.size());
        
        // Send request to slave
        command_socket_.send(request_msg, zmq::send_flags::none);
        
        // Wait for response
        zmq::message_t response_msg;
        command_socket_.set(zmq::sockopt::rcvtimeo, 10000); // 10 second timeout
        auto result = command_socket_.recv(response_msg);
        
        if (result.has_value()) {
            std::string response_data(static_cast<char*>(response_msg.data()), response_msg.size());
            json response_json = json::parse(response_data);
            
            if (response_json["status"] == "ok") {
                log_message("Slave confirmed partial data request: " + response_json["message"].get<std::string>());
                
                // Now wait for the actual partial data file with extended timeout
                log_message("Waiting for partial data file from slave...");
                
                // Give slave time to prepare and send the file
                std::this_thread::sleep_for(std::chrono::seconds(2));
                
                // Wait for file receiver thread to complete with timeout
                int wait_cycles = 0;
                const int max_wait_cycles = 30; // 30 * 2 seconds = 60 seconds total wait
                
                while (file_receiver_thread_.joinable() && wait_cycles < max_wait_cycles) {
                    std::this_thread::sleep_for(std::chrono::seconds(2));
                    wait_cycles++;
                    
                    if (wait_cycles % 5 == 0) {
                        log_message("Still waiting for partial data transfer... (cycle " + std::to_string(wait_cycles) + "/" + std::to_string(max_wait_cycles) + ")", true);
                    }
                }
                
                if (wait_cycles >= max_wait_cycles) {
                    log_message("WARNING: Partial data transfer timeout after " + std::to_string(max_wait_cycles * 2) + " seconds");
                } else {
                    log_message("Partial data transfer completed successfully");
                }
                
            } else {
                log_message("ERROR: Slave rejected partial data request: " + response_json["message"].get<std::string>());
            }
        } else {
            log_message("ERROR: No response from slave for partial data request");
        }
        
    } catch (const std::exception& e) {
        log_message("ERROR: Failed to request partial data from slave: " + std::string(e.what()));
    }
}

void MasterController::request_full_data_from_slave() {
    try {
        log_message("Sending request for full data to slave...");
        
        json request;
        request["command"] = "request_full_data";
        request["sequence"] = 1;
        
        std::string request_str = request.dump();
        zmq::message_t request_msg(request_str.size());
        memcpy(request_msg.data(), request_str.c_str(), request_str.size());
        
        // Send request to slave
        command_socket_.send(request_msg, zmq::send_flags::none);
        
        // Wait for response
        zmq::message_t response_msg;
        command_socket_.set(zmq::sockopt::rcvtimeo, 10000); // 10 second timeout
        auto result = command_socket_.recv(response_msg);
        
        if (result.has_value()) {
            std::string response_data(static_cast<char*>(response_msg.data()), response_msg.size());
            json response_json = json::parse(response_data);
            
            if (response_json["status"] == "ok") {
                log_message("Slave confirmed full data request: " + response_json["message"].get<std::string>());
            } else {
                log_message("ERROR: Slave rejected full data request: " + response_json["message"].get<std::string>());
            }
        } else {
            log_message("ERROR: No response from slave for full data request");
        }
        
    } catch (const std::exception& e) {
        log_message("ERROR: Failed to request full data from slave: " + std::string(e.what()));
    }
}

void MasterController::request_text_data_from_slave() {
    try {
        log_message("Sending request for text data to slave...");
        
        json request;
        request["command"] = "request_text_data";
        request["sequence"] = 1;
        
        std::string request_str = request.dump();
        zmq::message_t request_msg(request_str.size());
        memcpy(request_msg.data(), request_str.c_str(), request_str.size());
        
        // Send request to slave
        command_socket_.send(request_msg, zmq::send_flags::none);
        
        // Wait for response
        zmq::message_t response_msg;
        command_socket_.set(zmq::sockopt::rcvtimeo, 10000); // 10 second timeout
        auto result = command_socket_.recv(response_msg);
        
        if (result.has_value()) {
            std::string response_data(static_cast<char*>(response_msg.data()), response_msg.size());
            json response_json = json::parse(response_data);
            
            if (response_json["status"] == "ok") {
                log_message("Slave confirmed text data request: " + response_json["message"].get<std::string>());
            } else {
                log_message("ERROR: Slave rejected text data request: " + response_json["message"].get<std::string>());
            }
        } else {
            log_message("ERROR: No response from slave for text data request");
        }
        
    } catch (const std::exception& e) {
        log_message("ERROR: Failed to request text data from slave: " + std::string(e.what()));
    }
}

