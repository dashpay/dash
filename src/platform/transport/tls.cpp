// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <platform/transport/tls.h>

#include <netbase.h>

#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/error.h>
#include <mbedtls/net_sockets.h>
#include <mbedtls/ssl.h>

#include <util/sock.h>

#include <algorithm>
#include <chrono>
#include <climits>
#include <cstring>

namespace platform::transport {

struct TlsConnection::Impl {
    mbedtls_ssl_context ssl;
    mbedtls_ssl_config conf;
    mbedtls_ctr_drbg_context ctr_drbg;
    mbedtls_entropy_context entropy;
    std::unique_ptr<Sock> sock;
    std::chrono::steady_clock::time_point deadline;
    std::function<bool()> interrupted;
    bool ssl_ready{false};

    Impl()
    {
        mbedtls_ssl_init(&ssl);
        mbedtls_ssl_config_init(&conf);
        mbedtls_ctr_drbg_init(&ctr_drbg);
        mbedtls_entropy_init(&entropy);
    }
    ~Impl()
    {
        if (ssl_ready) mbedtls_ssl_close_notify(&ssl);
        mbedtls_ssl_free(&ssl);
        mbedtls_ssl_config_free(&conf);
        mbedtls_ctr_drbg_free(&ctr_drbg);
        mbedtls_entropy_free(&entropy);
    }

    static bool IsTemporarySocketError(int error)
    {
        return error == WSAEAGAIN || error == WSAEINTR || error == WSAEWOULDBLOCK || error == WSAEINPROGRESS;
    }

    static int Send(void* context, const unsigned char* data, size_t size)
    {
        const Sock& sock{*static_cast<Sock*>(context)};
        const ssize_t ret{sock.Send(data, std::min(size, static_cast<size_t>(INT_MAX)), MSG_NOSIGNAL)};
        if (ret >= 0) return static_cast<int>(ret);
        return IsTemporarySocketError(WSAGetLastError()) ? MBEDTLS_ERR_SSL_WANT_WRITE : MBEDTLS_ERR_NET_SEND_FAILED;
    }

    static int Recv(void* context, unsigned char* data, size_t size)
    {
        const Sock& sock{*static_cast<Sock*>(context)};
        const ssize_t ret{sock.Recv(data, std::min(size, static_cast<size_t>(INT_MAX)), 0)};
        if (ret >= 0) return static_cast<int>(ret);
        return IsTemporarySocketError(WSAGetLastError()) ? MBEDTLS_ERR_SSL_WANT_READ : MBEDTLS_ERR_NET_RECV_FAILED;
    }

    bool Wait(Sock::Event requested, std::string& error) const
    {
        while (true) {
            if (!CheckDeadline(error)) return false;
            const auto now{std::chrono::steady_clock::now()};
            const auto remaining{std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now)};
            const auto wait_time{std::min(remaining, std::chrono::milliseconds{100})};
            Sock::Event occurred{0};
#ifdef USE_POLL
            constexpr SocketEventsMode FALLBACK_MODE{SocketEventsMode::Poll};
#else
            constexpr SocketEventsMode FALLBACK_MODE{SocketEventsMode::Select};
#endif
            const SocketEventsMode mode{g_socket_events_mode == SocketEventsMode::Unknown ? FALLBACK_MODE
                                                                                          : g_socket_events_mode};
            if (!sock->Wait(wait_time, requested, SocketEventsParams{mode}, &occurred)) {
                error = "socket wait failed";
                return false;
            }
            if (occurred & requested) return true;
        }
    }

    bool WaitForTls(int ret, std::string& error) const
    {
        return Wait(ret == MBEDTLS_ERR_SSL_WANT_WRITE ? Sock::SEND : Sock::RECV, error);
    }

    bool CheckDeadline(std::string& error) const
    {
        if (interrupted && interrupted()) {
            error = "Platform request interrupted";
            return false;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            error = "Platform request timed out";
            return false;
        }
        return true;
    }
};

TlsConnection::TlsConnection() : m_impl(std::make_unique<Impl>()) {}
TlsConnection::~TlsConnection() = default;

