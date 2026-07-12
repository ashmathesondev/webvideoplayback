#pragma once

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>

#include <cstdint>
#include <string>

namespace webvideoplayback::server {

class WinsockGuard {
public:
    WinsockGuard();
    ~WinsockGuard();
};

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

bool send_all(SOCKET socket, const char* data, int size);
bool send_all(SOCKET socket, const std::string& data);
Socket create_listener(std::uint16_t port);

} // namespace webvideoplayback::server
