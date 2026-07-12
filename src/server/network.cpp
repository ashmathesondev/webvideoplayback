#include "server/network.hpp"

#include <ws2tcpip.h>

#include <stdexcept>

namespace webvideoplayback::server {

WinsockGuard::WinsockGuard()
{
    WSADATA data = {};
    if (WSAStartup(MAKEWORD(2, 2), &data) != 0) {
        throw std::runtime_error("WSAStartup failed");
    }
}

WinsockGuard::~WinsockGuard()
{
    WSACleanup();
}

Socket::Socket(SOCKET socket)
    : socket_(socket)
{
}

Socket::Socket(Socket&& other) noexcept
    : socket_(other.socket_)
{
    other.socket_ = INVALID_SOCKET;
}

Socket& Socket::operator=(Socket&& other) noexcept
{
    if (this != &other) {
        close();
        socket_ = other.socket_;
        other.socket_ = INVALID_SOCKET;
    }
    return *this;
}

Socket::~Socket()
{
    close();
}

SOCKET Socket::get() const
{
    return socket_;
}

bool Socket::valid() const
{
    return socket_ != INVALID_SOCKET;
}

void Socket::close()
{
    if (socket_ != INVALID_SOCKET) {
        closesocket(socket_);
        socket_ = INVALID_SOCKET;
    }
}

bool send_all(SOCKET socket, const char* data, int size)
{
    int sent = 0;
    while (sent < size) {
        const int result = send(socket, data + sent, size - sent, 0);
        if (result == SOCKET_ERROR || result == 0) {
            return false;
        }
        sent += result;
    }
    return true;
}

bool send_all(SOCKET socket, const std::string& data)
{
    return send_all(socket, data.data(), static_cast<int>(data.size()));
}

Socket create_listener(std::uint16_t port)
{
    Socket listener(socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
    if (!listener.valid()) {
        throw std::runtime_error("socket failed");
    }

    BOOL reuse = TRUE;
    setsockopt(listener.get(), SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse), sizeof(reuse));

    sockaddr_in address = {};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(port);

    if (bind(listener.get(), reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == SOCKET_ERROR) {
        throw std::runtime_error("bind failed");
    }
    if (listen(listener.get(), SOMAXCONN) == SOCKET_ERROR) {
        throw std::runtime_error("listen failed");
    }

    return listener;
}

} // namespace webvideoplayback::server
