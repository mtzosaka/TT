#include <iostream>
#include <string>
#include <thread>
#include <chrono>
#include <zmq.hpp>

// Test program to verify network communication between master and slave
class NetworkTest {
public:
    NetworkTest(bool is_master, const std::string& remote_addr) 
        : context_(1), is_master_(is_master), remote_addr_(remote_addr) {
    }
    
    bool run_test() {
        std::cout << "Running network test as " << (is_master_ ? "MASTER" : "SLAVE") << std::endl;
        std::cout << "Remote address: " << remote_addr_ << std::endl;
        
        try {
            // Test 1: Basic connectivity
            if (!test_basic_connectivity()) {
                std::cerr << "Basic connectivity test failed" << std::endl;
                return false;
            }
            
            // Test 2: Latency measurement
            if (!test_latency()) {
                std::cerr << "Latency test failed" << std::endl;
                return false;
            }
            
            // Test 3: Throughput measurement
            if (!test_throughput()) {
                std::cerr << "Throughput test failed" << std::endl;
                return false;
            }
            
            std::cout << "All network tests passed successfully!" << std::endl;
            return true;
        }
        catch (const std::exception& e) {
            std::cerr << "Error during network test: " << e.what() << std::endl;
            return false;
        }
    }
    
private:
    bool test_basic_connectivity() {
        std::cout << "Testing basic connectivity..." << std::endl;
        
        zmq::socket_t socket(context_, is_master_ ? zmq::socket_type::req : zmq::socket_type::rep);
        
        if (is_master_) {
            socket.connect("tcp://" + remote_addr_ + ":5555");
            
            // Send ping
            std::string ping_msg = "PING";
            zmq::message_t request(ping_msg.size());
            memcpy(request.data(), ping_msg.c_str(), ping_msg.size());
            
            socket.send(request, zmq::send_flags::none);
            std::cout << "Sent: " << ping_msg << std::endl;
            
            // Receive pong
            zmq::message_t reply;
            auto result = socket.recv(reply, zmq::recv_flags::none);
            
            if (!result.has_value()) {
                std::cerr << "No reply received" << std::endl;
                return false;
            }
            
            std::string pong_msg(static_cast<char*>(reply.data()), reply.size());
            std::cout << "Received: " << pong_msg << std::endl;
            
            return pong_msg == "PONG";
        }
        else {
            socket.bind("tcp://*:5555");
            
            // Receive ping
            zmq::message_t request;
            auto result = socket.recv(request, zmq::recv_flags::none);
            
            if (!result.has_value()) {
                std::cerr << "No request received" << std::endl;
                return false;
            }
            
            std::string ping_msg(static_cast<char*>(request.data()), request.size());
            std::cout << "Received: " << ping_msg << std::endl;
            
            // Send pong
            std::string pong_msg = "PONG";
            zmq::message_t reply(pong_msg.size());
            memcpy(reply.data(), pong_msg.c_str(), pong_msg.size());
            
            socket.send(reply, zmq::send_flags::none);
            std::cout << "Sent: " << pong_msg << std::endl;
            
            return ping_msg == "PING";
        }
    }
    
