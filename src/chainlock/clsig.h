// Copyright (c) 2019-2025 The Dash Core developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_CHAINLOCK_CLSIG_H
#define BITCOIN_CHAINLOCK_CLSIG_H

#include <cstdint>

class CChain;
class uint256;

namespace Consensus {
struct Params;
} // namespace Consensus

namespace llmq {
class CQuorumManager;
enum class VerifyRecSigStatus : uint8_t;
} // namespace llmq

namespace chainlock {
struct ChainLockSig;

//! Generate clsig request ID with block height
uint256 GenSigRequestId(const int32_t nHeight);

llmq::VerifyRecSigStatus VerifyChainLock(const Consensus::Params& params, const CChain& chain,
                                         const llmq::CQuorumManager& qman, const ChainLockSig& clsig);
} // namespace chainlock

#endif // BITCOIN_CHAINLOCK_CLSIG_H
