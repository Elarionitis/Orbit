#include "server.h"
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <iostream>
#include <cstring>
#include <utility>

Server::Server(uint16_t port)
    : m_port(port), m_server_fd(-1), m_running(false) {}

Server::~Server() {
    stop();
}

Server::Server(Server&& other) noexcept
    : m_port(other.m_port), m_server_fd(other.m_server_fd), m_running(other.m_running) {
    other.m_server_fd = -1;
    other.m_running = false;
}

Server& Server::operator=(Server&& other) noexcept {
    if (this != &other) {
        stop();
        m_port = other.m_port;
        m_server_fd = other.m_server_fd;
        m_running = other.m_running;
        other.m_server_fd = -1;
        other.m_running = false;
    }
    return *this;
}

bool Server::start() {
    if (m_running) {
        std::cerr << "Server is already running on port " << m_port << std::endl;
        return false;
    }

    // 1. Create IPv4 TCP Socket
    m_server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (m_server_fd < 0) {
        std::cerr << "Failed to create socket: " << strerror(errno) << std::endl;
        return false;
    }

    // Enable SO_REUSEADDR to avoid "Address already in use" errors on restart
    int opt = 1;
    if (setsockopt(m_server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        std::cerr << "Failed to set socket options: " << strerror(errno) << std::endl;
        stop();
        return false;
    }

    // 2. Configure Address (Bind to localhost)
    struct sockaddr_in address{};
    std::memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_port = htons(m_port);
    
    // Bind to 127.0.0.1 (localhost)
    if (inet_pton(AF_INET, "127.0.0.1", &address.sin_addr) <= 0) {
        std::cerr << "Invalid address/ Address not supported" << std::endl;
        stop();
        return false;
    }

    // 3. Bind
    if (bind(m_server_fd, (struct sockaddr*)&address, sizeof(address)) < 0) {
        std::cerr << "Failed to bind to port " << m_port << ": " << strerror(errno) << std::endl;
        stop();
        return false;
    }

    // 4. Start Listening
    if (listen(m_server_fd, SOMAXCONN) < 0) {
        std::cerr << "Failed to listen on socket: " << strerror(errno) << std::endl;
        stop();
        return false;
    }

    m_running = true;
    std::cout << "Server successfully started. Listening on 127.0.0.1:" << m_port << std::endl;
    return true;
}

void Server::stop() {
    if (m_server_fd >= 0) {
        // Close socket and reset m_server_fd
        close(m_server_fd);
        m_server_fd = -1;
    }
    m_running = false;
}

void Server::accept_clients() {
    if (!m_running) return;

    while (m_running) {
        struct sockaddr_in client_addr{};
        socklen_t client_len = sizeof(client_addr);

        int client_fd = accept(m_server_fd, (struct sockaddr*)&client_addr, &client_len);

        if (client_fd < 0) {
            if (m_running) {
                std::cerr << "Accept failed: " << strerror(errno) << std::endl;
            }
            continue;
        }

        char client_ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &client_addr.sin_addr, client_ip, INET_ADDRSTRLEN);
        uint16_t client_port = ntohs(client_addr.sin_port);

        std::cout << "Client connected: " << client_ip << ":" << client_port << std::endl;

        // Simple echo/reply handling
        char buffer[1024] = {0};
        ssize_t bytes_received = recv(client_fd, buffer, sizeof(buffer) - 1, 0);
        if (bytes_received > 0) {
            std::cout << "Received: " << buffer << std::endl;
            const char* reply = "Message received";
            send(client_fd, reply, strlen(reply), 0);
        }

        // Cleanly close client socket after handling
        close(client_fd);
        std::cout << "Client disconnected: " << client_ip << ":" << client_port << std::endl;
    }
}
