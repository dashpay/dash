// Copyright (c) 2014-2024 The Dash Core developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_MASTERNODE_PAYMENTS_H
#define BITCOIN_MASTERNODE_PAYMENTS_H

#include <consensus/amount.h>
#include <evo/types.h>
#include <primitives/transaction.h>

#include <vector>

class CBlockIndex;
class CDeterministicMNList;

/**
 * Match the list of expected masternode payment outputs against the coinbase
 * outputs.
 *
 * When @p strict_multiplicity is true, every expected output must be matched
 * by a distinct actual output (multiplicity-correct matching): two identical
 * expected outputs require two identical actual outputs.
 *
 * When false, the legacy behaviour is used where each expected output only
 * has to appear at least once in the actual outputs. This is preserved for
 * pre-v24 historical validation.
 *
 * @return -1 if every expected output is matched, otherwise the index in
 * @p expected of the first output that could not be matched.
 */
int FindUnmatchedMasternodePayment(const std::vector<CTxOut>& expected,
                                   const std::vector<CTxOut>& actual,
                                   bool strict_multiplicity);

namespace Consensus { struct Params; }

/**
 * This helper returns amount that should be reallocated to platform
 * It is calculated based on total amount of masternode rewards (not block reward)
 */
CAmount PlatformShare(const CAmount masternodeReward);

/**
 * Masternode reward era for a given block, gating how the reward is computed.
 * Each era implies the previous one: EvoReward is only reachable once CreditPool
 * (V20) is active, so the ordering encodes that invariant. The era is computed by
 * the caller (which owns the block context) and passed in, keeping this module free
 * of deployment dependencies. Note this is orthogonal to DIP0003 enforcement, which
 * gates whether payees are validated at all and is handled separately.
 */
enum class MnRewardEra {
    Classic,    // historical reward schedule, no credit pool
    CreditPool, // V20: credit pool active, no platform reallocation yet
    EvoReward,  // MN_RR: platform share is reallocated from the masternode reward
};

CAmount GetMasternodePayment(int nHeight, CAmount blockValue, const Consensus::Params& consensus_params, MnRewardEra era);

/**
 * Expected masternode payment outputs for the block after @p pindexPrev,
 * derived from the deterministic masternode list at @p pindexPrev.
 *
 * Returns false when the list has entries but no payee could be determined;
 * @p voutMasternodePaymentsRet may already carry the platform reallocation
 * output in that case.
 */
bool GetMasternodePayments(const CDeterministicMNList& mn_list, const CBlockIndex* pindexPrev,
                           const CDeterministicMNCPtr& dmnPayee, CAmount blockSubsidy, CAmount feeReward, MnRewardEra era,
                           const Consensus::Params& consensus_params,
                           std::vector<CTxOut>& voutMasternodePaymentsRet);

/** Superblocks happen once per cycle after the superblock hardfork. */
bool IsSuperblockHeight(int nBlockHeight, const Consensus::Params& consensus_params);

/** What governance says about the superblock at a block height. */
struct SuperblockStatus {
    enum class State {
        //! governance data is not loaded, so only the superblock value bounds can be checked
        ValidationDisabled,
        //! no funded superblock trigger exists for the height
        NotTriggered,
        //! a funded trigger exists; payments holds the winning trigger's outputs, in order
        Triggered,
    };
    State state{State::ValidationDisabled};
    std::vector<CTxOut> payments;
};

/**
 * Whether the coinbase @p txNew carries every payment of the triggered
 * @p superblock, in order, within the superblock payments limit, and pays
 * out no more than @p blockReward on top of them.
 */
bool IsSuperblockValid(const SuperblockStatus& superblock, const CTransaction& txNew, CAmount blockReward, bool is_v24, CAmount nPaymentsLimit);

#endif // BITCOIN_MASTERNODE_PAYMENTS_H
