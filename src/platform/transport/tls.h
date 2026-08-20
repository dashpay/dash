// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_PLATFORM_TRANSPORT_TLS_H
#define BITCOIN_PLATFORM_TRANSPORT_TLS_H

#include <cstdint>
#include <functional>
#include <memory>
#include <span.h>
#include <string>
#include <vector>

/**
 * Minimal blocking TLS client over a TCP socket, backed by mbedTLS (3.6 LTS).
 *
 * Transport-level certificate verification is intentionally NOT enforced:
 * evonode DAPI endpoints are reached by bare IP from the deterministic
 * masternode list and their certificates do not chain to a public CA.
 * Integrity comes entirely from the GroveDB proof + quorum signature
 * verification applied to every response, exactly as the reference SDKs
 * operate. If Platform later publishes a cert-pinning scheme this is where it
 * would be enforced.
 */
namespace platform::transport {

class TlsConnection
{
public:
    ~TlsConnection();

    //! Connect to host:port with a timeout. Returns nullptr on failure
    //! (error set).
    static std::unique_ptr<TlsConnection> Connect(const std::string& host, uint16_t port, int timeout_ms,
                                                  std::string& error, const std::function<bool()>& interrupted = {});

    //! Write all bytes. Returns false on error.
    bool WriteAll(Span<const uint8_t> data, std::string& error);
    //! Read up to buf.size() bytes; returns count read (>0), 0 on clean EOF,
    //! -1 on error.
    int Read(Span<uint8_t> buf, std::string& error);

    TlsConnection(const TlsConnection&) = delete;
    TlsConnection& operator=(const TlsConnection&) = delete;

private:
    TlsConnection();
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace platform::transport

#endif // BITCOIN_PLATFORM_TRANSPORT_TLS_H
