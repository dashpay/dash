// Copyright (c) 2019-2025 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_INSTANTSEND_LOCK_H
#define BITCOIN_INSTANTSEND_LOCK_H

#include <bls/bls.h>
#include <consensus/consensus.h>
#include <serialize.h>
#include <uint256.h>

#include <cstdint>
#include <memory>
#include <vector>

class COutPoint;
class CTransaction;

namespace instantsend {
struct InstantSendLock {
    static constexpr uint8_t CURRENT_VERSION{1};
    // An islock pins the same outpoints as the locked transaction's inputs, so it can
    // never carry more inputs than a consensus-valid transaction. Such a transaction must
    // fit in a block (MaxBlockSize()) and each transaction input (CTxIn) serializes to at
    // least 41 bytes (COutPoint 36 + scriptSig length 1 + nSequence 4), bounding it at
    // MaxBlockSize() / 41 inputs; the islock stores those inputs as 36-byte COutPoints.
    // Deriving from MaxBlockSize() keeps this cap in lockstep with any future block-size
    // change. The ceiling can never reject a valid islock, but it lets us drop oversized
    // locks before the O(n) hashing/dedup work and bounds each retained pending entry.
    static constexpr size_t MAX_INPUTS{MaxBlockSize() / 41};

    uint8_t nVersion{CURRENT_VERSION};
    std::vector<COutPoint> inputs;
    uint256 txid;
    uint256 cycleHash;
    CBLSLazySignature sig;

    InstantSendLock() = default;

    SERIALIZE_METHODS(InstantSendLock, obj)
    {
        READWRITE(obj.nVersion);
        READWRITE(obj.inputs);
        READWRITE(obj.txid);
        READWRITE(obj.cycleHash);
        READWRITE(obj.sig);
    }

    uint256 GetRequestId() const;
    bool TriviallyValid() const;
};

uint256 GenInputLockRequestId(const COutPoint& outpoint);

/** The outpoints an InstantSend lock on this transaction pins. These are the transaction's
 *  prevouts, except for asset unlock transactions, which have no inputs: an unlock's lock pins
 *  the synthetic outpoint {DIP-27 signing request id of its withdrawal index, 0}. Every
 *  instance of one withdrawal, whatever its version or txid, maps to that same outpoint, so a
 *  lock binds the withdrawal index to one txid and any other transaction claiming the index
 *  conflicts with it through the ordinary outpoint conflict handling. */
std::vector<COutPoint> GetLockInputs(const CTransaction& tx);

/** Whether GetLockInputs() is non-empty: the transaction spends outputs or is an asset unlock.
 *  Coinbase, quorum commitment and other input-less special transactions cannot be locked or
 *  conflict with a lock. */
bool HasLockInputs(const CTransaction& tx);

using InstantSendLockPtr = std::shared_ptr<InstantSendLock>;
} // namespace instantsend

#endif // BITCOIN_INSTANTSEND_LOCK_H
