#ifndef COMMON_HPP
#define COMMON_HPP

#include <string>
#include <stdexcept>
#include <zmq.hpp>
#include "json.hpp"
#include <filesystem>
#include <fstream>
#include <thread>
#include <chrono>
#include <iostream>

// Default ports for Time Controller (SCPI) and DataLinkTarget (DLT) services
constexpr int DLT_PORT = 6060;
constexpr int SCPI_PORT = 5555;

// Default DataLinkTargetService installation path and executable name
const std::filesystem::path DEFAULT_DLT_PATH = "/etc/elvis";
const std::string DEFAULT_DLT_FILENAME = "DataLinkTargetService";

// Exception type for DataLinkTarget errors (e.g., if DLT returns an error JSON)
class DataLinkTargetError : public std::runtime_error {
public:
    explicit DataLinkTargetError(const std::string& msg) : std::runtime_error(msg) {}
};

// Establish a ZMQ REQ connection to the given address and port (for TC or DLT)
zmq::socket_t connect_zmq(const std::string& address, int port);

// Send a SCPI command string over ZMQ and return the response string
std::string zmq_exec(zmq::socket_t& socket, const std::string& cmd);

// Send a DLT command string and parse the JSON response (throws DataLinkTargetError on error)
nlohmann::json dlt_exec(zmq::socket_t& dlt_socket, const std::string& cmd);

// Start or connect to DataLinkTargetService, using `dlt_path` (directory or full path of exe) 
// and `output_dir` for DLT's target folder (-f argument). Returns a ZMQ REQ socket to DLT.
zmq::socket_t dlt_connect(const std::filesystem::path& output_dir,
                          const std::filesystem::path& dlt_path = DEFAULT_DLT_PATH);

// Gracefully stop any active acquisitions on DLT (similar to Python close_active_acquisitions)
void close_active_acquisitions(zmq::socket_t& dlt_socket);

// Wait for the end of all timestamp sub-acquisitions (or error/timeout) before closing (similar to wait_end_of_timestamps_acquisition)
void wait_end_of_timestamps_acquisition(zmq::socket_t& tc_socket, 
                                        zmq::socket_t& dlt_socket, 
                                        const std::map<int, std::string>& acquisitions_id, 
                                        double timeout = 10.0);

// Close the timestamp acquisitions on DLT and TC (stop streaming, check errors) and report success
bool close_timestamps_acquisition(zmq::socket_t& tc_socket, 
                                  zmq::socket_t& dlt_socket, 
                                  const std::map<int, std::string>& acquisitions_id);

// Configure each channel to have no reference signal (RAW#:REF:LINK NONE), required for merging
void configure_timestamps_references(zmq::socket_t& tc_socket, const std::vector<int>& channels);

#endif // COMMON_HPP
