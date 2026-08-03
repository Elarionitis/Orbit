#ifndef NEXUS_CLIENT_H
#define NEXUS_CLIENT_H

#include <string>
#include <cstdint>

class Client {
public:
    Client(const std::string& address, uint16_t port);
    ~Client();

    bool connect_to_server();
    void disconnect();

    bool send_message(const std::string& message);
    std::string receive_reply();

private:
    std::string m_address;
    uint16_t m_port;
    int m_client_fd;
    bool m_connected;
};

#endif // NEXUS_CLIENT_H
