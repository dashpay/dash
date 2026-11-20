// Copyright (c) 2020-2024 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_UTIL_EDGE_H
#define BITCOIN_UTIL_EDGE_H

#include <sync.h>
#include <util/sock.h>

#include <chrono>
#include <memory>
#include <unordered_map>

/**
 * A manager for abstracting logic surrounding edge-triggered socket events
 * modes like kqueue and epoll.
 */
class EdgeTriggeredEvents
{
public:
    explicit EdgeTriggeredEvents(SocketEventsMode events_mode);
    ~EdgeTriggeredEvents();

    bool IsValid() const { return m_valid; }

    /* Add socket to interest list */
    bool AddSocket(const std::shared_ptr<const Sock>& sock) EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);
    /* Remove socket from interest list */
    bool RemoveSocket(const std::shared_ptr<const Sock>& sock) EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);

    /* Register events for socket */
    bool RegisterEvents(const std::shared_ptr<const Sock>& sock) EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);
    /* Unregister events for socket */
    bool UnregisterEvents(const std::shared_ptr<const Sock>& sock) EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);

    /**
     * Wait for events on the sockets added or registered so far.
     * @param[in] timeout Wait this long for at least one event to occur.
     * @param[out] events_per_sock Replaced with the sockets that have events and what occurred on
     * them. Unlike `Sock::WaitMany()`, the caller does not choose the sockets to wait on.
     * @return true on success (or timeout, if `events_per_sock` is returned empty), false otherwise
     */
    [[nodiscard]] bool Wait(std::chrono::milliseconds timeout, Sock::EventsPerSock& events_per_sock)
        EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);

private:
    bool Register(const std::shared_ptr<const Sock>& sock, bool edge_triggered) EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);
    bool Unregister(const std::shared_ptr<const Sock>& sock, bool edge_triggered) EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);

private:
    /* Instance validity flag set during construction */
    bool m_valid{false};
    /* Flag for storing selected socket events mode */
    SocketEventsMode m_mode;
    /* File descriptor used to interact with events mode */
    int m_fd{-1};

    Mutex m_mutex;
    /**
     * Sockets in the interest list, by the address their events are tagged with (see
     * `Sock::RegisterEvents()`). Events for a socket can still be reported after another thread
     * has removed it; the tag of such an event is only looked up here, never dereferenced.
     */
    std::unordered_map<const void*, std::shared_ptr<const Sock>> m_socks GUARDED_BY(m_mutex);
};

#endif // BITCOIN_UTIL_EDGE_H
