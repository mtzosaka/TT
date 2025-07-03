#include "common.hpp"
#include "streams.hpp"
#include <map>
#include <vector>
#include <string>
#include <iostream>
#include <cstdlib>
#include <fstream>
#include "json.hpp"
using json = nlohmann::json;



int main(int argc, char* argv[]) {
    std::set_terminate([]() {
    std::cerr << "🔴 std::terminate() called (uncaught exception or thread crash)." << std::endl;
    std::abort(); // ensure termination
    });

    try {
        // your existing full logic here
        // including acquisition, waiting, etc.
    // Default parameters (same defaults as Python script)
    double duration = 0.6;
    double sub_duration = 0.2;  // DEFAULT_SUB_ACQUISITION_DURATION (min 0.2s)
    std::string tc_address = "169.254.218.109";  // DEFAULT_TC_ADDRESS
    std::vector<int> channels = {1, 2, 3, 4};    // DEFAULT_CHANNELS
    std::string output_file = "results3.txt";    // DEFAULT_OUTPUT_FILE
    std::string log_path;
    bool verbose = false;

    // Simple CLI argument parsing (not exhaustive, for illustration)
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if ((arg == "--duration" || arg == "-d") && i+1 < argc) {
            duration = std::atof(argv[++i]);
        } else if (arg == "--sub-duration" && i+1 < argc) {
            sub_duration = std::atof(argv[++i]);
        } else if (arg == "--address" && i+1 < argc) {
            tc_address = argv[++i];
        } else if (arg == "--channels" && i+1 < argc) {
            channels.clear();
            // Parse channel list (either as a single comma-separated string or space-separated numbers)
            std::string list = argv[++i];
            size_t start = 0;
            while (start < list.size()) {
                // Skip delimiters
                if (list[start] == ',' || list[start] == ' ') {
                    ++start;
                    continue;
                }
                size_t end = list.find_first_of(", ", start);
                if (end == std::string::npos) end = list.size();
                int ch = std::atoi(list.substr(start, end - start).c_str());
                if (ch >= 1 && ch <= 4) {
                    channels.push_back(ch);
                }
                start = end;
            }
        } else if (arg == "--output-file" && i+1 < argc) {
            output_file = argv[++i];
        } else if (arg == "--log-path" && i+1 < argc) {
            log_path = argv[++i];
        } else if (arg == "-v" || arg == "--verbose") {
            verbose = true;
        }
    }

    // Set up logging to file if requested, and verbosity
    std::ofstream logFile;
    if (!log_path.empty()) {
        logFile.open(log_path);
        if (logFile.is_open()) {
            std::cerr.rdbuf(logFile.rdbuf());  // redirect cerr to log file
        }
    }
    if (verbose) {
        std::cerr << "Verbose mode enabled. Detailed logs will be printed.\n";
    }

    try {
        // Connect to Time Controller (SCPI interface over ZMQ)
        zmq::socket_t tc = connect_zmq(tc_address, SCPI_PORT);
        // Launch/connect to DataLinkTargetService (DLT)
        zmq::socket_t dlt = dlt_connect(std::filesystem::path(output_file).parent_path());
        // Close any prior acquisitions (clean slate)
        close_active_acquisitions(dlt);

        // (Optional: if a demo mode were needed, we would configure dummy timestamp generation here)

        // Ensure each channel's timestamps have no external reference (needed for merging)
        configure_timestamps_references(tc, channels);

        // Compute pulse width (PWID) and period (PPER) in picoseconds for sub-acquisitions
        long long pwid_ps = static_cast<long long>(1e12 * sub_duration);
        long long pper_ps = static_cast<long long>(1e12 * (sub_duration + 40e-9));  // add 40 ns dead-time
        // Configure the Time Controller's recording settings for synchronized sub-acquisitions
        zmq_exec(tc, "REC:TRIG:ARM:MODE MANUal");  // manual trigger mode
        zmq_exec(tc, "REC:ENABle ON");             // enable the Record generator
        zmq_exec(tc, "REC:STOP");                 // ensure no acquisition is currently running
        zmq_exec(tc, "REC:NUM INF");              // infinite number of sub-acquisitions (until stopped)
        // Set sub-acquisition duration (PWID) and period (PPER = duration + dead time)
        zmq_exec(tc, "REC:PWID " + std::to_string(pwid_ps) + ";PPER " + std::to_string(pper_ps));

        // Open streamed acquisitions on each requested channel
        std::map<int, std::string> acquisitions_id;
        std::vector<BufferStreamClient*> stream_clients;
        for (int ch : channels) {
            zmq_exec(tc, "RAW" + std::to_string(ch) + ":ERRORS:CLEAR");  // reset error counter on channel
            // Start a BufferStreamClient to receive timestamps for this channel
            BufferStreamClient* client = new BufferStreamClient(ch);
            stream_clients.push_back(client);
            client->start();
            // Instruct DLT to start streaming this channel to the given port
            std::string cmd = "start-stream --address " + tc_address + 
                              " --channel " + std::to_string(ch) + 
                              " --stream-port " + std::to_string(client->port);
            json response = dlt_exec(dlt, cmd);
            if (response.contains("id")) {
                acquisitions_id[ch] = response["id"].get<std::string>();
            }
            // Tell the Time Controller to send timestamps from this channel
            zmq_exec(tc, "RAW" + std::to_string(ch) + ":SEND ON");
        }

        // Start the merging thread to combine incoming timestamps on the fly
        TimestampsMergerThread merger(stream_clients, output_file, static_cast<uint64_t>(pper_ps));
        merger.start();

        // Start the synchronized acquisition on the Time Controller
        zmq_exec(tc, "REC:PLAY");
        if (verbose) {
            std::cerr << "Acquisition started. Collecting data for " << duration << " seconds...\n";
        }
        // Let the acquisition run for the specified duration
        std::this_thread::sleep_for(std::chrono::milliseconds(static_cast<int>(duration * 1000)));

        // Stop the acquisition
        zmq_exec(tc, "REC:STOP");
        if (verbose) {
            std::cerr << "Acquisition stopped. Waiting for final data...\n";
            std::cout << "Finalizing post-processing..." << std::endl;
        try {
            // ← put all your final logic here
            // Example:
            // merger.join();
            // streamClient.stop();
            // output file merge etc.

            std::cout << "Shutdown complete." << std::endl;
        } catch (const std::exception& ex) {
            std::cerr << "Exception during shutdown: " << ex.what() << std::endl;
        } catch (...) {
            std::cerr << "Unknown exception during shutdown." << std::endl;
        }


        }
        // Wait for all sub-acquisitions to complete and data to be transferred
        wait_end_of_timestamps_acquisition(tc, dlt, acquisitions_id);

        // Close the acquisitions on DLT and check for errors
        bool success = close_timestamps_acquisition(tc, dlt, acquisitions_id);

        // Stop all stream clients and merger thread
        for (BufferStreamClient* client : stream_clients) {
            client->join();
        }
        std::cout << "Joining merger thread..." << std::endl;
        merger.join();
        std::cout << "Merger thread joined." << std::endl;

        // Cleanup dynamically allocated clients
        for (BufferStreamClient* client : stream_clients) {
            delete client;
        }

        if (verbose) {
            std::cerr << "Merged timestamps written to " << output_file 
                      << ". Total channels: " << channels.size() 
                      << (success ? " (no errors).\n" : " (with errors).\n");
        }
        return success ? 0 : 1;
    } catch (const DataLinkTargetError& e) {
        std::cerr << "DataLinkTargetError: " << e.what() << std::endl;
        return 1;
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }

    std::cout << "Shutdown complete." << std::endl;
    } catch (const std::exception& ex) {
        std::cerr << "Unhandled exception: " << ex.what() << std::endl;
    } catch (...) {
        std::cerr << "Unknown exception occurred." << std::endl;
    }
}
