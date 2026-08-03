#include "client.h"
#include <iostream>

int main() {
    Client client("127.0.0.1", 8080);

    if (client.connect_to_server()) {
        std::cout << "Connected to server." << std::endl;

        std::string message = "Hello, Server!";
        if (client.send_message(message)) {
            std::cout << "Sent: " << message << std::endl;

            std::string reply = client.receive_reply();
            if (!reply.empty()) {
                std::cout << "Received reply: " << reply << std::endl;
            }
        }

        client.disconnect();
        std::cout << "Disconnected from server." << std::endl;
    } else {
        std::cerr << "Could not connect to server." << std::endl;
        return 1;
    }

    return 0;
}
