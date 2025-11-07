// Copyright (c) 2019-2026 The Dash Core developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_CHAINLOCK_CHAINLOCK_H
#define BITCOIN_CHAINLOCK_CHAINLOCK_H

#include <bls/bls.h>
#include <gsl/pointers.h>
#include <serialize.h>
#include <sync.h>
#include <uint256.h>

#include <deque>
#include <vector>

class CBlockIndex;
class CSporkManager;
class uint256;

namespace chainlock {

//! Depth of block including transactions before it's considered safe
static constexpr int32_t TX_CONFIRM_THRESHOLD{5};

struct ChainLockSig {
private:
    int32_t nHeight{-1};
    uint256 blockHash;
    CBLSSignature sig;

public:
    ChainLockSig();
    ~ChainLockSig();

    ChainLockSig(int32_t nHeight, const uint256& blockHash, const CBLSSignature& sig);

    [[nodiscard]] int32_t getHeight() const { return nHeight; }
    [[nodiscard]] const uint256& getBlockHash() const { return blockHash; }
    [[nodiscard]] const CBLSSignature& getSig() const { return sig; }
    [[nodiscard]] bool IsNull() const { return nHeight == -1 && blockHash == uint256(); }
    [[nodiscard]] std::string ToString() const;

    SERIALIZE_METHODS(ChainLockSig, obj)
    {
        READWRITE(obj.nHeight, obj.blockHash, obj.sig);
    }
};

class Chainlocks
{
private:
    const CSporkManager& m_sporks;

    mutable Mutex cs;
    const CBlockIndex* bestChainLockBlockIndex GUARDED_BY(cs){nullptr};

    uint256 bestChainLockHash GUARDED_BY(cs);
    chainlock::ChainLockSig bestChainLock GUARDED_BY(cs);

    chainlock::ChainLockSig bestChainLockWithKnownBlock GUARDED_BY(cs);

    // Queue for coinbase chainlocks pending asynchronous processing by ChainlockHandler
    std::deque<chainlock::ChainLockSig> pendingCoinbaseChainLocks GUARDED_BY(cs);

public:
    Chainlocks(const CSporkManager& sporkman);

    [[nodiscard]] bool IsEnabled() const;
    [[nodiscard]] bool IsSigningEnabled() const;

    [[nodiscard]] chainlock::ChainLockSig GetBestChainLock() const EXCLUSIVE_LOCKS_REQUIRED(!cs);

    [[nodiscard]] int32_t GetBestChainLockHeight() const EXCLUSIVE_LOCKS_REQUIRED(!cs);

    [[nodiscard]] bool HasChainLock(int nHeight, const uint256& blockHash) const EXCLUSIVE_LOCKS_REQUIRED(!cs);

    [[nodiscard]] bool HasConflictingChainLock(int nHeight, const uint256& blockHash) const EXCLUSIVE_LOCKS_REQUIRED(!cs);

    bool UpdateBestChainlock(const uint256& hash, const chainlock::ChainLockSig& clsig, const CBlockIndex* pindex)
        EXCLUSIVE_LOCKS_REQUIRED(!cs);

    std::pair<chainlock::ChainLockSig, const CBlockIndex*> GetBestChainlockWithPindex() const
        EXCLUSIVE_LOCKS_REQUIRED(!cs);

    bool GetChainLockByHash(const uint256& hash, chainlock::ChainLockSig& ret) const EXCLUSIVE_LOCKS_REQUIRED(!cs);

    void AcceptedBlockHeader(gsl::not_null<const CBlockIndex*> pindexNew) EXCLUSIVE_LOCKS_REQUIRED(!cs);

    void ResetChainlock() EXCLUSIVE_LOCKS_REQUIRED(!cs);

    // Queue a coinbase chainlock for asynchronous processing by the ChainlockHandler.
    // Called during block validation to avoid blocking the main validation flow.
    void QueueCoinbaseChainLock(const chainlock::ChainLockSig& clsig) EXCLUSIVE_LOCKS_REQUIRED(!cs);

    // Drain pending coinbase chainlocks for processing by ChainlockHandler.
    std::vector<chainlock::ChainLockSig> DrainPendingCoinbaseChainLocks() EXCLUSIVE_LOCKS_REQUIRED(!cs);
};

} // namespace chainlock

#endif // BITCOIN_CHAINLOCK_CHAINLOCK_H
