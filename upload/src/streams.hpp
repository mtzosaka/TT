#ifndef STREAMS_HPP
#define STREAMS_HPP

#include <vector>
#include <cstdint>
#include <thread>
#include <mutex>
#include <atomic>
#include <fstream>
#include <zmq.hpp>

// Forward declaration
class TimestampsMergerThread;

// Client that connects to a DLT timestamp stream (ZMQ PAIR) for one channel and buffers incoming data
class BufferStreamClient {
public:
    explicit BufferStreamClient(int channel);
    ~BufferStreamClient();

    // Start the receiver thread
    void start();
    // Signal the receiver thread to stop and wait for it to finish
    void join();

    // Channel number accessor
    int channel_number() const { return number; }
    // (The buffer is accessed by the merger thread directly)

    // Expose port (for use in constructing DLT command)
    int port;

    // Friend the merger thread class to allow direct buffer access
    friend class TimestampsMergerThread;

private:
    void run();  // Thread loop function for receiving data

    int number;
    zmq::socket_t data_socket;
    zmq::socket_t monitor_socket;
    std::atomic<bool> running;
    std::vector<std::vector<uint8_t>> buffer;  // Buffer of raw message data for this channel
    std::thread recv_thread;
};

// Thread that merges timestamps from multiple BufferStreamClients
class TimestampsMergerThread {
public:
    TimestampsMergerThread(const std::vector<BufferStreamClient*>& streams, const std::string& output_path, uint64_t sub_acquisition_pper);
    ~TimestampsMergerThread();

    // Start the merging thread
    void start();
    // Signal to stop (no more incoming data expected) and wait for thread to finish
    void join();

private:
    void run();                            // Thread loop for merging logic
    bool all_channels_buffer_ready();      // Check if all streams have an unmerged message at current index
    void merge_next_timestamp_block();     // Merge one batch of timestamps (current index) from all channels

    std::vector<BufferStreamClient*> streams;
    std::atomic<bool> expect_more;
    std::thread merge_thread;
    std::ofstream outfile;
    uint64_t sub_acquisition_pper;  // period (interval) of sub-acquisition in picoseconds
    size_t next_merge_index;
    uint64_t total_merged;
};

#endif // STREAMS_HPP
