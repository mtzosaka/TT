#include "common.hpp"
#include <cstdlib>  // for system() or _spawnl on Windows
#include <iostream> // for std::cerr
using json = nlohmann::json;

zmq::socket_t connect_zmq(const std::string& address, int port) {
    // Use a static ZMQ context (shared for all connections in this process)
    static zmq::context_t context(1);
    zmq::socket_t socket(context, zmq::socket_type::req);
    std::string endpoint = "tcp://" + address + ":" + std::to_string(port);
    try {
        socket.connect(endpoint);
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
    // Determine the path to DataLinkTargetService.exe
    std::filesystem::path dlt_dir = dlt_path;
    std::filesystem::path dlt_bin;
    if (std::filesystem::is_directory(dlt_path)) {
        dlt_bin = dlt_path / DEFAULT_DLT_FILENAME;
    } else {
        dlt_bin = dlt_path;
        dlt_dir = dlt_path.parent_path();
    }
    if (!std::filesystem::exists(dlt_bin)) {
        throw std::runtime_error("DataLinkTarget binary \"" + dlt_bin.string() + "\" not found.");
    }
    // Check for configuration template file required by DLT (for logging)
    std::filesystem::path config_template = dlt_dir / "DataLinkTargetService.log.conf";
    if (!std::filesystem::exists(config_template)) {
        throw std::runtime_error("Configuration template \"" + config_template.string() + "\" not found.");
    }
    // If DLT not already running, launch it
    bool need_start = true;
    try {
        // Try connecting to DLT port to test if running
        zmq::socket_t testSock = connect_zmq("localhost", DLT_PORT);
        need_start = false;
        testSock.close();
    } catch (...) {
        need_start = true;
    }
    if (need_start) {
        // Prepare a log config file for DLT by copying the template (in current directory)
        std::filesystem::path script_dir = std::filesystem::current_path();
        std::filesystem::path log_conf_path = script_dir / "DataLinkTargetService.log.conf";
        std::ifstream templateFile(config_template);
        std::ofstream logConfFile(log_conf_path);
        std::string line;
        const std::string prefix = "log4cplus.appender.AppenderFile.File=";
        while (std::getline(templateFile, line)) {
            if (line.rfind(prefix, 0) == 0) {
                // Redirect log output to current directory
                logConfFile << prefix << script_dir.string() << "/" << std::endl;
            } else {
                logConfFile << line << std::endl;
            }
        }
        templateFile.close();
        logConfFile.close();
        // Launch DataLinkTargetService (non-blocking)
        std::string command = "\"" + dlt_bin.string() + "\" -f \"" + output_dir.string() +
                              "\" --logconf \"" + log_conf_path.string() + "\"";
#ifdef _WIN32
        // On Windows, use _spawnl or CreateProcess to run in background
        int ret = _spawnl(_P_NOWAIT, dlt_bin.string().c_str(), 
                          dlt_bin.string().c_str(), "-f", output_dir.string().c_str(), 
                          "--logconf", log_conf_path.string().c_str(), NULL);
        if (ret < 0) {
            throw std::runtime_error("Failed to launch DataLinkTargetService.");
        }
#else
        // On Linux/Unix, use std::system (assuming DataLinkTargetService is available)
        if (std::system(command.c_str()) != 0) {
            throw std::runtime_error("Failed to launch DataLinkTargetService.");
        }
#endif
        std::this_thread::sleep_for(std::chrono::milliseconds(100));  // brief wait for service startup
    }
    // Connect to DataLinkTargetService on localhost:6060
    return connect_zmq("localhost", DLT_PORT);
}

void close_active_acquisitions(zmq::socket_t& dlt_socket) {
    // Stop any prior open acquisitions to start fresh (DLT "list" and "stop")
    json active_list = dlt_exec(dlt_socket, "list");
    if (active_list.is_array()) {
        for (const auto& acqu_id_val : active_list) {
            if (!acqu_id_val.is_string()) continue;
            std::string acqu_id = acqu_id_val.get<std::string>();
            std::cerr << "Closing active acquisition '" << acqu_id << "'" << std::endl;
            try {
                dlt_exec(dlt_socket, "stop --id " + acqu_id);
            } catch (const DataLinkTargetError& e) {
                std::cerr << "Error closing acquisition " << acqu_id << ": " << e.what() << std::endl;
            }
        }
    }
}

void wait_end_of_timestamps_acquisition(zmq::socket_t& tc_socket, zmq::socket_t& dlt_socket,
                                        const std::map<int, std::string>& acquisitions_id, double timeout) {
    const int SLEEP_TIME = 1;
    const double NATURAL_INACTIVITY = 1.0;
    // Ensure minimum timeout
    timeout = std::max(timeout, std::max<double>(SLEEP_TIME + 1, NATURAL_INACTIVITY));
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
    while (true) {
        if (std::all_of(done.begin(), done.end(), [](auto p){ return p.second; })) {
            break;  // all channels done
        }
        std::this_thread::sleep_for(std::chrono::seconds(1));
        // Check if Time Controller is still in PLAYING state
        bool playing = false;
        std::string stage = zmq_exec(tc_socket, "REC:STAGe?");
        if (!stage.empty()) {
            std::string stage_up = stage;
            for (char& c : stage_up) c = toupper(c);
            playing = (stage_up.find("PLAY") != std::string::npos);  // "PLAYING" vs "STOPPED"
        }
        // Get status of active acquisitions from DLT
        int max_acq_count = 0;
        for (auto& [ch, id] : acquisitions_id) {
            if (done[ch]) continue;
            json status = dlt_exec(dlt_socket, "status --id " + id);
            // If an error occurred in DLT for this channel, mark done
            if (status.contains("error") && !status["error"].is_null()) {
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
                            done[ch] = true;
                        }
                    }
                } else {
                    // Finite number of sub-acquisitions
                    if (status.contains("acquisitions_count") && status["acquisitions_count"].get<int>() >= number_of_records) {
                        done[ch] = true;
                    }
                }
                // Timeout check: if no new data for too long after acquisition end
                if (!done[ch] && status.contains("inactivity") && status["inactivity"].get<double>() > timeout) {
                    std::cerr << "[channel " << ch << "] timestamp transfer timeout" << std::endl;
                    done[ch] = true;
                }
            }
        }
    }
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
