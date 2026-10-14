// Copyright (c) 2022 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_KERNEL_CHAINSTATEMANAGER_OPTS_H
#define BITCOIN_KERNEL_CHAINSTATEMANAGER_OPTS_H

#include <arith_uint256.h>
#include <uint256.h>
#include <util/time.h>

#include <cstdint>
#include <functional>
#include <optional>

class CChainParams;
class CDeterministicMNList;
class CMasternodeSync;
struct SuperblockStatus;

static constexpr bool DEFAULT_CHECKPOINTS_ENABLED{true};
static constexpr auto DEFAULT_MAX_TIP_AGE{6h}; // ~144 blocks behind -> 2 x fork detection time, was 24h in bitcoin
static constexpr bool DEFAULT_EVO_PRUNE_LOCK{true};

namespace kernel {

/**
 * An options struct for `ChainstateManager`, more ergonomically referred to as
 * `ChainstateManager::Options` due to the using-declaration in
 * `ChainstateManager`.
 */
struct ChainstateManagerOpts {
    const CChainParams& chainparams;
    std::optional<bool> check_block_index{};
    bool checkpoints_enabled{DEFAULT_CHECKPOINTS_ENABLED};
    //! If set, it will override the minimum work we will assume exists on some valid chain.
    std::optional<arith_uint256> minimum_chain_work{};
    //! If set, it will override the block hash whose ancestors we will assume to have valid scripts without checking them.
    std::optional<uint256> assumed_valid_block{};
    //! If the tip is older than this, the node is considered to be in initial block download.
    std::chrono::seconds max_tip_age{DEFAULT_MAX_TIP_AGE};
    //! Dash: keep the blocks evo validation reads back from disk when pruning, see GetEvoPruneLockHeight(). Only
    //! disabled in tests, to get a datadir pruned without them.
    bool evo_prune_lock{DEFAULT_EVO_PRUNE_LOCK};
    //! Dash: governance's superblock status for a block height, consulted by
    //! block validation and the miner. Unset means superblock validation is disabled.
    std::function<SuperblockStatus(const CDeterministicMNList& mn_list, int nBlockHeight)> superblock_status{};
    //! Dash: sync state gating superblock enforcement; must stay valid while
    //! blocks are connected. Unset counts as not synced, so nothing is enforced.
    const CMasternodeSync* mn_sync{nullptr};
};

} // namespace kernel

#endif // BITCOIN_KERNEL_CHAINSTATEMANAGER_OPTS_H
