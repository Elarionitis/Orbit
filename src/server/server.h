#ifndef NEXUS_SERVER_H
#define NEXUS_SERVER_H

#include <cstdint>

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

    // Cleanup resources
    void stop();

    // Accept and handle incoming connections
    void accept_clients();

    // Query server state
    bool is_running() const { return m_running; }
    int get_socket_fd() const { return m_server_fd; }
    uint16_t get_port() const { return m_port; }

private:
    uint16_t m_port;
    int m_server_fd;
    bool m_running;
};

#endif // NEXUS_SERVER_H
