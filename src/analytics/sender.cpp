// Copyright (c) 2017-2021 Vincent Thiery
// Copyirght (C) 2017-2021 The cpp-statsd-client contributors
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <analytics/sender.h>

namespace Statsd {
UDPSender::UDPSender(const std::string& host,
                     const uint16_t port,
                     const uint64_t batchsize,
                     const uint64_t sendInterval) noexcept
    : m_host(host), m_port(port), m_batchsize(batchsize), m_sendInterval(sendInterval) {
    // Initialize the socket
    if (!initialize()) {
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
    SOCKET_CLOSE(m_socket);
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

bool UDPSender::initialize() noexcept {
#ifdef _WIN32
    if (!detail::WinSockSingleton::getInstance().ok()) {
        m_errorMessage = "WSAStartup failed: errno=" + std::to_string(SOCKET_ERRNO);
    }
#endif

    // Connect the socket
    m_socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (!detail::isValidSocket(m_socket)) {
        m_errorMessage = "socket creation failed: errno=" + std::to_string(SOCKET_ERRNO);
        return false;
    }

    std::memset(&m_server, 0, sizeof(m_server));
    m_server.sin_family = AF_INET;
    m_server.sin_port = htons(m_port);

    if (inet_pton(AF_INET, m_host.c_str(), &m_server.sin_addr) == 0) {
        // An error code has been returned by inet_aton

        // Specify the criteria for selecting the socket address structure
        struct addrinfo hints;
        std::memset(&hints, 0, sizeof(hints));
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_DGRAM;

        // Get the address info using the hints
        struct addrinfo* results = nullptr;
        const int ret{getaddrinfo(m_host.c_str(), nullptr, &hints, &results)};
        if (ret != 0) {
            // An error code has been returned by getaddrinfo
            SOCKET_CLOSE(m_socket);
            m_socket = k_invalidSocket;
            m_errorMessage = "getaddrinfo failed: err=" + std::to_string(ret) + ", msg=" + gai_strerror(ret);
            return false;
        }

        // Copy the results in m_server
        struct sockaddr_in* host_addr = (struct sockaddr_in*)results->ai_addr;
        std::memcpy(&m_server.sin_addr, &host_addr->sin_addr, sizeof(struct in_addr));

        // Free the memory allocated
        freeaddrinfo(results);
    }

    return true;
}

void UDPSender::sendToDaemon(const std::string& message) noexcept {
    // Try sending the message
    const auto ret = sendto(m_socket,
                            message.data(),
#ifdef _WIN32
                            static_cast<int>(message.size()),
#else
                            message.size(),
#endif
                            0,
                            (struct sockaddr*)&m_server,
                            sizeof(m_server));
    if (ret == -1) {
        m_errorMessage = "sendto server failed: host=" + m_host + ":" + std::to_string(m_port) +
                         ", err=" + std::to_string(SOCKET_ERRNO);
    }
}

bool UDPSender::initialized() const noexcept {
    return m_socket != k_invalidSocket;
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
