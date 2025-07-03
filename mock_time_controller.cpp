#include <iostream>
#include <string>
#include <vector>
#include <thread>
#include <chrono>
#include <zmq.hpp>

// Simple test client to simulate a Time Controller for testing
class MockTimeController {
public:
    MockTimeController(int port = 5555) : context_(1), socket_(context_, zmq::socket_type::rep) {
        socket_.bind("tcp://*:" + std::to_string(port));
        running_ = true;
    }
    
    ~MockTimeController() {
        stop();
    }
    
    void start() {
        thread_ = std::thread(&MockTimeController::run, this);
    }
    
    void stop() {
        running_ = false;
        if (thread_.joinable()) {
            thread_.join();
        }
        socket_.close();
    }
    
private:
    void run() {
        std::cout << "Mock Time Controller started" << std::endl;
        
        while (running_) {
            zmq::message_t request;
            
            try {
                auto result = socket_.recv(request, zmq::recv_flags::dontwait);
                if (result.has_value()) {
                    std::string cmd(static_cast<char*>(request.data()), request.size());
                    std::cout << "Received command: " << cmd << std::endl;
                    
                    std::string response;
                    
                    if (cmd == "*IDN?") {
                        response = "MOCK,TimeController,1.0,12345";
                    }
                    else if (cmd.find("REC:") == 0 || cmd.find("RAW") == 0) {
                        response = "OK";
                    }
                    else {
                        response = "ERROR: Unknown command";
                    }
                    
                    zmq::message_t reply(response.size());
                    memcpy(reply.data(), response.c_str(), response.size());
                    socket_.send(reply, zmq::send_flags::none);
                }
            }
            catch (const zmq::error_t& e) {
                if (e.num() != EAGAIN) {
                    std::cerr << "ZeroMQ error: " << e.what() << std::endl;
                }
            }
            
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        
        std::cout << "Mock Time Controller stopped" << std::endl;
    }
    
    zmq::context_t context_;
    zmq::socket_t socket_;
    std::thread thread_;
    bool running_;
};

int main(int argc, char* argv[]) {
    int port = 5555;
    
    if (argc > 1) {
        port = std::stoi(argv[1]);
    }
    
    std::cout << "Starting Mock Time Controller on port " << port << std::endl;
    
    MockTimeController controller(port);
    controller.start();
    
    std::cout << "Press Enter to stop..." << std::endl;
    std::cin.get();
    
    controller.stop();
    
    return 0;
}
