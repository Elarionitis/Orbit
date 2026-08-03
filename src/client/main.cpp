#include "client.h"
#include "../common/utils.h"
#include <iostream>
#include <thread>
#include <string>
#include <atomic>

int main() {
    Client client("127.0.0.1", orbit::kDefaultPort);

    if (client.connect_to_server()) {
        std::cout << "Connected to server." << std::endl;

        std::string username;
        std::cout << "Enter your username: ";
        std::getline(std::cin, username);
        if (username.empty()) {
            username = "Anonymous";
        }
        if (!client.send_message(username)) {
            std::cerr << "Failed to send username." << std::endl;
            client.disconnect();
            return 1;
        }

        std::atomic<bool> running{true};
        std::thread receive_thread([&client, &running]() {
            while (running) {
                std::string msg = client.receive_reply();
                if (msg.empty()) {
                    if (running) {
                        std::cout << "\nDisconnected from server." << std::endl;
                        running = false;
                    }
                    break;
                }
                std::cout << "\r" << msg << "\n" << "> " << std::flush;
            }
        });

        std::string message;
        while (running) {
            std::cout << "> " << std::flush;
            if (!std::getline(std::cin, message)) break;
            if (message == "/quit") break;
            if (!client.send_message(message)) {
                std::cerr << "Failed to send message." << std::endl;
                break;
            }
        }

        running = false;
        client.disconnect();
        if (receive_thread.joinable()) receive_thread.join();
        std::cout << "Client shut down." << std::endl;
    } else {
        std::cerr << "Could not connect to server." << std::endl;
        return 1;
    }

    return 0;
}
