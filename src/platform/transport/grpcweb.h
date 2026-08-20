// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_PLATFORM_TRANSPORT_GRPCWEB_H
#define BITCOIN_PLATFORM_TRANSPORT_GRPCWEB_H

#include <cstdint>
#include <functional>
#include <span.h>
#include <string>
#include <vector>

/**
 * A single gRPC-Web unary call over HTTP/1.1 + TLS.
 *
 * DAPI's Envoy gateway exposes the Platform gRPC service as gRPC-Web
 * (application/grpc-web+proto) over HTTP/1.1, which lets a plain TLS client
 * speak it without an HTTP/2 stack. Framing: each message is
 * [1 byte flags][4 byte big-endian length][payload]; the response body is
 * the response message frame followed by a trailers frame (flags bit 0x80)
 * carrying grpc-status / grpc-message.
 * (dashpay/platform packages/dashmate templates gateway/envoy — grpc_web
 * filter.)
 */
namespace platform::transport {

struct GrpcCallResult {
    bool transport_ok{false};   //!< the HTTP/TLS exchange itself succeeded
    int grpc_status{-1};        //!< gRPC status code from trailers (0 = OK)
    std::string grpc_message;   //!< grpc-message trailer (on error)
    std::vector<uint8_t> message; //!< decoded response protobuf (grpc_status==0)
    std::string transport_error;  //!< set when transport_ok is false
};

//! Parse a complete HTTP/1.1 gRPC-Web response. Exposed for focused tests of
//! the untrusted response framing; callers normally use GrpcWebUnary.
GrpcCallResult ParseGrpcWebResponse(Span<const uint8_t> response);

//! Perform a unary gRPC-Web call. `path` is the full gRPC method path, e.g.
//! "/org.dash.platform.dapi.v0.Platform/getIdentity". `request` is the
//! serialized request protobuf. Blocking; intended to run on a worker thread.
GrpcCallResult GrpcWebUnary(const std::string& host, uint16_t port, const std::string& path,
                            const std::vector<uint8_t>& request, int timeout_ms,
                            const std::function<bool()>& interrupted = {});

} // namespace platform::transport

#endif // BITCOIN_PLATFORM_TRANSPORT_GRPCWEB_H
