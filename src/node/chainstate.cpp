// Copyright (c) 2021 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <node/chainstate.h>

#include <consensus/params.h>
#include <deploymentstatus.h>
#include <node/blockstorage.h>
#include <node/context.h>
#include <validation.h>

#include <evo/chainhelper.h>
#include <evo/creditpool.h>
#include <evo/deterministicmns.h>
#include <evo/evodb.h>
#include <evo/mnhftx.h>
#include <llmq/chainlocks.h>
#include <llmq/context.h>
#include <llmq/instantsend.h>
#include <llmq/snapshot.h>

std::optional<ChainstateLoadingError> LoadChainstate(bool fReset,
                                                     ChainstateManager& chainman,
                                                     NodeContext& node,
                                                     bool fPruneMode,
                                                     bool is_addrindex_enabled,
                                                     bool is_governance_enabled,
                                                     bool is_spentindex_enabled,
                                                     bool is_timeindex_enabled,
                                                     bool is_txindex_enabled,
                                                     const Consensus::Params& consensus_params,
                                                     const std::string& network_id,
                                                     bool fReindexChainState,
                                                     int64_t nBlockTreeDBCache,
                                                     int64_t nCoinDBCache,
                                                     int64_t nCoinCacheUsage,
                                                     bool block_tree_db_in_memory,
                                                     bool coins_db_in_memory,
                                                     std::function<bool()> shutdown_requested,
                                                     std::function<void()> coins_error_cb)
{
    auto is_coinsview_empty = [&](CChainState* chainstate) EXCLUSIVE_LOCKS_REQUIRED(::cs_main) {
        return fReset || fReindexChainState || chainstate->CoinsTip().GetBestBlock().IsNull();
    };

    LOCK(cs_main);

    int64_t nEvoDbCache{64 * 1024 * 1024}; // TODO
    node.evodb.reset();
    node.evodb = std::make_unique<CEvoDB>(nEvoDbCache, false, fReset || fReindexChainState);

    node.mnhf_manager.reset();
    node.mnhf_manager = std::make_unique<CMNHFManager>(*node.evodb);

    chainman.InitializeChainstate(Assert(node.mempool.get()), *node.evodb, node.chain_helper, llmq::chainLocksHandler, llmq::quorumInstantSendManager);
    chainman.m_total_coinstip_cache = nCoinCacheUsage;
    chainman.m_total_coinsdb_cache = nCoinDBCache;

    auto& pblocktree{chainman.m_blockman.m_block_tree_db};
    // new CBlockTreeDB tries to delete the existing file, which
    // fails if it's still open from the previous loop. Close it first:
    pblocktree.reset();
    pblocktree.reset(new CBlockTreeDB(nBlockTreeDBCache, block_tree_db_in_memory, fReset));

    DashChainstateSetup(chainman, node, fReset, fReindexChainState, consensus_params);

    if (fReset) {
        pblocktree->WriteReindexing(true);
        //If we're reindexing in prune mode, wipe away unusable block files and all undo data files
        if (fPruneMode)
            CleanupBlockRevFiles();
    }

    if (shutdown_requested && shutdown_requested()) return ChainstateLoadingError::SHUTDOWN_PROBED;

    // LoadBlockIndex will load m_have_pruned if we've ever removed a
    // block file from disk.
    // Note that it also sets fReindex based on the disk flag!
    // From here on out fReindex and fReset mean something different!
    if (!chainman.LoadBlockIndex()) {
        if (shutdown_requested && shutdown_requested()) return ChainstateLoadingError::SHUTDOWN_PROBED;
        return ChainstateLoadingError::ERROR_LOADING_BLOCK_DB;
    }

    if (!chainman.BlockIndex().empty() &&
            !chainman.m_blockman.LookupBlockIndex(consensus_params.hashGenesisBlock)) {
        return ChainstateLoadingError::ERROR_BAD_GENESIS_BLOCK;
    }

    if (!consensus_params.hashDevnetGenesisBlock.IsNull() && !chainman.BlockIndex().empty() &&
            !chainman.m_blockman.LookupBlockIndex(consensus_params.hashDevnetGenesisBlock)) {
        return ChainstateLoadingError::ERROR_BAD_DEVNET_GENESIS_BLOCK;
    }

    // TODO: Remove this when pruning is fixed.
    // See https://github.com/dashpay/dash/pull/1817 and https://github.com/dashpay/dash/pull/1743
    if (is_governance_enabled && !is_txindex_enabled && network_id != CBaseChainParams::REGTEST) {
        return ChainstateLoadingError::ERROR_TXINDEX_DISABLED_WHEN_GOV_ENABLED;
    }

    // Check for changed -addressindex state
    if (fAddressIndex != is_addrindex_enabled) {
        return ChainstateLoadingError::ERROR_ADDRIDX_NEEDS_REINDEX;
    }

    // Check for changed -spentindex state
    if (fSpentIndex != is_spentindex_enabled) {
        return ChainstateLoadingError::ERROR_SPENTIDX_NEEDS_REINDEX;
    }

    // Check for changed -timestampindex state
    if (fTimestampIndex != is_timeindex_enabled) {
        return ChainstateLoadingError::ERROR_TIMEIDX_NEEDS_REINDEX;
    }

    // Check for changed -prune state.  What we are concerned about is a user who has pruned blocks
    // in the past, but is now trying to run unpruned.
    if (chainman.m_blockman.m_have_pruned && !fPruneMode) {
        return ChainstateLoadingError::ERROR_PRUNED_NEEDS_REINDEX;
    }

    // At this point blocktree args are consistent with what's on disk.
    // If we're not mid-reindex (based on disk + args), add a genesis block on disk
    // (otherwise we use the one already on disk).
    // This is called again in ThreadImport after the reindex completes.
    if (!fReindex && !chainman.ActiveChainstate().LoadGenesisBlock()) {
        return ChainstateLoadingError::ERROR_LOAD_GENESIS_BLOCK_FAILED;
    }

    // At this point we're either in reindex or we've loaded a useful
    // block tree into BlockIndex()!

    for (CChainState* chainstate : chainman.GetAll()) {
        chainstate->InitCoinsDB(
            /* cache_size_bytes */ nCoinDBCache,
            /* in_memory */ coins_db_in_memory,
            /* should_wipe */ fReset || fReindexChainState);

        if (coins_error_cb) {
            chainstate->CoinsErrorCatcher().AddReadErrCallback(coins_error_cb);
        }

        // If necessary, upgrade from older database format.
        // This is a no-op if we cleared the coinsviewdb with -reindex or -reindex-chainstate
        if (!chainstate->CoinsDB().Upgrade()) {
            return ChainstateLoadingError::ERROR_CHAINSTATE_UPGRADE_FAILED;
        }

        // ReplayBlocks is a no-op if we cleared the coinsviewdb with -reindex or -reindex-chainstate
        if (!chainstate->ReplayBlocks()) {
            return ChainstateLoadingError::ERROR_REPLAYBLOCKS_FAILED;
        }

        // The on-disk coinsdb is now in a good state, create the cache
        chainstate->InitCoinsCache(nCoinCacheUsage);
        assert(chainstate->CanFlushToDisk());

        // flush evodb
        // TODO: CEvoDB instance should probably be a part of CChainState
        // (for multiple chainstates to actually work in parallel)
        // and not a global
        if (&chainman.ActiveChainstate() == chainstate && !node.evodb->CommitRootTransaction()) {
            return ChainstateLoadingError::ERROR_COMMITING_EVO_DB;
        }

        if (!is_coinsview_empty(chainstate)) {
            // LoadChainTip initializes the chain based on CoinsTip()'s best block
            if (!chainstate->LoadChainTip()) {
                return ChainstateLoadingError::ERROR_LOADCHAINTIP_FAILED;
            }
            assert(chainstate->m_chain.Tip() != nullptr);
        }
    }

    if (!node.dmnman->MigrateDBIfNeeded() || !node.dmnman->MigrateDBIfNeeded2()) {
        return ChainstateLoadingError::ERROR_UPGRADING_EVO_DB;
    }

    return std::nullopt;
}

