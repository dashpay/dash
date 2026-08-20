// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

//! Unit tests for the DAPI client's availability/anti-replay logic that the
//! proof vectors cannot reach: the per-endpoint freshness watermark
//! (transport::FreshnessTracker) and the pin-one-endpoint-per-operation retry
//! driver (transport::RetryAcrossEndpoints). These exercise the exact code
//! paths behind the "honest lagging node" and "mixed-root" regressions
//! without needing live evonodes or fabricated signed proofs.

#include <platform/client.h>
#include <platform/transport/endpoint_retry.h>
#include <platform/transport/freshness.h>
#include <platform/transport/grpcweb.h>

#include <test/util/setup_common.h>

#include <boost/test/unit_test.hpp>

#include <charconv>
#include <iterator>
#include <string>
#include <vector>

using platform::Endpoint;
using platform::transport::AttemptStatus;
using platform::transport::FreshnessTracker;
using platform::transport::ParseGrpcWebResponse;
using platform::transport::RetryAcrossEndpoints;

BOOST_FIXTURE_TEST_SUITE(platform_client_tests, BasicTestingSetup)

namespace {

std::vector<uint8_t> GrpcFrame(uint8_t flags, Span<const uint8_t> data)
{
    const uint32_t size{static_cast<uint32_t>(data.size())};
    std::vector<uint8_t> frame{flags, static_cast<uint8_t>(size >> 24), static_cast<uint8_t>(size >> 16),
                               static_cast<uint8_t>(size >> 8), static_cast<uint8_t>(size)};
    frame.insert(frame.end(), data.begin(), data.end());
    return frame;
}

std::vector<uint8_t> HttpResponse(const std::vector<uint8_t>& body, const std::string& headers = {},
                                  const std::string& status = "200 OK")
{
    const std::string head{"HTTP/1.1 " + status + "\r\n" + headers + "\r\n"};
    std::vector<uint8_t> response(head.begin(), head.end());
    response.insert(response.end(), body.begin(), body.end());
    return response;
}

std::vector<uint8_t> ValidGrpcBody()
{
    const std::vector<uint8_t> message{1, 2, 3};
    const std::string trailers{"grpc-status: 0\r\n"};
    const std::vector<uint8_t> trailer_bytes(trailers.begin(), trailers.end());
    auto body{GrpcFrame(0x00, message)};
    const auto trailer_frame{GrpcFrame(0x80, trailer_bytes)};
    body.insert(body.end(), trailer_frame.begin(), trailer_frame.end());
    return body;
}

} // namespace

BOOST_AUTO_TEST_CASE(grpcweb_valid_response)
{
    const auto result{ParseGrpcWebResponse(HttpResponse(ValidGrpcBody()))};
    BOOST_CHECK(result.transport_ok);
    BOOST_CHECK_EQUAL(result.grpc_status, 0);
    const std::vector<uint8_t> expected{1, 2, 3};
    BOOST_CHECK_EQUAL_COLLECTIONS(result.message.begin(), result.message.end(), expected.begin(), expected.end());
}

BOOST_AUTO_TEST_CASE(grpcweb_valid_chunked_response)
{
    const auto body{ValidGrpcBody()};
    char size_buffer[2 * sizeof(size_t) + 1];
    const auto [size_end, ec]{std::to_chars(std::begin(size_buffer), std::end(size_buffer), body.size(), 16)};
    BOOST_REQUIRE(ec == std::errc{});
    const std::string chunk_header(size_buffer, size_end);
    std::vector<uint8_t> chunked(chunk_header.begin(), chunk_header.end());
    chunked.insert(chunked.end(), {'\r', '\n'});
    chunked.insert(chunked.end(), body.begin(), body.end());
    const std::string final{"\r\n0\r\n\r\n"};
    chunked.insert(chunked.end(), final.begin(), final.end());

    const auto result{ParseGrpcWebResponse(HttpResponse(chunked, "Transfer-Encoding: chunked\r\n"))};
    BOOST_CHECK(result.transport_ok);
    BOOST_CHECK_EQUAL(result.grpc_status, 0);
}

BOOST_AUTO_TEST_CASE(grpcweb_rejects_http_and_missing_status)
{
    BOOST_CHECK(!ParseGrpcWebResponse(HttpResponse({}, {}, "500 Internal Server Error")).transport_ok);
    BOOST_CHECK(!ParseGrpcWebResponse(HttpResponse({})).transport_ok);
    BOOST_CHECK(!ParseGrpcWebResponse(HttpResponse({}, "grpc-status: 0\r\n")).transport_ok);
    const auto grpc_error{ParseGrpcWebResponse(HttpResponse({}, "grpc-status: 3\r\n"))};
    BOOST_CHECK(grpc_error.transport_ok);
    BOOST_CHECK_EQUAL(grpc_error.grpc_status, 3);
    BOOST_CHECK(!ParseGrpcWebResponse(HttpResponse({}, "grpc-status: invalid\r\n")).transport_ok);
}

