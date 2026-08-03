#include "server.h"
#include "../common/utils.h"
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <iostream>
#include <cstring>
#include <utility>
#include <algorithm>

Server::Server(uint16_t port)
    : m_port(port), m_server_fd(-1), m_running(false) {}

Server::~Server() {
    stop();
}

Server::Server(Server&& other) noexcept
    : m_port(other.m_port), m_server_fd(other.m_server_fd), m_running(other.m_running.load()) {
    other.m_server_fd = -1;
    other.m_running = false;
}

Server& Server::operator=(Server&& other) noexcept {
    if (this != &other) {
        stop();
        m_port = other.m_port;
        m_server_fd = other.m_server_fd;
        m_running = other.m_running.load();
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
    m_running = false;

    if (m_server_fd >= 0) {
        close(m_server_fd);
        m_server_fd = -1;
    }

    std::vector<std::thread> client_threads;
    {
        std::lock_guard<std::mutex> lock(m_clients_mutex);
        for (int fd : m_client_fds) {
            close(fd);
        }
        m_client_fds.clear();
        client_threads.swap(m_client_threads);
    }

    for (auto& client_thread : client_threads) {
        if (client_thread.joinable()) {
            client_thread.join();
        }
    }
}

void Server::accept_clients() {
    if (!m_running) return;

    while (m_running) {
        struct sockaddr_in client_addr{};
        socklen_t client_len = sizeof(client_addr);

        int client_fd = accept(m_server_fd, (struct sockaddr*)&client_addr, &client_len);

        if (client_fd < 0) {
            if (m_running) std::cerr << "Accept failed: " << strerror(errno) << std::endl;
            continue;
        }

        if (!m_running) {
            close(client_fd);
            break;
        }

        char client_ip[INET_ADDRSTRLEN] = {};
        if (inet_ntop(AF_INET, &client_addr.sin_addr, client_ip, sizeof(client_ip)) == nullptr) {
            std::strcpy(client_ip, "unknown");
        }
        uint16_t client_port = ntohs(client_addr.sin_port);

        std::lock_guard<std::mutex> lock(m_clients_mutex);
        m_client_fds.push_back(client_fd);
        m_client_threads.emplace_back(&Server::handle_client, this, client_fd, std::string(client_ip), client_port);
    }
}

void Server::handle_client(int client_fd, std::string ip, uint16_t port) {
    std::cout << "[" << orbit::get_timestamp() << "] Connection from " << ip << ":" << port << std::endl;

    char buffer[orbit::kBufferSize];
    std::string username = "Unknown";

    // First message is expected to be the username
    ssize_t bytes_received = recv(client_fd, buffer, sizeof(buffer) - 1, 0);
    if (bytes_received > 0) {
        username.assign(buffer, bytes_received);
        std::cout << "[" << orbit::get_timestamp() << "] " << ip << ":" << port << " identified as: " << username << std::endl;
        broadcast("[" + orbit::get_timestamp() + "] " + username + " joined the chat.", client_fd);
    }

    while (m_running) {
        bytes_received = recv(client_fd, buffer, sizeof(buffer) - 1, 0);

        if (bytes_received <= 0) {
            if (bytes_received < 0 && m_running) {
                std::cerr << "[" << orbit::get_timestamp() << "] Error receiving from " << username << ": " << strerror(errno) << std::endl;
            }
            break;
        }

        const std::string message = "[" + orbit::get_timestamp() + "] " + username + ": " +
                                    std::string(buffer, bytes_received);
        std::cout << message << std::endl;
        broadcast(message, client_fd);
    }

    std::cout << "[" << orbit::get_timestamp() << "] " << username << " (" << ip << ":" << port << ") disconnected." << std::endl;
    broadcast("[" + orbit::get_timestamp() + "] " + username + " left the chat.", client_fd);

    close(client_fd);

    std::lock_guard<std::mutex> lock(m_clients_mutex);
    m_client_fds.erase(std::remove(m_client_fds.begin(), m_client_fds.end(), client_fd), m_client_fds.end());
}

void Server::broadcast(const std::string& message, int sender_fd) {
    std::lock_guard<std::mutex> lock(m_clients_mutex);
    for (int fd : m_client_fds) {
        if (fd != sender_fd) {
            if (send(fd, message.c_str(), message.length(), 0) < 0) {
                std::cerr << "Failed to send message to client " << fd << ": " << strerror(errno) << std::endl;
            }
        }
    }
}
