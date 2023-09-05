// Copyright (c) 2017-2021 Vincent Thiery
// Copyirght (C) 2017-2021 The cpp-statsd-client contributors
// Copyright (c) 2023 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <analytics/sender.h>

namespace Statsd {
UDPSender::UDPSender(const std::string& host,
                     const uint16_t port,
                     const uint64_t batchsize,
                     const uint64_t sendInterval) noexcept
    : m_batchsize(batchsize), m_sendInterval(sendInterval) {
    // Initialize the socket
    if (!initialize(host, port)) {
        return;
    }
    // If batching is on, use a dedicated thread to send after the wait time is reached
    if (m_batchsize != 0 && m_sendInterval > 0) {
        // Define the batching thread
        m_batchingThread = std::thread([this] {
            // TODO: this will drop unsent stats, should we send all the unsent stats before we exit?
            while (!m_mustExit.load(std::memory_order_acquire)) {
                std::deque<std::string> stagedMessageQueue;

                std::unique_lock<std::mutex> batchingLock(m_batchingMutex);
                m_batchingMessageQueue.swap(stagedMessageQueue);
                batchingLock.unlock();

                // Flush the queue
                while (!stagedMessageQueue.empty()) {
                    sendToDaemon(stagedMessageQueue.front());
                    stagedMessageQueue.pop_front();
                }

                // Wait before sending the next batch
                std::this_thread::sleep_for(std::chrono::milliseconds(m_sendInterval));
            }
        });
    }
}

UDPSender::~UDPSender() {
    if (!initialized()) {
        return;
    }

    // If we're running a background thread tell it to stop
    if (m_batchingThread.joinable()) {
        m_mustExit.store(true, std::memory_order_release);
        m_batchingThread.join();
    }

    // Cleanup the socket
    m_socket.Reset();
}

void UDPSender::send(const std::string& message) noexcept {
    m_errorMessage.clear();

    // If batching is on, accumulate messages in the queue
    if (m_batchsize > 0) {
        queueMessage(message);
        return;
    }

    // Or send it right now
    sendToDaemon(message);
}

void UDPSender::queueMessage(const std::string& message) noexcept {
    // We aquire a lock but only if we actually need to (i.e. there is a thread also accessing the queue)
    auto batchingLock =
        m_batchingThread.joinable() ? std::unique_lock<std::mutex>(m_batchingMutex) : std::unique_lock<std::mutex>();
    // Either we don't have a place to batch our message or we exceeded the batch size, so make a new batch
    if (m_batchingMessageQueue.empty() || m_batchingMessageQueue.back().length() > m_batchsize) {
        m_batchingMessageQueue.emplace_back();
        m_batchingMessageQueue.back().reserve(m_batchsize + 256);
    }  // When there is already a batch open we need a separator when its not empty
    else if (!m_batchingMessageQueue.back().empty()) {
        m_batchingMessageQueue.back().push_back('\n');
    }
    // Add the new message to the batch
    m_batchingMessageQueue.back().append(message);
}

const std::string UDPSender::errorMessage() const noexcept {
    return m_errorMessage;
}

bool UDPSender::initialize(const std::string& str_host, uint16_t u16_port) noexcept {
    /* Resolve address */
    std::vector<CNetAddr> resolved;
    if (!LookupHost(str_host, resolved, /* nMaxSolutions */ 256, /* fAllowLookup */ true, WrappedGetAddrInfoUDP) || resolved.empty()) {
        // cannot resolve address
        return false;
    }
    m_server = CService(resolved.front(), u16_port);
    if (!m_server.IsValid()) {
        // resolver returned invalid address
        return false;
    }
    /* Create socket to resolved address */
    std::unique_ptr<Sock> socket = CreateSockUDP(m_server);
    if (!socket) {
        // unable to create socket
        return false;
    }
    /* Connect to socket  */
    if (!ConnectSocketDirectly(m_server, *socket, DEFAULT_CONNECT_TIMEOUT, /* manual_connection */ false)) {
        // inform that we eren't able to connect _directly_
        socket->Reset();
        return false;
    }
    /* Move unique pointer to member object */
    m_socket = std::move(*socket);
    return true;
}

void UDPSender::sendToDaemon(const std::string& message) noexcept {
    if (m_socket.Send(message.data(), message.size(), 0) == -1) {
        m_errorMessage = strprintf(
            "sendto server failed: host=%s, err=%s",
            m_server.ToStringIPPort(/* fUseGetnameinfo */ true),
            NetworkErrorString(WSAGetLastError())
        );
    }
}

bool UDPSender::initialized() const noexcept {
    return m_socket.Get() != INVALID_SOCKET;
}

void UDPSender::flush() noexcept {
    // We aquire a lock but only if we actually need to (ie there is a thread also accessing the queue)
    auto batchingLock =
        m_batchingThread.joinable() ? std::unique_lock<std::mutex>(m_batchingMutex) : std::unique_lock<std::mutex>();
    // Flush the queue
    while (!m_batchingMessageQueue.empty()) {
        sendToDaemon(m_batchingMessageQueue.front());
        m_batchingMessageQueue.pop_front();
    }
}
}  // namespace Statsd
