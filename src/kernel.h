// Copyright (c) 2012-2013 The PPCoin developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.
#ifndef BITCOIN_KERNEL_H
#define BITCOIN_KERNEL_H
#include "uint256.h"
#include "streams.h"
#include "arith_uint256.h"

class CBlock;
class CWallet;
class COutPoint;
class CBlockIndex;
class CTransaction;
// MODIFIER_INTERVAL: time to elapse before new modifier is computed
static const unsigned int MODIFIER_INTERVAL = 60;
static const unsigned int MODIFIER_INTERVAL_TESTNET = 20;
extern unsigned int nModifierInterval;
extern unsigned int getIntervalVersion(bool fTestNet);
// MODIFIER_INTERVAL_RATIO:
// ratio of group interval length between the last group and the first group
static const int MODIFIER_INTERVAL_RATIO = 3;
// Compute the hash modifier for proof-of-stake
bool ComputeNextStakeModifier(const CBlockIndex* pindexPrev, uint64_t& nStakeModifier, bool& fGeneratedStakeModifier);
// Check whether stake kernel meets hash target
// Sets hashProofOfStake on success return
bool CheckStakeKernelHash(unsigned int nBits, const CBlock& blockFrom, unsigned int nTxPrevOffset,
                          std::shared_ptr<const CTransaction> txPrev, const COutPoint& prevout, unsigned int nTimeTx,
                          uint256& hashProofOfStake, bool fPrintProofOfStake = false);
// Check kernel hash target and coinstake signature
// Sets hashProofOfStake on success return
bool CheckProofOfStake(const CBlock &block, uint256& hashProofOfStake);
// Check whether the coinstake timestamp meets protocol
bool CheckCoinStakeTimestamp(int64_t nTimeBlock, int64_t nTimeTx);
// Get stake modifier checksum
unsigned int GetStakeModifierChecksum(const CBlockIndex* pindex);
// Check stake modifier hard checkpoints
bool CheckStakeModifierCheckpoints(int nHeight, unsigned int nStakeModifierChecksum);
#endif // BITCOIN_KERNEL_H