#include "server.h"
#include <iostream>
#include <csignal>
#include <thread>
#include <chrono>
#include <string>

namespace {
    volatile std::sig_atomic_t g_signal_status = 0;
}

void signal_handler(int signal) {
    g_signal_status = signal;
}

int main(int argc, char* argv[]) {
    uint16_t port = 8080;
    
    // Parse configurable port
    if (argc > 1) {
        try {
            int parsed_port = std::stoi(argv[1]);
            if (parsed_port < 1 || parsed_port > 65535) {
                std::cerr << "Error: Port must be in range 1-65535." << std::endl;
                return 1;
            }
            port = static_cast<uint16_t>(parsed_port);
        } catch (const std::exception& e) {
            std::cerr << "Error: Invalid port argument specified: " << e.what() << std::endl;
            return 1;
        }
    }

    // Register signal handlers for clean exit
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    Server server(port);
    if (!server.start()) {
        std::cerr << "Failed to bootstrap Nexus Server." << std::endl;
        return 1;
    }

    std::cout << "Nexus Server is running. Press Ctrl+C to stop gracefully." << std::endl;
    
    // Create a thread for accepting clients since accept() is blocking
    std::thread accept_thread([&server]() {
        server.accept_clients();
    });

    // Loop until we receive interrupt or termination signal
    while (g_signal_status == 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    std::cout << "\nInterrupted. Shutting down server gracefully..." << std::endl;
    server.stop();

    if (accept_thread.joinable()) {
        accept_thread.join();
    }
    std::cout << "Server shutdown successfully." << std::endl;

    return 0;
}
