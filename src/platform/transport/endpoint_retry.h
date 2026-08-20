// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_PLATFORM_TRANSPORT_ENDPOINT_RETRY_H
#define BITCOIN_PLATFORM_TRANSPORT_ENDPOINT_RETRY_H

#include <platform/client.h>

#include <algorithm>
#include <cstddef>
#include <vector>

namespace platform::transport {

enum class AttemptStatus {
    Success, //!< the logical operation completed; stop.
    Retry,   //!< this endpoint could not answer; try the next one.
};

//! Runs a whole logical operation against a *single* pinned endpoint at a
//! time, moving to the next endpoint (round-robin from `start`) only when the
//! attempt returns Retry. Since getIdentity collapsed to a single proved
//! request, no in-tree operation issues multiple sub-queries per attempt any
//! more; DoGetIdentity still uses this helper for its retry-on-verification-
//! failure behaviour, and the pinning again matters the moment an operation
//! spans more than one proof. `attempt(endpoint, attempt_index)` performs the
//! full operation against `endpoint`.
//!
//! Returns the number of attempts made (0 when there are no endpoints). At
//! most min(max_attempts, endpoints.size()) distinct endpoints are tried, so
//! a persistently failing query terminates instead of looping.
template <typename Attempt>
size_t RetryAcrossEndpoints(const std::vector<Endpoint>& endpoints, size_t start,
                            size_t max_attempts, Attempt&& attempt)
{
    if (endpoints.empty() || max_attempts == 0) return 0;
    const size_t limit = std::min(max_attempts, endpoints.size());
    for (size_t i = 0; i < limit; ++i) {
        const Endpoint& endpoint = endpoints[(start + i) % endpoints.size()];
        if (attempt(endpoint, i) == AttemptStatus::Success) return i + 1;
    }
    return limit;
}

} // namespace platform::transport

#endif // BITCOIN_PLATFORM_TRANSPORT_ENDPOINT_RETRY_H
