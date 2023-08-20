// Copyright (c) 2017-2021 Vincent Thiery
// Copyirght (C) 2017-2021 The cpp-statsd-client contributors
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_ANALYTICS_SENDER_H
#define BITCOIN_ANALYTICS_SENDER_H

#ifdef _WIN32
#define NOMINMAX
#include <io.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include <atomic>
#include <cstdint>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <thread>

namespace Statsd {
#ifdef _WIN32
using SOCKET_TYPE = SOCKET;
constexpr SOCKET_TYPE k_invalidSocket{INVALID_SOCKET};
#define SOCKET_ERRNO WSAGetLastError()
#define SOCKET_CLOSE closesocket
#else
using SOCKET_TYPE = int;
constexpr SOCKET_TYPE k_invalidSocket{-1};
#define SOCKET_ERRNO errno
#define SOCKET_CLOSE close
#endif

class UDPSender final {
public:
    UDPSender(const std::string& host,
              const uint16_t port,
              const uint64_t batchsize,
              const uint64_t sendInterval) noexcept;
    ~UDPSender();

    UDPSender(const UDPSender&) = delete;
    UDPSender& operator=(const UDPSender&) = delete;
    UDPSender(UDPSender&&) = delete;

    void send(const std::string& message) noexcept;
    const std::string errorMessage() const noexcept;
    bool initialized() const noexcept;
    void flush() noexcept;

private:
    //! Initialize the sender and returns true when it is initialized
    bool initialize() noexcept;
    //! Queue a message to be sent to the daemon later
    void queueMessage(const std::string& message) noexcept;
    //! Send a message to the daemon
    void sendToDaemon(const std::string& message) noexcept;

private:
    std::atomic<bool> m_mustExit{false};
    //! The hostname
    std::string m_host;
    //! The port
    uint16_t m_port;
    //! The structure holding the server
    struct sockaddr_in m_server;
    //! The socket to be used
    SOCKET_TYPE m_socket = k_invalidSocket;

private:
    //! The batching size
    uint64_t m_batchsize;
    //! The sending frequency in milliseconds
    uint64_t m_sendInterval;
    //! The queue batching the messages
    std::deque<std::string> m_batchingMessageQueue;
    //! The mutex used for batching
    std::mutex m_batchingMutex;
    //! The thread dedicated to the batching
    std::thread m_batchingThread;

private:
    //! Error message (optional string)
    std::string m_errorMessage;
};

namespace detail {
inline bool isValidSocket(const SOCKET_TYPE socket) {
    return socket != k_invalidSocket;
}

#ifdef _WIN32
struct WinSockSingleton {
    inline static const WinSockSingleton& getInstance() {
        static const WinSockSingleton instance;
        return instance;
    }
    inline bool ok() const {
        return m_ok;
    }
    ~WinSockSingleton() {
        WSACleanup();
    }

private:
    WinSockSingleton() {
        WSADATA wsa;
        m_ok = WSAStartup(MAKEWORD(2, 2), &wsa) == 0;
    }
    bool m_ok;
};
#endif
}  // namespace detail
}  // namespace Statsd

#endif // BITCOIN_ANALYTICS_SENDER_H
