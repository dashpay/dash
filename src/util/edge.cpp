// Copyright (c) 2020-2024 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <util/edge.h>

#include <logging.h>

#ifdef USE_EPOLL
#include <sys/epoll.h>
#endif

#ifdef USE_KQUEUE
#include <sys/event.h>
#endif

#include <array>
#include <cassert>
#include <utility>

static constexpr size_t MAX_EVENTS = 64;

EdgeTriggeredEvents::EdgeTriggeredEvents(SocketEventsMode events_mode)
    : m_mode(events_mode)
{
    if (m_mode == SocketEventsMode::EPoll) {
#ifdef USE_EPOLL
        m_fd = epoll_create1(0);
        if (m_fd == -1) {
            LogPrintf("Unable to initialize EdgeTriggeredEvents, epoll_create1 returned -1 with error %s\n",
                      NetworkErrorString(WSAGetLastError()));
            return;
        }
#else
        LogPrintf("Attempting to initialize EdgeTriggeredEvents for epoll without support compiled in!\n");
        return;
#endif /* USE_EPOLL */
    } else if (m_mode == SocketEventsMode::KQueue) {
#ifdef USE_KQUEUE
        m_fd = kqueue();
        if (m_fd == -1) {
            LogPrintf("Unable to initialize EdgeTriggeredEvents, kqueue returned -1 with error %s\n",
                      NetworkErrorString(WSAGetLastError()));
            return;
        }
#else
        LogPrintf("Attempting to initialize EdgeTriggeredEvents for kqueue without support compiled in!\n");
        return;
#endif /* USE_KQUEUE */
    } else {
        assert(false);
    }
    m_valid = true;
}

EdgeTriggeredEvents::~EdgeTriggeredEvents()
{
    if (m_valid) {
#if defined(USE_KQUEUE) || defined(USE_EPOLL)
        if (close(m_fd) != 0) {
            LogPrintf("Destroying EdgeTriggeredEvents instance, close() failed for m_fd = %d with error %s\n", m_fd,
                      NetworkErrorString(WSAGetLastError()));
        }
#else
        assert(false);
#endif /* defined(USE_KQUEUE) || defined(USE_EPOLL) */
    }
}

bool EdgeTriggeredEvents::Register(const std::shared_ptr<const Sock>& sock, bool edge_triggered)
{
    assert(m_valid);

    // The socket must be known before an event tagged with it can be reported.
    WITH_LOCK(m_mutex, m_socks.emplace(sock.get(), sock));
    if (!sock->RegisterEvents(m_mode, m_fd, edge_triggered)) {
        LogPrintf("Failed to add socket to %s fd %d (error %s)\n", SEMToString(m_mode), m_fd,
                  NetworkErrorString(WSAGetLastError()));
        WITH_LOCK(m_mutex, m_socks.erase(sock.get()));
        return false;
    }
    return true;
}

bool EdgeTriggeredEvents::Unregister(const std::shared_ptr<const Sock>& sock, bool edge_triggered)
{
    assert(m_valid);

    const bool ret{sock->UnregisterEvents(m_mode, m_fd, edge_triggered)};
    if (!ret) {
        LogPrintf("Failed to remove socket from %s fd %d (error %s)\n", SEMToString(m_mode), m_fd,
                  NetworkErrorString(WSAGetLastError()));
    }
    WITH_LOCK(m_mutex, m_socks.erase(sock.get()));
    return ret;
}

bool EdgeTriggeredEvents::AddSocket(const std::shared_ptr<const Sock>& sock)
{
    return Register(sock, /*edge_triggered=*/false);
}

bool EdgeTriggeredEvents::RemoveSocket(const std::shared_ptr<const Sock>& sock)
{
    return Unregister(sock, /*edge_triggered=*/false);
}

bool EdgeTriggeredEvents::RegisterEvents(const std::shared_ptr<const Sock>& sock)
{
    return Register(sock, /*edge_triggered=*/true);
}

bool EdgeTriggeredEvents::UnregisterEvents(const std::shared_ptr<const Sock>& sock)
{
    return Unregister(sock, /*edge_triggered=*/true);
}

bool EdgeTriggeredEvents::Wait(std::chrono::milliseconds timeout, Sock::EventsPerSock& events_per_sock)
{
    assert(m_valid);

    std::array<std::pair<const void*, Sock::Event>, MAX_EVENTS> occurred_per_tag;
    int ret{SOCKET_ERROR};

    if (m_mode == SocketEventsMode::EPoll) {
#ifdef USE_EPOLL
        std::array<epoll_event, MAX_EVENTS> events{};

        ret = epoll_wait(m_fd, events.data(), events.size(), count_milliseconds(timeout));
        for (int idx = 0; idx < ret; ++idx) {
            const auto& ev = events[idx];
            Sock::Event occurred = 0;
            if (ev.events & (EPOLLERR | EPOLLHUP)) {
                occurred |= Sock::ERR;
            } else {
                if (ev.events & EPOLLIN) {
                    occurred |= Sock::RECV;
                }
                if (ev.events & EPOLLOUT) {
                    occurred |= Sock::SEND;
                }
            }
            occurred_per_tag[idx] = {ev.data.ptr, occurred};
        }
#else
        assert(false);
#endif /* USE_EPOLL */
    } else if (m_mode == SocketEventsMode::KQueue) {
#ifdef USE_KQUEUE
        std::array<struct kevent, MAX_EVENTS> events{};
        struct timespec ts = MillisToTimespec(timeout);

        ret = kevent(m_fd, nullptr, 0, events.data(), events.size(), &ts);
        for (int idx = 0; idx < ret; ++idx) {
            const auto& ev = events[idx];
            Sock::Event occurred = 0;
            if (ev.flags & (EV_ERROR | EV_EOF)) {
                occurred |= Sock::ERR;
            } else {
                if (ev.filter == EVFILT_READ) {
                    occurred |= Sock::RECV;
                }
                if (ev.filter == EVFILT_WRITE) {
                    occurred |= Sock::SEND;
                }
            }
            occurred_per_tag[idx] = {ev.udata, occurred};
        }
#else
        assert(false);
#endif /* USE_KQUEUE */
    } else {
        assert(false);
    }

    events_per_sock.clear();
    if (ret == SOCKET_ERROR) {
        return false;
    }

    LOCK(m_mutex);
    for (int idx = 0; idx < ret; ++idx) {
        const auto& [tag, occurred] = occurred_per_tag[idx];
        const auto it = m_socks.find(tag);
        if (it == m_socks.end()) continue;
        events_per_sock.try_emplace(it->second, Sock::Events{Sock::RECV | Sock::SEND}).first->second.occurred |= occurred;
    }

    return true;
}
