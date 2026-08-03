#ifndef ORBIT_SERVER_H
#define ORBIT_SERVER_H

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

class Server {
public:
    explicit Server(uint16_t port);
    ~Server();

    // Prevent copying to maintain unique ownership of the socket file descriptor (RAII)
    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;

    // Support move semantics
    Server(Server&& other) noexcept;
    Server& operator=(Server&& other) noexcept;

    // Bootstrap: Create socket, bind to localhost, and start listening
    bool start();

    // Accept and handle incoming connections
    void accept_clients();

    // Cleanup resources and stop server
    void stop();

    // Broadcast a message to all clients except the sender
    void broadcast(const std::string& message, int sender_fd);

    // Query server state
    bool is_running() const { return m_running.load(); }
    int get_socket_fd() const { return m_server_fd; }
    uint16_t get_port() const { return m_port; }

private:
    void handle_client(int client_fd, std::string ip, uint16_t port);

    uint16_t m_port;
    int m_server_fd;
    std::atomic<bool> m_running;

    std::vector<int> m_client_fds;
    std::vector<std::thread> m_client_threads;
    std::mutex m_clients_mutex;
};

#endif // ORBIT_SERVER_H
