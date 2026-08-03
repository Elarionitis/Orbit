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
