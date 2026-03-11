// Copyright (c) 2015-2022 The Bitcoin Core developers
// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_REST_SERVER_H
#define BITCOIN_REST_SERVER_H

#include <context.h>

#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>

enum class RESTResponseFormat {
    UNDEF,
    BINARY,
    HEX,
    JSON,
};

namespace rest {
/** Default value for SO_REUSEPORT flag use. */
static constexpr bool DEFAULT_REUSE_PORT{false};
/** Default string of address to bind to. */
static constexpr const char* DEFAULT_ADDR{"127.0.0.1"};
/** Default number of I/O threads (matches DEFAULT_HTTP_THREADS). */
static constexpr size_t DEFAULT_THREADS{4};
/** Default number of maximum concurrent connections. */
static constexpr size_t DEFAULT_MAX_CONNECTIONS{100};
/** Default number of seconds before an idle connection times out (matches DEFAULT_HTTP_SERVER_TIMEOUT). */
static constexpr size_t DEFAULT_IDLE_TIMEOUT{30};
/** Minimum allowed idle connection timeout in seconds. */
static constexpr size_t MIN_IDLE_TIMEOUT_SECS{5};
/** Maximum allowed idle connection timeout in seconds. */
static constexpr size_t MAX_IDLE_TIMEOUT_SECS{60*60};
/** Minimum allowed value for maximum concurrent connections. */
static constexpr size_t MIN_MAXCONNECTIONS{1};
/** Maximum allowed value for maximum concurrent connections. */
static constexpr size_t MAX_MAXCONNECTIONS{std::numeric_limits<uint16_t>::max()};

struct Options {
    /** Flag to allow multiple sockets to bind to the same port. */
    bool m_reuse_port{DEFAULT_REUSE_PORT};
    /** Number of I/O threads for the REST server. */
    size_t m_threads{DEFAULT_THREADS};
    /** Maximum number of concurrent connections the REST server will accept. */
    size_t m_max_connections{DEFAULT_MAX_CONNECTIONS};
    /** Seconds of inactivity before an idle connection is closed. */
    size_t m_idle_timeout{DEFAULT_IDLE_TIMEOUT};
    /** IP address to bind the REST server to. */
    std::string m_addr{DEFAULT_ADDR};
    /** Port number on which the REST server listens. */
    uint16_t m_port{0};
};

/** Start HTTP REST subsystem. */
bool StartServer(const CoreContext& context, const Options& opts);

/** Interrupt HTTP REST subsystem. */
void InterruptServer();

/** Stop HTTP REST subsystem. */
void StopServer();
} // namespace rest

/**
 * Parse a URI to get the data format and URI without data format
 * and query string.
 *
 * @param[out]  param   The strReq without the data format string and
 *                      without the query string (if any).
 * @param[in]   strReq  The URI to be parsed.
 * @return      RESTResponseFormat that was parsed from the URI.
 */
RESTResponseFormat ParseDataFormat(std::string& param, const std::string& strReq);

#endif // BITCOIN_REST_SERVER_H
