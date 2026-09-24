// Copyright (c) 2019-2025 The Dash Core developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_CHAINLOCK_CLSIG_H
#define BITCOIN_CHAINLOCK_CLSIG_H

#include <bls/bls.h>
#include <chainlock/chainlock.h>
#include <uint256.h>

#include <array>
#include <cstdint>
#include <map>
#include <optional>

class CChain;
class CBlockIndex;

namespace Consensus {
struct Params;
} // namespace Consensus

namespace llmq {
class CQuorumManager;
enum class VerifyRecSigStatus : uint8_t;
} // namespace llmq

namespace chainlock {
/** A ChainLock certificate carried by a historical coinbase. The signature is
 *  kept as bytes: decoding a BLS point dominates the cost of reading a carrier,
 *  and binary search only needs heights. Signed() decodes it (and throws if the
 *  bytes are not a valid signature). */
struct CoinbaseChainLock {
    int32_t height{-1};
    uint256 block_hash;
    const CBlockIndex* carrier{nullptr};
    std::array<uint8_t, CBLSSignature::SerSize> signature_bytes{};
    ChainLockSig Signed() const;
};

/** Reads historical signatures from a fixed, validated chain view.
 * What each carrier block says is memoized process-wide by block hash, so a
 * carrier read once stays available even if its block data is later removed;
 * data that was never read and is missing throws instead of implying that a
 * certificate does not exist. Disk reads do not hold cs_main.
 */
class CoinbaseChainLockReader
{
    const CBlockIndex* m_tip;
    std::map<int, std::optional<CoinbaseChainLock>> m_cache;

public:
    explicit CoinbaseChainLockReader(const CBlockIndex* tip) : m_tip(tip) {}
    std::optional<CoinbaseChainLock> Read(int carrier_height);
    /** First certificate at or above minimum_height, limited by maximum_height. */
    std::optional<CoinbaseChainLock> Find(int minimum_height, int maximum_height);
};

/** Coinbase ChainLocks are memoized process-wide by carrier block hash, so a
 *  carrier read once stays available even if its block data is later removed. */
void ClearCoinbaseChainLockCacheForTesting();

//! Generate clsig request ID with block height
uint256 GenSigRequestId(const int32_t nHeight);

llmq::VerifyRecSigStatus VerifyChainLock(const Consensus::Params& params, const CChain& chain,
                                         const llmq::CQuorumManager& qman, const ChainLockSig& clsig);
llmq::VerifyRecSigStatus VerifyChainLock(const Consensus::Params& params, const llmq::CQuorumManager& qman,
                                         const ChainLockSig& clsig, const CBlockIndex* pindexStart);
} // namespace chainlock

#endif // BITCOIN_CHAINLOCK_CLSIG_H