    bool test_latency() {
        std::cout << "Testing network latency..." << std::endl;
        
        zmq::socket_t socket(context_, is_master_ ? zmq::socket_type::req : zmq::socket_type::rep);
        
        if (is_master_) {
            socket.connect("tcp://" + remote_addr_ + ":5556");
            
            const int num_iterations = 100;
            double total_latency_ms = 0.0;
            
            for (int i = 0; i < num_iterations; ++i) {
                // Send ping with timestamp
                auto start_time = std::chrono::high_resolution_clock::now();
                
                std::string ping_msg = "PING";
                zmq::message_t request(ping_msg.size());
                memcpy(request.data(), ping_msg.c_str(), ping_msg.size());
                
                socket.send(request, zmq::send_flags::none);
                
                // Receive pong
                zmq::message_t reply;
                auto result = socket.recv(reply, zmq::recv_flags::none);
                
                auto end_time = std::chrono::high_resolution_clock::now();
                auto latency = std::chrono::duration_cast<std::chrono::microseconds>(end_time - start_time);
                
                double latency_ms = latency.count() / 1000.0;
                total_latency_ms += latency_ms;
                
                std::cout << "Iteration " << (i + 1) << ": Latency = " << latency_ms << " ms" << std::endl;
                
                // Small delay between iterations
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            
            double avg_latency_ms = total_latency_ms / num_iterations;
            std::cout << "Average latency: " << avg_latency_ms << " ms" << std::endl;
            
            // Consider test passed if average latency is below 10ms
            return avg_latency_ms < 10.0;
        }
        else {
            socket.bind("tcp://*:5556");
            
            // Process ping-pong requests for 5 seconds
            auto start_time = std::chrono::steady_clock::now();
            
            while (std::chrono::duration_cast<std::chrono::seconds>(
                   std::chrono::steady_clock::now() - start_time).count() < 5) {
                
                zmq::message_t request;
                auto result = socket.recv(request, zmq::recv_flags::dontwait);
                
                if (result.has_value()) {
                    // Echo back the same message
                    socket.send(request, zmq::send_flags::none);
                }
                
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            
            return true;
        }
    }
    
    bool test_throughput() {
        std::cout << "Testing network throughput..." << std::endl;
        
        if (is_master_) {
            zmq::socket_t socket(context_, zmq::socket_type::push);
            socket.connect("tcp://" + remote_addr_ + ":5557");
            
            // Allow time for connection to establish
            std::this_thread::sleep_for(std::chrono::seconds(1));
            
            // Send 100MB of data in 64KB chunks
            const size_t chunk_size = 64 * 1024;
            const size_t total_size = 100 * 1024 * 1024;
            const size_t num_chunks = total_size / chunk_size;
            
            std::vector<char> data(chunk_size, 'A');
            
            auto start_time = std::chrono::high_resolution_clock::now();
            
            for (size_t i = 0; i < num_chunks; ++i) {
                zmq::message_t message(chunk_size);
                memcpy(message.data(), data.data(), chunk_size);
                
                socket.send(message, zmq::send_flags::none);
                
                if (i % 100 == 0) {
                    std::cout << "Sent " << (i * chunk_size / (1024 * 1024)) << " MB" << std::endl;
                }
            }
            
            auto end_time = std::chrono::high_resolution_clock::now();
            auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);
            
            double seconds = duration.count() / 1000.0;
            double mbps = (total_size / (1024.0 * 1024.0)) / seconds;
            
            std::cout << "Throughput: " << mbps << " MB/s" << std::endl;
            
            // Consider test passed if throughput is above 10 MB/s
            return mbps > 10.0;
        }
        else {
            zmq::socket_t socket(context_, zmq::socket_type::pull);
            socket.bind("tcp://*:5557");
            
            // Receive data for 30 seconds or until 100MB is received
            const size_t total_size = 100 * 1024 * 1024;
            size_t bytes_received = 0;
            
            auto start_time = std::chrono::steady_clock::now();
            
            while (bytes_received < total_size && 
                   std::chrono::duration_cast<std::chrono::seconds>(
                   std::chrono::steady_clock::now() - start_time).count() < 30) {
                
                zmq::message_t message;
                auto result = socket.recv(message, zmq::recv_flags::dontwait);
                
                if (result.has_value()) {
                    bytes_received += message.size();
                    
                    if (bytes_received % (10 * 1024 * 1024) == 0) {
                        std::cout << "Received " << (bytes_received / (1024 * 1024)) << " MB" << std::endl;
                    }
                }
                
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            
            auto end_time = std::chrono::steady_clock::now();
            auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);
            
            double seconds = duration.count() / 1000.0;
            double mbps = (bytes_received / (1024.0 * 1024.0)) / seconds;
            
            std::cout << "Received " << (bytes_received / (1024 * 1024)) << " MB in " << seconds << " seconds" << std::endl;
            std::cout << "Throughput: " << mbps << " MB/s" << std::endl;
            
            return bytes_received > 0;
        }
    }
    
    zmq::context_t context_;
    bool is_master_;
    std::string remote_addr_;
};

int main(int argc, char* argv[]) {
    if (argc < 3) {
        std::cerr << "Usage: network_test [master|slave] <remote_address>" << std::endl;
        return 1;
    }
    
    std::string mode = argv[1];
    std::string remote_addr = argv[2];
    
    bool is_master = (mode == "master");
    
    NetworkTest test(is_master, remote_addr);
    
    if (!test.run_test()) {
        std::cerr << "Network test failed" << std::endl;
        return 1;
    }
    
    return 0;
}
