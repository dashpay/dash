// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_PLATFORM_TRANSPORT_FRESHNESS_H
#define BITCOIN_PLATFORM_TRANSPORT_FRESHNESS_H

#include <tinyformat.h>

#include <cstdint>
#include <map>
#include <string>

namespace platform::transport {

//! Bounds how stale a validly signed platform proof may be before the client
//! rejects it. TLS to evonodes is unauthenticated by design (integrity comes
//! from proof + quorum-signature verification), so without a freshness bound
//! an on-path attacker could replay an old but validly signed response.
//!
//! This is a best-effort staleness bound, not an airtight anti-rollback:
//!  - The core-ChainLock floor only rejects proofs whose signed
//!    core-chain-locked height trails the node's own best ChainLock by more
//!    than MAX_CORE_CHAINLOCK_LAG core blocks, so a replay up to that window
//!    old is still accepted. On a freshly started client (no per-endpoint
//!    history yet) this floor is the only guard.
//!  - The per-endpoint height watermark is monotonic *per endpoint*: it stops
//!    a single node from rolling its own reported platform height backwards
//!    (a replay from that node), while tolerating honest nodes that are a few
//!    blocks apart. It is deliberately not a single global maximum, because a
//!    global watermark rejects any endpoint lagging the fastest one by even a
//!    block — normal propagation delay — turning availability into a failure.
//!
//! The tracker holds no locks; callers that share it across threads must
//! serialize access themselves.
class FreshnessTracker
{
public:
    //! Coarse staleness bound (in core blocks) between a proof's signed
    //! core-chain-locked height and the node's own best ChainLock. ~288
    //! blocks is roughly half a day at 2.5 min/block; generous so normal
    //! platform lag never trips it.
    static constexpr int64_t MAX_CORE_CHAINLOCK_LAG{288};

    //! Update the node's best locally verified core ChainLock height (the
    //! anchor for the staleness floor). Monotonic; 0 means unknown.
    void SetLocalChainLockHeight(int32_t height)
    {
        if (height > m_local_core_chainlocked_height) m_local_core_chainlocked_height = height;
    }

    int32_t LocalChainLockHeight() const { return m_local_core_chainlocked_height; }

    //! Returns true if a proof from `endpoint_key` at the given signed
    //! platform height / core-chain-locked height is fresh enough to accept,
    //! and advances that endpoint's watermark. On rejection returns false and
    //! sets `err`. `endpoint_key` identifies the answering evonode (e.g. its
    //! proTxHash); distinct keys never interfere so honest cross-node lag is
    //! tolerated.
    bool Accept(const std::string& endpoint_key, uint64_t platform_height,
                uint32_t signed_core_chainlocked_height, std::string& err)
    {
        // Absolute staleness floor against local chain state.
        if (m_local_core_chainlocked_height > 0 &&
            static_cast<int64_t>(signed_core_chainlocked_height) + MAX_CORE_CHAINLOCK_LAG <
                static_cast<int64_t>(m_local_core_chainlocked_height)) {
            err = strprintf(
                "stale platform proof: signed core chainlock height %u trails the local ChainLock "
                "height %d by more than %d blocks",
                signed_core_chainlocked_height, m_local_core_chainlocked_height,
                MAX_CORE_CHAINLOCK_LAG);
            return false;
        }
        // Per-endpoint monotonic platform height: a node must not roll its own
        // reported height backwards.
        auto it = m_endpoint_height.find(endpoint_key);
        if (it != m_endpoint_height.end() && platform_height < it->second) {
            err = strprintf(
                "stale platform proof from endpoint %s: signed platform height %llu is below the "
                "highest height %llu already verified from that endpoint (rollback/replay)",
                endpoint_key, static_cast<unsigned long long>(platform_height),
                static_cast<unsigned long long>(it->second));
            return false;
        }
        if (it == m_endpoint_height.end()) {
            m_endpoint_height.emplace(endpoint_key, platform_height);
        } else if (platform_height > it->second) {
            it->second = platform_height;
        }
        return true;
    }

private:
    std::map<std::string, uint64_t> m_endpoint_height;
    int32_t m_local_core_chainlocked_height{0};
};

} // namespace platform::transport

#endif // BITCOIN_PLATFORM_TRANSPORT_FRESHNESS_H
