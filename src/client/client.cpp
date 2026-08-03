#include "client.h"
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <iostream>
#include <cstring>
#include <vector>

Client::Client(const std::string& address, uint16_t port)
    : m_address(address), m_port(port), m_client_fd(-1), m_connected(false) {}

Client::~Client() {
    disconnect();
}

bool Client::connect_to_server() {
    m_client_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (m_client_fd < 0) {
        std::cerr << "Failed to create socket" << std::endl;
        return false;
    }

    struct sockaddr_in serv_addr{};
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(m_port);

    if (inet_pton(AF_INET, m_address.c_str(), &serv_addr.sin_addr) <= 0) {
        std::cerr << "Invalid address" << std::endl;
        return false;
    }

    if (connect(m_client_fd, (struct sockaddr*)&serv_addr, sizeof(serv_addr)) < 0) {
        std::cerr << "Connection failed" << std::endl;
        return false;
    }

    m_connected = true;
    return true;
}

void Client::disconnect() {
    if (m_client_fd >= 0) {
        close(m_client_fd);
        m_client_fd = -1;
    }
    m_connected = false;
}

bool Client::send_message(const std::string& message) {
    if (!m_connected) return false;
    ssize_t bytes_sent = send(m_client_fd, message.c_str(), message.length(), 0);
    return bytes_sent >= 0;
}

std::string Client::receive_reply() {
    if (!m_connected) return "";
    char buffer[1024] = {0};
    ssize_t bytes_received = recv(m_client_fd, buffer, sizeof(buffer) - 1, 0);
    if (bytes_received <= 0) return "";
    return std::string(buffer, bytes_received);
}