void DashChainstateSetup(ChainstateManager& chainman,
                         NodeContext& node,
                         bool fReset,
                         bool fReindexChainState,
                         const Consensus::Params& consensus_params)
{
    // Same logic as pblocktree
    node.dmnman.reset();
    node.dmnman = std::make_unique<CDeterministicMNManager>(chainman.ActiveChainstate(), *node.connman, *node.evodb);
    node.mempool->ConnectManagers(node.dmnman.get());

    node.cpoolman.reset();
    node.cpoolman = std::make_unique<CCreditPoolManager>(*node.evodb);

    llmq::quorumSnapshotManager.reset();
    llmq::quorumSnapshotManager.reset(new llmq::CQuorumSnapshotManager(*node.evodb));

    if (node.llmq_ctx) {
        node.llmq_ctx->Interrupt();
        node.llmq_ctx->Stop();
    }
    node.llmq_ctx.reset();
    node.llmq_ctx = std::make_unique<LLMQContext>(chainman, *node.connman, *node.dmnman, *node.evodb, *node.mn_metaman, *node.mnhf_manager, *node.sporkman,
                                                  *node.mempool, node.mn_activeman.get(), *node.mn_sync, node.peerman, /* unit_tests = */ false, /* wipe = */ fReset || fReindexChainState);
    // Enable CMNHFManager::{Process, Undo}Block
    node.mnhf_manager->ConnectManagers(node.chainman.get(), node.llmq_ctx->qman.get());

    // Have to start it early to let VerifyDB check ChainLock signatures in coinbase
    node.llmq_ctx->Start();

    node.chain_helper.reset();
    node.chain_helper = std::make_unique<CChainstateHelper>(*node.cpoolman, *node.dmnman, *node.mnhf_manager, *node.govman, *(node.llmq_ctx->quorum_block_processor), *node.chainman,
                                                            consensus_params, *node.mn_sync, *node.sporkman, *(node.llmq_ctx->clhandler), *(node.llmq_ctx->qman));
}

