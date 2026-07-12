#pragma once

// Winsock network primitives for the local test server.
//
// The server module keeps HTTP parsing and routing separate from socket
// lifetime management. This file is Windows-specific by design.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>

#include <cstdint>
#include <string>

namespace webvideoplayback::server {

// Initializes and tears down Winsock for the server process.
class WinsockGuard {
public:
    WinsockGuard();
    ~WinsockGuard();
};

// Move-only RAII wrapper for SOCKET handles.
class Socket {
public:
    explicit Socket(SOCKET socket = INVALID_SOCKET);
    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;
    Socket(Socket&& other) noexcept;
    Socket& operator=(Socket&& other) noexcept;
    ~Socket();

    SOCKET get() const;
    bool valid() const;

private:
    void close();

    SOCKET socket_ = INVALID_SOCKET;
};

// Sends an entire buffer unless the peer disconnects.
bool send_all(SOCKET socket, const char* data, int size);
bool send_all(SOCKET socket, const std::string& data);

// Creates a loopback TCP listener on the requested port.
Socket create_listener(std::uint16_t port);

} // namespace webvideoplayback::server
