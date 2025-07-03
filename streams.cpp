#include "streams.hpp"
#include <algorithm>
#include <iostream>
#include <chrono>
#include <zmq.h>  // for zmq_socket_monitor

// Create a static ZMQ context for all stream sockets (separate from REQ context for safety)
static zmq::context_t streamsContext(1);

BufferStreamClient::BufferStreamClient(int channel)
    : number(channel), port(4241 + channel),
      data_socket(streamsContext, zmq::socket_type::pair),
      monitor_socket(streamsContext, zmq::socket_type::pair),
      running(false)
{
    // Connect to the DataLinkTargetService stream port for this channel (localhost)
    std::string addr = "tcp://127.0.0.1:" + std::to_string(port);
    data_socket.connect(addr);
    // Set up a monitor socket to detect when the data_socket is disconnected
    std::string monEndpoint = "inproc://monitor-" + std::to_string(number);
    zmq_socket_monitor(data_socket.handle(), monEndpoint.c_str(), ZMQ_EVENT_DISCONNECTED);
    monitor_socket.connect(monEndpoint);
}

BufferStreamClient::~BufferStreamClient() {
    // Ensure thread is stopped
    if (running) {
        join();
    }
    data_socket.close();
    monitor_socket.close();
}

void BufferStreamClient::start() {
    running = true;
    recv_thread = std::thread(&BufferStreamClient::run, this);
}

void BufferStreamClient::join() {
    // Signal the receiving loop to stop
    running = false;
    // The thread will exit on its own (poll loop breaks on running==false or socket disconnect)
    if (recv_thread.joinable()) {
        recv_thread.join();
    }
}

void BufferStreamClient::run() {
    // Poll on both data and monitor sockets
    zmq_pollitem_t items[2];
    items[0].socket = data_socket.handle();
    items[0].events = ZMQ_POLLIN;
    items[1].socket = monitor_socket.handle();
    items[1].events = ZMQ_POLLIN;
    while (running) {
        int rc = zmq_poll(items, 2, 1000);  // wait up to 1000 ms for an event
        if (rc < 0) {
            break;  // interrupted or error
        }
        if (items[0].revents & ZMQ_POLLIN) {
            // Received a data message (timestamps)
            zmq_msg_t msg;
            zmq_msg_init(&msg);
            int received = zmq_msg_recv(&msg, data_socket.handle(), 0);
            if (received >= 0) {
                size_t msg_size = zmq_msg_size(&msg);
                uint8_t* data_ptr = static_cast<uint8_t*>(zmq_msg_data(&msg));
                if (msg_size == 0) {
                    // Zero-length message indicates end-of-stream
                    running = false;
                } else {
                    // Buffer the received bytes (each timestamp is 8 bytes, unsigned 64-bit)
                    std::vector<uint8_t> message(data_ptr, data_ptr + msg_size);
                    buffer.push_back(std::move(message));
                    size_t received_timestamps = msg_size / 8;
                    // Log buffering info (channel, count, total buffered bytes)
                    size_t total_buffered = 0;
                    for (auto& chunk : buffer) {
                        total_buffered += chunk.size();
                    }
                    std::cerr << "[channel " << number << "] buffering " << received_timestamps
                              << " timestamps (message #" << buffer.size() 
                              << ", buffered: " << total_buffered << " bytes)" << std::endl;
                }
            } else {
                // recv error (socket likely closed), stop
                running = false;
            }
            zmq_msg_close(&msg);
        }
        if (items[1].revents & ZMQ_POLLIN) {
            // Monitor event received (e.g., socket disconnected)
            zmq_msg_t event_msg;
            zmq_msg_init(&event_msg);
            zmq_msg_recv(&event_msg, monitor_socket.handle(), 0);
            // The first frame of monitor message is zmq_event_t
            zmq_event_t* ev = static_cast<zmq_event_t*>(zmq_msg_data(&event_msg));
            if (ev && ev->event == ZMQ_EVENT_DISCONNECTED) {
                running = false;
            }
            zmq_msg_close(&event_msg);
            // (There may be an address frame as well which we're not explicitly reading here)
        }
    }
    // Exiting thread loop
}

TimestampsMergerThread::TimestampsMergerThread(const std::vector<BufferStreamClient*>& streams_, 
                                               const std::string& output_path, 
                                               uint64_t sub_acquisition_pper_)
    : streams(streams_), expect_more(true),
      outfile(output_path), sub_acquisition_pper(sub_acquisition_pper_),
      next_merge_index(0), total_merged(0)
{
    if (!outfile.is_open()) {
        throw std::runtime_error("Cannot open output file: " + output_path);
    }
}

TimestampsMergerThread::~TimestampsMergerThread() {
    if (outfile.is_open()) {
        outfile.close();
    }
}

void TimestampsMergerThread::start() {
    merge_thread = std::thread(&TimestampsMergerThread::run, this);
}