void DashChainstateSetupClose(NodeContext& node)
{
    assert(node.chainman);

    node.chain_helper.reset();
    if (node.mnhf_manager) {
        node.mnhf_manager->DisconnectManagers();
    }
    node.llmq_ctx.reset();
    llmq::quorumSnapshotManager.reset();
    node.mempool->DisconnectManagers();
    node.dmnman.reset();
}

std::optional<ChainstateLoadVerifyError> VerifyLoadedChainstate(ChainstateManager& chainman,
                                                                CEvoDB& evodb,
                                                                bool fReset,
                                                                bool fReindexChainState,
                                                                const Consensus::Params& consensus_params,
                                                                unsigned int check_blocks,
                                                                unsigned int check_level,
                                                                std::function<int64_t()> get_unix_time_seconds)
{
    auto is_coinsview_empty = [&](CChainState* chainstate) EXCLUSIVE_LOCKS_REQUIRED(::cs_main) {
        return fReset || fReindexChainState || chainstate->CoinsTip().GetBestBlock().IsNull();
    };

    LOCK(cs_main);

    for (CChainState* chainstate : chainman.GetAll()) {
        if (!is_coinsview_empty(chainstate)) {
            const CBlockIndex* tip = chainstate->m_chain.Tip();
            if (tip && tip->nTime > get_unix_time_seconds() + 2 * 60 * 60) {
                return ChainstateLoadVerifyError::ERROR_BLOCK_FROM_FUTURE;
            }
            const bool v19active{DeploymentActiveAfter(tip, consensus_params, Consensus::DEPLOYMENT_V19)};
            if (v19active) {
                bls::bls_legacy_scheme.store(false);
                LogPrintf("%s: bls_legacy_scheme=%d\n", __func__, bls::bls_legacy_scheme.load());
            }

            if (!CVerifyDB().VerifyDB(
                    *chainstate, consensus_params, chainstate->CoinsDB(),
                    evodb,
                    check_level,
                    check_blocks)) {
                return ChainstateLoadVerifyError::ERROR_CORRUPTED_BLOCK_DB;
            }

            // VerifyDB() disconnects blocks which might result in us switching back to legacy.
            // Make sure we use the right scheme.
            if (v19active && bls::bls_legacy_scheme.load()) {
                bls::bls_legacy_scheme.store(false);
                LogPrintf("%s: bls_legacy_scheme=%d\n", __func__, bls::bls_legacy_scheme.load());
            }

            if (check_level >= 3) {
                chainstate->ResetBlockFailureFlags(nullptr);
            }

        } else {
            // TODO: CEvoDB instance should probably be a part of CChainState
            // (for multiple chainstates to actually work in parallel)
            // and not a global
            if (&chainman.ActiveChainstate() == chainstate && !evodb.IsEmpty()) {
                // EvoDB processed some blocks earlier but we have no blocks anymore, something is wrong
                return ChainstateLoadVerifyError::ERROR_EVO_DB_SANITY_FAILED;
            }
        }
    }

    return std::nullopt;
}
