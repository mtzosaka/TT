#include "common.hpp"
#include <cstdlib>  // for system() or _spawnl on Windows
using json = nlohmann::json;

zmq::socket_t connect_zmq(const std::string& address, int port) {
    // Use a static ZMQ context (shared for all connections in this process)
    static zmq::context_t context(1);
    zmq::socket_t socket(context, zmq::socket_type::req);
    
    // Set timeouts to prevent infinite hanging
    int timeout_ms = 5000; // 5 second timeout
    socket.set(zmq::sockopt::rcvtimeo, timeout_ms);
    socket.set(zmq::sockopt::sndtimeo, timeout_ms);
    socket.set(zmq::sockopt::linger, 0); // Don't wait on close
    
    std::string endpoint = "tcp://" + address + ":" + std::to_string(port);
    try {
        socket.connect(endpoint);
        std::cerr << "Connected to " << endpoint << std::endl;
    } catch (const zmq::error_t& e) {
        throw std::runtime_error(std::string("Unable to connect to \"") + address +
                                 "\" on port " + std::to_string(port) + ": " + e.what());
    }
    return socket;
}

std::string zmq_exec(zmq::socket_t& socket, const std::string& cmd) {
    // Send the command string
    zmq::message_t request(cmd.size());
    memcpy(request.data(), cmd.c_str(), cmd.size());
    socket.send(request, zmq::send_flags::none);
    // Receive the reply
    zmq::message_t reply;
    auto result = socket.recv(reply, zmq::recv_flags::none);
    if (!result.has_value()) {
        throw std::runtime_error("No reply for command: " + cmd);
    }

    std::string ans(static_cast<char*>(reply.data()), reply.size());
    if (!ans.empty() && ans.back() == '\n') {
        ans.pop_back();  // remove trailing newline if present
    }
    // (Optionally log the command and response here if verbose)
    return ans;
}

json dlt_exec(zmq::socket_t& dlt_socket, const std::string& cmd) {
    std::string ans = zmq_exec(dlt_socket, cmd);
    json result;
    if (!ans.empty()) {
        // Parse JSON response
        result = json::parse(ans);
    } else {
        result = nullptr;  // no response
    }
    if (result.is_object() && result.contains("error")) {
        // DLT returned an error structure
        std::string errMsg = "unknown error";
        if (result["error"].is_object() && result["error"].contains("description")) {
            errMsg = result["error"]["description"].get<std::string>();
        }
        throw DataLinkTargetError(errMsg);
    }
    return result;
}

zmq::socket_t dlt_connect(const std::filesystem::path& output_dir, const std::filesystem::path& dlt_path) {
    // Ensure output directory exists
    if (!std::filesystem::exists(output_dir)) {
        throw std::runtime_error("Output folder \"" + output_dir.string() + "\" does not exist.");
    }
    
    std::cerr << "Connecting to existing DataLinkTargetService on localhost:" << DLT_PORT << "..." << std::endl;
    
    // Connect directly to the existing running DLT instance
    // Don't try to launch a new one since DLT is already attached to hardware
    try {
        zmq::socket_t dlt_socket = connect_zmq("localhost", DLT_PORT);
        std::cerr << "Successfully connected to running DataLinkTargetService" << std::endl;
        return dlt_socket;
    } catch (const std::exception& e) {
        std::string error_msg = "Failed to connect to running DataLinkTargetService on localhost:" + 
                               std::to_string(DLT_PORT) + ". Error: " + e.what() + 
                               "\nPlease ensure DataLinkTargetService is running and accessible.";
        throw std::runtime_error(error_msg);
    }
}

