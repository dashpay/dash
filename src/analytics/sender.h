// Copyright (c) 2017-2021 Vincent Thiery
// Copyirght (C) 2017-2021 The cpp-statsd-client contributors
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_ANALYTICS_SENDER_H
#define BITCOIN_ANALYTICS_SENDER_H

#include <atomic>
#include <cstdint>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <thread>

#include <netbase.h>
#include <util/sock.h>

namespace Statsd {
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
    bool initialize(const std::string& str_host, uint16_t u16_port) noexcept;
    //! Queue a message to be sent to the daemon later
    void queueMessage(const std::string& message) noexcept;
    //! Send a message to the daemon
    void sendToDaemon(const std::string& message) noexcept;

private:
    std::atomic<bool> m_mustExit{false};
    //! The structure holding the server
    CService m_server;
    //! The socket to be used
    Sock m_socket;

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
}  // namespace Statsd

#endif // BITCOIN_ANALYTICS_SENDER_H