BOOST_AUTO_TEST_CASE(grpcweb_rejects_malformed_chunk_framing)
{
    const std::string oversized{"ffffffffffffffff\r\nx\r\n0\r\n\r\n"};
    const std::vector<uint8_t> oversized_bytes(oversized.begin(), oversized.end());
    BOOST_CHECK(!ParseGrpcWebResponse(HttpResponse(oversized_bytes, "Transfer-Encoding: chunked\r\n")).transport_ok);

    const std::string unterminated{"1\r\nx0\r\n\r\n"};
    const std::vector<uint8_t> unterminated_bytes(unterminated.begin(), unterminated.end());
    BOOST_CHECK(!ParseGrpcWebResponse(HttpResponse(unterminated_bytes, "Transfer-Encoding: chunked\r\n")).transport_ok);
}

BOOST_AUTO_TEST_CASE(grpcweb_rejects_malformed_frames)
{
    const std::vector<uint8_t> truncated_header{0x00, 0x00};
    BOOST_CHECK(!ParseGrpcWebResponse(HttpResponse(truncated_header, "grpc-status: 0\r\n")).transport_ok);

    const std::vector<uint8_t> truncated_frame{0x00, 0x00, 0x00, 0x00, 0x02, 0x01};
    BOOST_CHECK(!ParseGrpcWebResponse(HttpResponse(truncated_frame, "grpc-status: 0\r\n")).transport_ok);
}

// ---------------------------------------------------------------------------
// FreshnessTracker
// ---------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(freshness_first_response_accepted)
{
    FreshnessTracker ft;
    std::string err;
    // No local ChainLock and no history: the first response from any endpoint
    // is accepted (freshness is a bound, not a hard gate on cold start).
    BOOST_CHECK(ft.Accept("nodeA", /*height=*/1000, /*cclh=*/500, err));
    BOOST_CHECK(err.empty());
}

BOOST_AUTO_TEST_CASE(freshness_per_endpoint_lag_is_tolerated)
{
    // The regression: a global watermark rejected an honest node lagging the
    // fastest by even one block. Per-endpoint tracking must accept a second
    // node that is a few blocks behind a first node.
    FreshnessTracker ft;
    std::string err;
    BOOST_CHECK(ft.Accept("fast", 2000, 900, err));
    BOOST_CHECK_MESSAGE(ft.Accept("slow", 1998, 900, err), err); // 2 blocks behind, different node
    BOOST_CHECK_MESSAGE(ft.Accept("slow", 1999, 900, err), err); // slow advances, still fine
}

BOOST_AUTO_TEST_CASE(freshness_same_endpoint_rollback_rejected)
{
    // A single node must not roll its own reported platform height backwards
    // (that is the replay we do want to catch).
    FreshnessTracker ft;
    std::string err;
    BOOST_CHECK(ft.Accept("nodeA", 2000, 900, err));
    BOOST_CHECK(ft.Accept("nodeA", 2000, 900, err)); // equal height ok (same block)
    BOOST_CHECK(ft.Accept("nodeA", 2001, 900, err)); // forward ok
    err.clear();
    BOOST_CHECK(!ft.Accept("nodeA", 2000, 900, err)); // backwards from 2001 -> reject
    BOOST_CHECK(!err.empty());
}

BOOST_AUTO_TEST_CASE(freshness_core_chainlock_floor)
{
    FreshnessTracker ft;
    ft.SetLocalChainLockHeight(1'000'000);
    std::string err;
    // Within the lag window: accepted.
    BOOST_CHECK_MESSAGE(
        ft.Accept("nodeA", 5000, 1'000'000 - FreshnessTracker::MAX_CORE_CHAINLOCK_LAG, err), err);
    // Beyond the lag window (one block too old): rejected as stale.
    err.clear();
    BOOST_CHECK(
        !ft.Accept("nodeB", 5000, 1'000'000 - FreshnessTracker::MAX_CORE_CHAINLOCK_LAG - 1, err));
    BOOST_CHECK(!err.empty());
}