void close_active_acquisitions(zmq::socket_t& dlt_socket) {
    std::cerr << "Attempting to close active acquisitions..." << std::endl;
    
    try {
        // Get list of active acquisitions with timeout protection
        json acquisitions;
        try {
            acquisitions = dlt_exec(dlt_socket, "list");
        } catch (const std::exception& e) {
            std::cerr << "Error getting active acquisitions list: " << e.what() << std::endl;
            std::cerr << "Ignoring DLT closing errors as requested - continuing with internal cleanup" << std::endl;
            return; // Exit gracefully without throwing
        }
        
        if (acquisitions.is_array() && !acquisitions.empty()) {
            std::cerr << "Found " << acquisitions.size() << " active acquisitions" << std::endl;
            
            for (const auto& acqu : acquisitions) {
                std::string acqu_id = acqu.get<std::string>();
                std::cerr << "Closing active acquisition '" << acqu_id << "'" << std::endl;
                
                try {
                    // Try to stop the acquisition with timeout protection
                    auto start_time = std::chrono::steady_clock::now();
                    dlt_exec(dlt_socket, "stop --id " + acqu_id);
                    auto elapsed = std::chrono::steady_clock::now() - start_time;
                    if (elapsed > std::chrono::seconds(5)) {
                        std::cerr << "Warning: Stop command took " << std::chrono::duration_cast<std::chrono::seconds>(elapsed).count() << " seconds" << std::endl;
                    }
                    std::cerr << "Successfully closed acquisition '" << acqu_id << "'" << std::endl;
                } catch (const DataLinkTargetError& e) {
                    std::cerr << "DLT error closing acquisition " << acqu_id << ": " << e.what() << std::endl;
                    std::cerr << "Ignoring DLT error as requested - continuing with next acquisition" << std::endl;
                } catch (const std::exception& e) {
                    std::cerr << "Unexpected error closing acquisition " << acqu_id << ": " << e.what() << std::endl;
                    std::cerr << "Ignoring error as requested - continuing with next acquisition" << std::endl;
                }
            }
        } else {
            std::cerr << "No active acquisitions found" << std::endl;
        }
    } catch (const std::exception& e) {
        std::cerr << "Error getting active acquisitions list: " << e.what() << std::endl;
        std::cerr << "Ignoring DLT closing errors as requested - DLT process will handle cleanup" << std::endl;
    }
    
    std::cerr << "Finished closing active acquisitions (internal cleanup completed)" << std::endl;
}

void wait_end_of_timestamps_acquisition(zmq::socket_t& tc_socket, zmq::socket_t& dlt_socket,
                                        const std::map<int, std::string>& acquisitions_id, double timeout) {
    const int SLEEP_TIME = 1;
    const double NATURAL_INACTIVITY = 1.0;
    const double MAX_TOTAL_TIMEOUT = 30.0; // Maximum total wait time
    
    // Ensure minimum timeout
    timeout = std::max(timeout, std::max<double>(SLEEP_TIME + 1, NATURAL_INACTIVITY));
    timeout = std::min(timeout, MAX_TOTAL_TIMEOUT); // Cap the timeout
    
    auto start_time = std::chrono::steady_clock::now();
    
    // Determine if a finite number of sub-acquisitions were set
    int number_of_records = -1;
    try {
        std::string rec_num = zmq_exec(tc_socket, "REC:NUMber?");
        number_of_records = std::stoi(rec_num);
    } catch (...) {
        // If REC:NUM was "INF" or not a number, treat as infinite
        number_of_records = -1;
        timeout += NATURAL_INACTIVITY;
    }
    
    // Track completion status for each channel
    std::map<int, bool> done;
    for (auto& [ch, id] : acquisitions_id) {
        done[ch] = false;
    }
    
    int iteration_count = 0;
    const int MAX_ITERATIONS = static_cast<int>(timeout / SLEEP_TIME) + 10; // Safety limit
    
    while (iteration_count < MAX_ITERATIONS) {
        iteration_count++;
        
        // Check total elapsed time
        auto elapsed = std::chrono::steady_clock::now() - start_time;
        if (elapsed > std::chrono::seconds(static_cast<int>(MAX_TOTAL_TIMEOUT))) {
            std::cerr << "Maximum timeout reached, forcing completion" << std::endl;
            break;
        }
        
        if (std::all_of(done.begin(), done.end(), [](auto p){ return p.second; })) {
            std::cerr << "All channels completed successfully" << std::endl;
            break;  // all channels done
        }
        
        std::this_thread::sleep_for(std::chrono::seconds(1));
        
        // Check if Time Controller is still in PLAYING state
        bool playing = false;
        try {
            std::string stage = zmq_exec(tc_socket, "REC:STAGe?");
            if (!stage.empty()) {
                std::string stage_up = stage;
                for (char& c : stage_up) c = toupper(c);
                playing = (stage_up.find("PLAY") != std::string::npos);  // "PLAYING" vs "STOPPED"
            }
        } catch (const std::exception& e) {
            std::cerr << "Error checking TC stage: " << e.what() << std::endl;
            playing = false; // Assume stopped if we can't check
        }
        
        // Get status of active acquisitions from DLT
        int max_acq_count = 0;
        for (auto& [ch, id] : acquisitions_id) {
            if (done[ch]) continue;
            
            try {
                json status = dlt_exec(dlt_socket, "status --id " + id);
                
                // If an error occurred in DLT for this channel, mark done
                if (status.contains("error") && !status["error"].is_null()) {
                    std::cerr << "[channel " << ch << "] DLT error, marking as done" << std::endl;
                    done[ch] = true;
                    continue;
                }
                
                // Update max acquisitions_count seen
                if (status.contains("acquisitions_count")) {
                    int count = status["acquisitions_count"].get<int>();
                    max_acq_count = std::max(max_acq_count, count);
                }
                
                // Determine if this channel acquisition can be considered finished
                if (!playing) {
                    if (number_of_records < 0) {
                        // Infinite sub-acquisitions: wait for natural end of last sub-acquisition
                        if (status.contains("acquisitions_count") && status.contains("inactivity")) {
                            int count = status["acquisitions_count"].get<int>();
                            double inact = status["inactivity"].get<double>();
                            if (count > 0 && count == max_acq_count && inact > NATURAL_INACTIVITY) {
                                std::cerr << "[channel " << ch << "] Natural completion detected" << std::endl;
                                done[ch] = true;
                            }
                        }
                    } else {
                        // Finite number of sub-acquisitions
                        if (status.contains("acquisitions_count") && status["acquisitions_count"].get<int>() >= number_of_records) {
                            std::cerr << "[channel " << ch << "] Reached target record count" << std::endl;
                            done[ch] = true;
                        }
                    }
                    
                    // Timeout check: if no new data for too long after acquisition end
                    if (!done[ch] && status.contains("inactivity") && status["inactivity"].get<double>() > timeout) {
                        std::cerr << "[channel " << ch << "] timestamp transfer timeout" << std::endl;
                        done[ch] = true;
                    }
                }
            } catch (const std::exception& e) {
                std::cerr << "[channel " << ch << "] Error getting status: " << e.what() << ", marking as done" << std::endl;
                done[ch] = true;
            }
        }
        
        // Progress report every 5 iterations
        if (iteration_count % 5 == 0) {
            int completed = std::count_if(done.begin(), done.end(), [](auto p){ return p.second; });
            std::cerr << "Progress: " << completed << "/" << done.size() << " channels completed (iteration " << iteration_count << ")" << std::endl;
        }
    }
    
    if (iteration_count >= MAX_ITERATIONS) {
        std::cerr << "Maximum iterations reached, forcing completion" << std::endl;
    }
    
    std::cerr << "Wait for timestamps acquisition completed" << std::endl;
}

