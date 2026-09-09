// Copyright (c) 2019-2025 The Dash Core developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_CHAINLOCK_CLSIG_H
#define BITCOIN_CHAINLOCK_CLSIG_H

#include <chainlock/chainlock.h>

#include <cstdint>
#include <map>
#include <optional>

class CChain;
class CBlockIndex;
class uint256;

namespace Consensus {
struct Params;
} // namespace Consensus

namespace llmq {
class CQuorumManager;
enum class VerifyRecSigStatus : uint8_t;
} // namespace llmq

namespace chainlock {
struct CoinbaseChainLock {
    ChainLockSig clsig;
    const CBlockIndex* carrier{nullptr};
};

/** Reads historical signatures from a fixed, validated chain view.
 * Cache lifetime is one request; missing block data throws instead of implying
 * that a certificate does not exist. Disk reads do not hold cs_main.
 */
class CoinbaseChainLockReader
{
    const CChain& m_chain;
    std::map<int, std::optional<CoinbaseChainLock>> m_cache;

public:
    explicit CoinbaseChainLockReader(const CChain& chain) :
        m_chain(chain)
    {
    }
    std::optional<CoinbaseChainLock> Read(int carrier_height);
    /** First certificate at or above minimum_height, limited by maximum_height. */
    std::optional<CoinbaseChainLock> Find(int minimum_height, int maximum_height);
};

//! Generate clsig request ID with block height
uint256 GenSigRequestId(const int32_t nHeight);

llmq::VerifyRecSigStatus VerifyChainLock(const Consensus::Params& params, const CChain& chain,
                                         const llmq::CQuorumManager& qman, const ChainLockSig& clsig);
llmq::VerifyRecSigStatus VerifyChainLock(const Consensus::Params& params, const llmq::CQuorumManager& qman,
                                         const ChainLockSig& clsig, const CBlockIndex* pindexStart);
} // namespace chainlock

#endif // BITCOIN_CHAINLOCK_CLSIG_H