BOOST_AUTO_TEST_CASE(freshness_floor_monotonic_and_disabled_when_unknown)
{
    FreshnessTracker ft;
    std::string err;
    // Unknown local ChainLock (0): the floor is disabled, even ancient proofs
    // pass the floor (the per-endpoint watermark still applies).
    BOOST_CHECK(ft.Accept("nodeA", 10, 1, err));

    ft.SetLocalChainLockHeight(1'000'000);
    ft.SetLocalChainLockHeight(999'999); // must not lower the floor
    BOOST_CHECK_EQUAL(ft.LocalChainLockHeight(), 1'000'000);
}

// ---------------------------------------------------------------------------
// RetryAcrossEndpoints
// ---------------------------------------------------------------------------

namespace {
std::vector<Endpoint> MakeEndpoints(size_t n)
{
    std::vector<Endpoint> eps(n);
    for (size_t i = 0; i < n; ++i) {
        // Distinct proTxHashes so the recorded order is identifiable.
        for (size_t b = 0; b < 32; ++b) eps[i].pro_tx_hash.begin()[b] = 0;
        eps[i].pro_tx_hash.begin()[0] = static_cast<uint8_t>(i + 1);
    }
    return eps;
}
uint8_t Tag(const Endpoint& ep) { return ep.pro_tx_hash.begin()[0]; }
} // namespace

BOOST_AUTO_TEST_CASE(retry_empty_endpoints_makes_no_attempt)
{
    const std::vector<Endpoint> none;
    size_t calls{0};
    const size_t made = RetryAcrossEndpoints(none, 0, 4, [&](const Endpoint&, size_t) {
        ++calls;
        return AttemptStatus::Success;
    });
    BOOST_CHECK_EQUAL(made, 0U);
    BOOST_CHECK_EQUAL(calls, 0U);
}

BOOST_AUTO_TEST_CASE(retry_stops_on_first_success)
{
    const auto eps = MakeEndpoints(4);
    std::vector<uint8_t> visited;
    const size_t made = RetryAcrossEndpoints(eps, /*start=*/1, 4, [&](const Endpoint& ep, size_t) {
        visited.push_back(Tag(ep));
        return AttemptStatus::Success;
    });
    BOOST_CHECK_EQUAL(made, 1U);
    BOOST_REQUIRE_EQUAL(visited.size(), 1U);
    BOOST_CHECK_EQUAL(visited[0], 2); // start index 1 -> endpoint tag 2
}

BOOST_AUTO_TEST_CASE(retry_rotates_round_robin_until_success)
{
    const auto eps = MakeEndpoints(4);
    std::vector<uint8_t> visited;
    // Fail the first two attempts, succeed on the third.
    const size_t made = RetryAcrossEndpoints(eps, /*start=*/3, 4, [&](const Endpoint& ep, size_t i) {
        visited.push_back(Tag(ep));
        return i < 2 ? AttemptStatus::Retry : AttemptStatus::Success;
    });
    BOOST_CHECK_EQUAL(made, 3U);
    // start=3 -> tags 4, 1, 2 (round robin, distinct endpoints).
    const std::vector<uint8_t> expected{4, 1, 2};
    BOOST_CHECK_EQUAL_COLLECTIONS(visited.begin(), visited.end(), expected.begin(), expected.end());
}

BOOST_AUTO_TEST_CASE(retry_bounded_by_max_and_endpoint_count)
{
    const auto eps = MakeEndpoints(3);
    // All attempts fail; max_attempts (5) exceeds endpoint count (3), so only
    // 3 distinct endpoints are tried and each exactly once.
    std::vector<uint8_t> visited;
    const size_t made = RetryAcrossEndpoints(eps, 0, 5, [&](const Endpoint& ep, size_t) {
        visited.push_back(Tag(ep));
        return AttemptStatus::Retry;
    });
    BOOST_CHECK_EQUAL(made, 3U);
    const std::vector<uint8_t> expected{1, 2, 3};
    BOOST_CHECK_EQUAL_COLLECTIONS(visited.begin(), visited.end(), expected.begin(), expected.end());

    // A tighter max caps attempts below the endpoint count.
    visited.clear();
    const size_t capped = RetryAcrossEndpoints(eps, 0, 2, [&](const Endpoint& ep, size_t) {
        visited.push_back(Tag(ep));
        return AttemptStatus::Retry;
    });
    BOOST_CHECK_EQUAL(capped, 2U);
    BOOST_CHECK_EQUAL(visited.size(), 2U);
}

BOOST_AUTO_TEST_CASE(retry_single_endpoint_one_attempt)
{
    const auto eps = MakeEndpoints(1);
    size_t calls{0};
    const size_t made = RetryAcrossEndpoints(eps, 0, 4, [&](const Endpoint&, size_t) {
        ++calls;
        return AttemptStatus::Retry;
    });
    BOOST_CHECK_EQUAL(made, 1U);
    BOOST_CHECK_EQUAL(calls, 1U);
}

// A logical operation that makes several sub-requests within one attempt must
// target a single endpoint (the invariant that keeps multi-proof getIdentity
// roots consistent). Model the three identity sub-queries and assert all three
// hit the same pinned endpoint per attempt.
BOOST_AUTO_TEST_CASE(retry_pins_single_endpoint_per_attempt)
{
    const auto eps = MakeEndpoints(3);
    std::vector<uint8_t> per_attempt_endpoints;
    RetryAcrossEndpoints(eps, /*start=*/0, 4, [&](const Endpoint& ep, size_t i) {
        // Simulate 3 sub-requests; every one uses the pinned `ep`.
        uint8_t first = Tag(ep);
        for (int sub = 0; sub < 3; ++sub) BOOST_CHECK_EQUAL(Tag(ep), first);
        per_attempt_endpoints.push_back(first);
        return i < 1 ? AttemptStatus::Retry : AttemptStatus::Success; // fail once, then succeed
    });
    // First attempt on endpoint 1, retry lands on endpoint 2 (never mixes).
    const std::vector<uint8_t> expected{1, 2};
    BOOST_CHECK_EQUAL_COLLECTIONS(per_attempt_endpoints.begin(), per_attempt_endpoints.end(),
                                  expected.begin(), expected.end());
}

BOOST_AUTO_TEST_SUITE_END()