bool close_timestamps_acquisition(zmq::socket_t& tc_socket, zmq::socket_t& dlt_socket,
                                  const std::map<int, std::string>& acquisitions_id) {
    bool success = true;
    dlt_exec(dlt_socket, "list");  // (refresh internal state, not strictly necessary)
    // Stop each acquisition and gather its status
    std::map<int, json> status_map;
    for (auto& [ch, id] : acquisitions_id) {
        json response = dlt_exec(dlt_socket, "stop --id " + id);
        if (response.contains("status")) {
            status_map[ch] = response["status"];
        } else {
            status_map[ch] = json{};  // empty status if none
        }
    }
    // Determine the highest sub-acquisition count among channels
    int expected_count = 1;
    for (auto& [ch, st] : status_map) {
        if (st.contains("acquisitions_count")) {
            expected_count = std::max(expected_count, st["acquisitions_count"].get<int>());
        }
    }
    // Analyze status for each channel
    for (auto& [ch, st] : status_map) {
        std::vector<std::string> errors;
        // Collect any DLT-reported errors for this channel
        if (st.contains("errors") && st["errors"].is_array()) {
            for (auto& err : st["errors"]) {
                if (err.is_object() && err.contains("description")) {
                    errors.push_back(err["description"].get<std::string>());
                }
            }
        }
        int acq_count = st.contains("acquisitions_count") ? st["acquisitions_count"].get<int>() : 0;
        if (acq_count < expected_count) {
            errors.push_back("End of acquisition not properly registered (" +
                             std::to_string(acq_count) + "/" + std::to_string(expected_count) + ")");
        }
        // Turn off the Time Controller sending for this channel
        zmq_exec(tc_socket, "RAW" + std::to_string(ch) + ":SEND OFF");
        // Check if the Time Controller itself logged any acquisition errors on this channel
        std::string errCount = zmq_exec(tc_socket, "RAW" + std::to_string(ch) + ":ERRORS?");
        try {
            if (!errCount.empty() && std::stoi(errCount) != 0) {
                errors.push_back("The Time Controller reports timestamps acquisition errors");
            }
        } catch (...) { /* ignore parsing errors */ }
        // Log any errors and update success flag
        for (const auto& errMsg : errors) {
            std::cerr << "[channel " << ch << "] " << errMsg << std::endl;
        }
        if (!errors.empty()) {
            success = false;
        }
    }
    return success;
}

void configure_timestamps_references(zmq::socket_t& tc_socket, const std::vector<int>& channels) {
    for (int ch : channels) {
        zmq_exec(tc_socket, "RAW" + std::to_string(ch) + ":REF:LINK NONE");
    }
}