void TimestampsMergerThread::join() {
    // Signal that no more new timestamps will arrive
    expect_more = false;
    if (merge_thread.joinable()) {
        merge_thread.join();
    }
}

bool TimestampsMergerThread::all_channels_buffer_ready() {
    // Check if each stream has at least next_merge_index+1 messages buffered and that message is not empty
    for (BufferStreamClient* stream : streams) {
        if (stream->buffer.size() <= next_merge_index) {
            return false;
        }
        if (stream->buffer[next_merge_index].empty()) {
            return false;
        }
    }
    return true;
}

void TimestampsMergerThread::merge_next_timestamp_block() {
    // Collect all timestamps from the next message batch (one per stream at next_merge_index)
    std::vector<std::pair<int, std::vector<uint64_t>>> batchData;
    batchData.reserve(streams.size());
    for (BufferStreamClient* stream : streams) {
        // Convert raw bytes to 64-bit integers
        const std::vector<uint8_t>& msg_bytes = stream->buffer[next_merge_index];
        size_t count = msg_bytes.size() / sizeof(uint64_t);
        std::vector<uint64_t> timestamps(count);
        memcpy(timestamps.data(), msg_bytes.data(), msg_bytes.size());
        // Adjust timestamps by adding sub_acquisition_pper * index (to account for each sub-acquisition’s offset)
        for (uint64_t& ts : timestamps) {
            ts += sub_acquisition_pper * next_merge_index;
        }
        batchData.emplace_back(stream->number, std::move(timestamps));
        // Mark this buffer slot as consumed by clearing it (free memory)
        // (We retain the slot to keep indices aligned; clearing the vector releases its data)
        const_cast<std::vector<uint8_t>&>(msg_bytes).clear();
    }
    // Merge all timestamps from this batch across channels, sorted by timestamp
    std::vector<std::pair<int, uint64_t>> merged;
    merged.reserve(batchData.size() * 1000); // reserve some space (guess) to minimize reallocations
    for (auto& [ch, ts_vec] : batchData) {
        for (uint64_t ts : ts_vec) {
            merged.emplace_back(ch, ts);
        }
    }
    std::sort(merged.begin(), merged.end(), [](const auto& a, const auto& b) {
        return a.second < b.second;
    });
    // Write merged timestamps to output file (channel;timestamp per line)
    for (auto& [ch, ts] : merged) {
        outfile << ch << ";" << ts << "\n";
        ++total_merged;
    }
    next_merge_index++;
    // Log merging progress
    size_t remaining_buffered = 0;
    for (BufferStreamClient* stream : streams) {
        // sum sizes of all remaining (unmerged) messages in buffers
        for (size_t idx = next_merge_index; idx < stream->buffer.size(); ++idx) {
            remaining_buffered += stream->buffer[idx].size();
        }
    }
    std::cerr << "Merged timestamps for batch #" << next_merge_index 
              << " (total merged: " << total_merged 
              << ", remaining buffered: " << remaining_buffered << " bytes)" << std::endl;
}

void TimestampsMergerThread::run() {
    // Continuously check for new data to merge while acquisition is ongoing
    while (expect_more) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        // Merge as many batches as are ready
        while (all_channels_buffer_ready()) {
            merge_next_timestamp_block();
        }
    }
    // Once no more data expected, flush any remaining unmerged data (if channels ended unevenly)
    bool stillData = true;
    while (stillData) {
        stillData = false;
        // Check if any channel still has an unmerged block at current index
        for (BufferStreamClient* stream : streams) {
            if (stream->buffer.size() > next_merge_index && !stream->buffer[next_merge_index].empty()) {
                stillData = true;
                break;
            }
        }
        if (stillData) {
            // Merge whatever is available at this index from any channel(s)
            std::vector<std::pair<int, uint64_t>> merged;
            for (BufferStreamClient* stream : streams) {
                if (stream->buffer.size() > next_merge_index && !stream->buffer[next_merge_index].empty()) {
                    const std::vector<uint8_t>& msg_bytes = stream->buffer[next_merge_index];
                    size_t count = msg_bytes.size() / sizeof(uint64_t);
                    std::vector<uint64_t> timestamps(count);
                    memcpy(timestamps.data(), msg_bytes.data(), msg_bytes.size());
                    for (uint64_t& ts : timestamps) {
                        ts += sub_acquisition_pper * next_merge_index;
                    }
                    for (uint64_t ts : timestamps) {
                        merged.emplace_back(stream->number, ts);
                    }
                    // Clear the consumed data
                    const_cast<std::vector<uint8_t>&>(msg_bytes).clear();
                }
            }
            std::sort(merged.begin(), merged.end(), [](auto& a, auto& b) {
                return a.second < b.second;
            });
            for (auto& [ch, ts] : merged) {
                outfile << ch << ";" << ts << "\n";
                ++total_merged;
            }
            next_merge_index++;
        }
    }
    // Thread exits; file will be closed in destructor
}