std::unique_ptr<TlsConnection> TlsConnection::Connect(const std::string& host, uint16_t port, int timeout_ms,
                                                      std::string& error, const std::function<bool()>& interrupted)
{
    auto conn = std::unique_ptr<TlsConnection>(new TlsConnection());
    Impl& s = *conn->m_impl;
    s.deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds{std::max(timeout_ms, 1)};
    s.interrupted = interrupted;

    const char* pers = "dash-platform-gui";
    if (mbedtls_ctr_drbg_seed(&s.ctr_drbg, mbedtls_entropy_func, &s.entropy,
                              reinterpret_cast<const unsigned char*>(pers), std::strlen(pers)) != 0) {
        error = "ctr_drbg seed failed";
        return nullptr;
    }

    const auto service{Lookup(host, port, /*fAllowLookup=*/false)};
    if (!service) {
        error = "invalid Platform endpoint " + host;
        return nullptr;
    }
    s.sock = CreateSock(service->GetSAFamily());
    if (!s.sock) {
        error = "unable to create Platform socket";
        return nullptr;
    }
    sockaddr_storage addr;
    socklen_t addr_len{sizeof(addr)};
    if (!service->GetSockAddr(reinterpret_cast<sockaddr*>(&addr), &addr_len)) {
        error = "unsupported Platform endpoint";
        return nullptr;
    }
    if (s.sock->Connect(reinterpret_cast<sockaddr*>(&addr), addr_len) == SOCKET_ERROR) {
        const int connect_error{WSAGetLastError()};
        if ((!Impl::IsTemporarySocketError(connect_error) && connect_error != WSAEINVAL) ||
            !s.Wait(Sock::RECV | Sock::SEND, error)) {
            if (error.empty()) error = "tcp connection failed";
            return nullptr;
        }
        int socket_error{0};
        socklen_t error_len{sizeof(socket_error)};
        if (s.sock->GetSockOpt(SOL_SOCKET, SO_ERROR, &socket_error, &error_len) == SOCKET_ERROR || socket_error != 0) {
            error = "tcp connection failed";
            return nullptr;
        }
    }

    if (mbedtls_ssl_config_defaults(&s.conf, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM,
                                    MBEDTLS_SSL_PRESET_DEFAULT) != 0) {
        error = "ssl config defaults failed";
        return nullptr;
    }
    // See the class comment: transport certs are not chain-verified; response
    // integrity is guaranteed by proof + quorum-signature verification.
    mbedtls_ssl_conf_authmode(&s.conf, MBEDTLS_SSL_VERIFY_NONE);
    mbedtls_ssl_conf_rng(&s.conf, mbedtls_ctr_drbg_random, &s.ctr_drbg);

    if (mbedtls_ssl_setup(&s.ssl, &s.conf) != 0) {
        error = "ssl setup failed";
        return nullptr;
    }
    if (mbedtls_ssl_set_hostname(&s.ssl, host.c_str()) != 0) {
        error = "unable to configure TLS server name";
        return nullptr;
    }
    mbedtls_ssl_set_bio(&s.ssl, s.sock.get(), Impl::Send, Impl::Recv, nullptr);
    s.ssl_ready = true;

    int ret;
    while ((ret = mbedtls_ssl_handshake(&s.ssl)) != 0) {
        if (!s.CheckDeadline(error)) return nullptr;
        if (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE) {
            if (!s.WaitForTls(ret, error)) return nullptr;
        } else {
            char buf[128];
            mbedtls_strerror(ret, buf, sizeof(buf));
            error = std::string("tls handshake failed: ") + buf;
            return nullptr;
        }
    }
    return conn;
}

bool TlsConnection::WriteAll(Span<const uint8_t> data, std::string& error)
{
    size_t written = 0;
    while (written < data.size()) {
        if (!m_impl->CheckDeadline(error)) return false;
        int ret = mbedtls_ssl_write(&m_impl->ssl, data.data() + written, data.size() - written);
        if (ret > 0) {
            written += static_cast<size_t>(ret);
            continue;
        }
        if (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE) {
            if (!m_impl->WaitForTls(ret, error)) return false;
            continue;
        }
        char buf[128];
        mbedtls_strerror(ret, buf, sizeof(buf));
        error = std::string("tls write failed: ") + buf;
        return false;
    }
    return true;
}

int TlsConnection::Read(Span<uint8_t> buf, std::string& error)
{
    for (;;) {
        if (!m_impl->CheckDeadline(error)) return -1;
        int ret = mbedtls_ssl_read(&m_impl->ssl, buf.data(), buf.size());
        if (ret >= 0) return ret;
        if (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE) {
            if (!m_impl->WaitForTls(ret, error)) return -1;
            continue;
        }
        if (ret == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) return 0;
        char b[128];
        mbedtls_strerror(ret, b, sizeof(b));
        error = std::string("tls read failed: ") + b;
        return -1;
    }
}

} // namespace platform::transport
