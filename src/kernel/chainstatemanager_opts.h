// Copyright (c) 2022 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_KERNEL_CHAINSTATEMANAGER_OPTS_H
#define BITCOIN_KERNEL_CHAINSTATEMANAGER_OPTS_H

#include <arith_uint256.h>
#include <dbwrapper.h>
#include <txdb.h>
#include <uint256.h>
#include <util/time.h>
#include <util/zstd.h>

#include <cstdint>
#include <functional>
#include <optional>

class CChainParams;
class CDeterministicMNList;
class CMasternodeSync;
struct SuperblockStatus;

static constexpr bool DEFAULT_CHECKPOINTS_ENABLED{true};
static constexpr auto DEFAULT_MAX_TIP_AGE{6h}; // ~144 blocks behind -> 2 x fork detection time, was 24h in bitcoin

namespace kernel {

/**
 * An options struct for `ChainstateManager`, more ergonomically referred to as
 * `ChainstateManager::Options` due to the using-declaration in
 * `ChainstateManager`.
 */
struct ChainstateManagerOpts {
    const CChainParams& chainparams;
    fs::path datadir;
    std::optional<bool> check_block_index{};
    bool checkpoints_enabled{DEFAULT_CHECKPOINTS_ENABLED};
    //! If set, it will override the minimum work we will assume exists on some valid chain.
    std::optional<arith_uint256> minimum_chain_work{};
    //! If set, it will override the block hash whose ancestors we will assume to have valid scripts without checking them.
    std::optional<uint256> assumed_valid_block{};
    //! If the tip is older than this, the node is considered to be in initial block download.
    std::chrono::seconds max_tip_age{DEFAULT_MAX_TIP_AGE};
    //! Dash: governance's superblock status for a block height, consulted by
    //! block validation and the miner. Unset means superblock validation is disabled.
    std::function<SuperblockStatus(const CDeterministicMNList& mn_list, int nBlockHeight)> superblock_status{};
    //! Dash: sync state gating superblock enforcement; must stay valid while
    //! blocks are connected. Unset counts as not synced, so nothing is enforced.
    const CMasternodeSync* mn_sync{nullptr};
    DBOptions block_tree_db{};
    DBOptions coins_db{};
    CoinsViewOptions coins_view{};
    //! zstd compression level for newly written block and undo records, 0 to store them uncompressed
    int block_compression{util::DefaultZstdLevel()};
};

} // namespace kernel

#endif // BITCOIN_KERNEL_CHAINSTATEMANAGER_OPTS_H
