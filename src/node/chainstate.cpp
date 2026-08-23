// Copyright (c) 2021 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <node/chainstate.h>

#include <chain.h>
#include <coins.h>
#include <chainparamsbase.h>
#include <consensus/params.h>
#include <deploymentstatus.h>
#include <node/blockstorage.h>
#include <node/caches.h>
#include <node/utxo_snapshot.h>
#include <sync.h>
#include <threadsafety.h>
#include <tinyformat.h>
#include <txdb.h>
#include <uint256.h>
#include <util/fs.h>
#include <util/fs_helpers.h>
#include <util/translation.h>
#include <validation.h>

#include <bls/bls.h>
#include <evo/chainhelper.h>
#include <evo/deterministicmns.h>
#include <evo/evodb.h>
#include <evo/mnhftx.h>
#include <gsl/pointers.h>
#include <llmq/context.h>

#include <atomic>
#include <cassert>
#include <memory>
#include <vector>

namespace node {
static bool RecoverSnapshotCleanup(CEvoDB& evodb, const fs::path& data_dir, bilingual_str& error)
{
    const fs::path normal{data_dir / "chainstate"};
    fs::path snapshot{normal};
    snapshot += SNAPSHOT_CHAINSTATE_SUFFIX;
    fs::path to_delete{normal};
    to_delete += SNAPSHOT_TODELETE_SUFFIX;
    fs::path invalid{snapshot};
    invalid += SNAPSHOT_INVALID_SUFFIX;

    uint256 snapshot_tip;
    const bool has_snapshot_tip{evodb.ReadBestBlock(EvoDbIdentity::SNAPSHOT, snapshot_tip)};
    uint256 ignored;
    const bool has_metadata{has_snapshot_tip || evodb.HasDualChainstateMarker() ||
                            evodb.ReadSnapshotBaseMNListHash(ignored)};

    // The first validated-cleanup rename completed. Roll it back so the normal
    // cleanup path can deterministically retry both renames.
    if (fs::exists(to_delete) && fs::exists(snapshot) && !fs::exists(normal)) {
        LogPrintf("[snapshot] rolling back interrupted background chainstate rename\n");
        try {
            RenameDurably(to_delete, normal);
        } catch (const fs::filesystem_error& e) {
            error = strprintf(_("Failed to recover interrupted snapshot cleanup: %s"), e.what());
            return false;
        }
        return true;
    }

    // Both validated-cleanup renames completed. The NORMAL coins directory now
    // contains the snapshot tip, while lifecycle markers still describe the dual
    // state. Match those durable facts before promoting the markers.
    if (fs::exists(normal) && !fs::exists(snapshot) && has_metadata) {
        uint256 coins_tip;
        try {
            CCoinsViewDB coins_db{normal, /*nCacheSize=*/1 << 20, /*fMemory=*/false, /*fWipe=*/false};
            coins_tip = coins_db.GetBestBlock();
        } catch (const std::exception& e) {
            error = strprintf(_("Failed to inspect interrupted snapshot cleanup: %s"), e.what());
            return false;
        }
        // Caution: this condition can also match a crash between an
        // invalid-snapshot rename and its marker discard when validation
        // failed with the background tip AT the base block (HASH_MISMATCH /
        // EVO_STATE_MISMATCH): the background coins tip and the SNAPSHOT
        // marker both hold the base hash. Taking the promote branch there is
        // deliberate and equivalent: PromoteSnapshotMarkers rewrites the
        // NORMAL best-block with the value it already has and erases the same
        // marker set DiscardSnapshotMarkers would. Keep the two functions'
        // side effects equivalent under that overlap, or disambiguate here.
        if (has_snapshot_tip && coins_tip == snapshot_tip) {
            LogPrintf("[snapshot] completing interrupted snapshot marker promotion\n");
            if (!evodb.PromoteSnapshotMarkers(snapshot_tip)) {
                error = _("Failed to finish interrupted snapshot marker promotion.");
                return false;
            }
            if (fs::exists(to_delete)) {
                try {
                    RemoveAllDurably(to_delete);
                } catch (const fs::filesystem_error& e) {
                    LogPrintf("[snapshot] unable to remove recovered background chainstate directory: %s\n", e.what());
                }
            }
            return true;
        }

        // Invalid snapshot rename completed, but the synced marker discard did
        // not. A stale _INVALID directory cannot mask a completed valid swap:
        // the coins/snapshot-tip match above always takes precedence.
        if (fs::exists(invalid)) {
            LogPrintf("[snapshot] completing interrupted invalid-snapshot marker cleanup\n");
            if (!evodb.DiscardSnapshotMarkers()) {
                error = _("Failed to finish interrupted invalid snapshot cleanup.");
                return false;
            }
            return true;
        }

        error = has_snapshot_tip
            ? _("Interrupted snapshot cleanup has inconsistent coins and EvoDB tips.")
            : _("Interrupted snapshot cleanup is missing its snapshot EvoDB marker.");
        return false;
    }

    // Marker promotion can become durable before deletion of the old background
    // directory. Treat that retry as success and finish the nonessential deletion.
    if (!has_metadata && fs::exists(normal) && !fs::exists(snapshot) && fs::exists(to_delete)) {
        try {
            RemoveAllDurably(to_delete);
        } catch (const fs::filesystem_error& e) {
            LogPrintf("[snapshot] unable to remove promoted background chainstate directory: %s\n", e.what());
        }
    }

    // No metadata is otherwise the already-promoted/no-snapshot state and needs no work.
    return true;
}

// Complete initialization of chainstates after the initial call has been made
// to ChainstateManager::InitializeChainstate().
static ChainstateLoadResult CompleteChainstateInitialization(ChainstateManager& chainman, const CacheSizes& cache_sizes,
                                                             const ChainstateLoadOptions& options, CEvoDB& evodb,
                                                             CDeterministicMNManager& dmnman,
                                                             std::unique_ptr<LLMQContext>& llmq_ctx,
                                                             std::unique_ptr<CChainstateHelper>& chain_helper)
    EXCLUSIVE_LOCKS_REQUIRED(::cs_main)
{
    const bool to_wipe_data = options.reindex || options.reindex_chainstate;

    auto& pblocktree{chainman.m_blockman.m_block_tree_db};
    // new CBlockTreeDB tries to delete the existing file, which
    // fails if it's still open from the previous loop. Close it first:
    pblocktree.reset();
    pblocktree.reset(new CBlockTreeDB(cache_sizes.block_tree_db, options.block_tree_db_in_memory, options.reindex));

    // Initialize llmq_ctx
    llmq_ctx.reset();
    llmq_ctx = std::make_unique<LLMQContext>(dmnman, evodb, chainman,
                                             util::DbWrapperParams{.path = options.data_dir, .memory = options.dash_dbs_in_memory, .wipe = to_wipe_data},
                                             options.bls_threads, options.worker_count, options.max_recsigs_age);

    // Initialize chain_helper
    chain_helper.reset();
    chain_helper = std::make_unique<CChainstateHelper>(evodb, dmnman, *options.mn_sync, *options.isman, *(llmq_ctx->quorum_block_processor),
                                                       *(llmq_ctx->qsnapman), chainman, chainman.GetConsensus(), *options.chainlocks,
                                                       *(llmq_ctx->qman));

    if (options.reindex) {
        pblocktree->WriteReindexing(true);
        //If we're reindexing in prune mode, wipe away unusable block files and all undo data files
        if (options.prune) {
            CleanupBlockRevFiles();
        }
    }

    if (options.check_interrupt && options.check_interrupt()) return {ChainstateLoadStatus::INTERRUPTED, {}};

    // LoadBlockIndex will load m_have_pruned if we've ever removed a
    // block file from disk.
    // Note that it also sets fReindex global based on the disk flag!
    // From here on, fReindex and options.reindex values may be different!
    if (!chainman.LoadBlockIndex()) {
        if (options.check_interrupt && options.check_interrupt()) return {ChainstateLoadStatus::INTERRUPTED, {}};
        return {ChainstateLoadStatus::FAILURE, _("Error loading block database")};
    }

    // Detection happens before LoadBlockIndex. Once the base is resolvable,
    // keep its full block available for Dash's completion-time CbTx check.
    chainman.ProtectSnapshotBaseFromPruning();

    if (!chainman.BlockIndex().empty() &&
            !chainman.m_blockman.LookupBlockIndex(chainman.GetConsensus().hashGenesisBlock)) {
        // If the loaded chain has a wrong genesis, bail out immediately
        // (we're likely using a testnet datadir, or the other way around).
        return {ChainstateLoadStatus::FAILURE_INCOMPATIBLE_DB, _("Incorrect or no genesis block found. Wrong datadir for network?")};
    }

    if (!chainman.GetConsensus().hashDevnetGenesisBlock.IsNull() && !chainman.BlockIndex().empty() &&
            !chainman.m_blockman.LookupBlockIndex(chainman.GetConsensus().hashDevnetGenesisBlock)) {
        return {ChainstateLoadStatus::FAILURE_INCOMPATIBLE_DB, _("Incorrect or no devnet genesis block found. Wrong datadir for devnet specified?")};
    }

    // Check for changed -prune state.  What we are concerned about is a user who has pruned blocks
    // in the past, but is now trying to run unpruned.
    if (chainman.m_blockman.m_have_pruned && !options.prune) {
        return {ChainstateLoadStatus::FAILURE, _("You need to rebuild the database using -reindex to go back to unpruned mode.  This will redownload the entire blockchain")};
    }

    // At this point blocktree args are consistent with what's on disk.
    // If we're not mid-reindex (based on disk + args), add a genesis block on disk
    // (otherwise we use the one already on disk).
    // This is called again in ThreadImport after the reindex completes.
    if (!fReindex && !chainman.ActiveChainstate().LoadGenesisBlock()) {
        return {ChainstateLoadStatus::FAILURE, _("Error initializing block database")};
    }

    auto is_coinsview_empty = [&](Chainstate* chainstate) EXCLUSIVE_LOCKS_REQUIRED(::cs_main) {
        return options.reindex || options.reindex_chainstate || chainstate->CoinsTip().GetBestBlock().IsNull();
    };

    assert(chainman.m_total_coinstip_cache > 0);
    assert(chainman.m_total_coinsdb_cache > 0);

    // Conservative value which is arbitrarily chosen, as it will ultimately be changed
    // by a call to `chainman.MaybeRebalanceCaches()`. We just need to make sure
    // that the sum of the two caches (40%) does not exceed the allowable amount
    // during this temporary initialization state.
    double init_cache_fraction = 0.2;

    // At this point we're either in reindex or we've loaded a useful
    // block tree into BlockIndex()!

    for (Chainstate* chainstate : chainman.GetAll()) {
        LogPrintf("Initializing chainstate %s\n", chainstate->ToString());

        chainstate->InitCoinsDB(
            /*cache_size_bytes=*/chainman.m_total_coinsdb_cache * init_cache_fraction,
            /*in_memory=*/options.coins_db_in_memory,
            /*should_wipe=*/options.reindex || options.reindex_chainstate);

        if (options.coins_error_cb) {
            chainstate->CoinsErrorCatcher().AddReadErrCallback(options.coins_error_cb);
        }

        // Refuse to load unsupported database format.
        // This is a no-op if we cleared the coinsviewdb with -reindex or -reindex-chainstate
        if (chainstate->CoinsDB().NeedsUpgrade()) {
            return {ChainstateLoadStatus::FAILURE_INCOMPATIBLE_DB, _("Unsupported chainstate database format found. "
                                                                     "Please restart with -reindex-chainstate. This will "
                                                                     "rebuild the chainstate database.")};
        }

        // ReplayBlocks is a no-op if we cleared the coinsviewdb with -reindex or -reindex-chainstate
        if (!chainstate->ReplayBlocks()) {
            return {ChainstateLoadStatus::FAILURE, _("Unable to replay blocks. You will need to rebuild the database using -reindex-chainstate.")};
        }

        // The on-disk coinsdb is now in a good state, create the cache
        chainstate->InitCoinsCache(chainman.m_total_coinstip_cache * init_cache_fraction);
        assert(chainstate->CanFlushToDisk());

        // flush evodb
        // TODO: CEvoDB instance should probably be a part of Chainstate
        // (for multiple chainstates to actually work in parallel)
        // and not a global
        // Commit every chainstate's own identity: ReplayBlocks processes
        // special transactions, and its coins repair is flushed to disk
        // immediately, so leaving a non-active identity's EvoDB writes in the
        // in-memory overlay would let a crash strand the coins DB ahead of
        // that identity's best-block marker.
        if (!evodb.CommitRootTransaction(chainstate->EvoDbIdentity())) {
            return {ChainstateLoadStatus::FAILURE, _("Failed to commit Evo database")};
        }

        if (!is_coinsview_empty(chainstate)) {
            // LoadChainTip initializes the chain based on CoinsTip()'s best block
            if (!chainstate->LoadChainTip()) {
                return {ChainstateLoadStatus::FAILURE, _("Error initializing block database")};
            }
            assert(chainstate->m_chain.Tip() != nullptr);
        }
    }

    if (!chain_helper->ehf_manager->ForceSignalDBUpdate()) {
        return {ChainstateLoadStatus::FAILURE, _("Error upgrading evo database for EHF")};
    }

    // Check if nVersion-first migration is needed and perform it
    if (dmnman.IsMigrationRequired() && !dmnman.MigrateLegacyDiffs(chainman.ActiveChainstate().m_chain.Tip())) {
        return {ChainstateLoadStatus::FAILURE, _("Failed to upgrade Evo database")};
    }

    // Now that chainstates are loaded and we're able to flush to
    // disk, rebalance the coins caches to desired levels based
    // on the condition of each chainstate.
    chainman.MaybeRebalanceCaches();

    return {ChainstateLoadStatus::SUCCESS, {}};
}

ChainstateLoadResult LoadChainstate(ChainstateManager& chainman, const CacheSizes& cache_sizes,
                                    const ChainstateLoadOptions& options, CEvoDB& evodb,
                                    CDeterministicMNManager& dmnman, std::unique_ptr<LLMQContext>& llmq_ctx,
                                    std::unique_ptr<CChainstateHelper>& chain_helper)
{
    assert(options.isman);
    assert(options.chainlocks);
    assert(options.mn_sync);

    if (!hashAssumeValid.IsNull()) {
        LogPrintf("Assuming ancestors of block %s have valid signatures.\n", hashAssumeValid.GetHex());
    } else {
        LogPrintf("Validating signatures for all blocks.\n");
    }
    LogPrintf("Setting nMinimumChainWork=%s\n", nMinimumChainWork.GetHex());
    if (nMinimumChainWork < UintToArith256(chainman.GetConsensus().nMinimumChainWork)) {
        LogPrintf("Warning: nMinimumChainWork set below default value of %s\n", chainman.GetConsensus().nMinimumChainWork.GetHex());
    }
    if (nPruneTarget == std::numeric_limits<uint64_t>::max()) {
        LogPrintf("Block pruning enabled.  Use RPC call pruneblockchain(height) to manually prune block and undo files.\n");
    } else if (nPruneTarget) {
        LogPrintf("Prune configured to target %u MiB on disk for block and undo files.\n", nPruneTarget / 1024 / 1024);
    }

    LOCK(cs_main);

    if (!options.dash_dbs_in_memory && !options.reindex && !options.reindex_chainstate) {
        bilingual_str recovery_error;
        if (!RecoverSnapshotCleanup(evodb, options.data_dir, recovery_error)) {
            return {ChainstateLoadStatus::FAILURE, recovery_error};
        }
    }
    chainman.m_total_coinstip_cache = cache_sizes.coins;
    chainman.m_total_coinsdb_cache = cache_sizes.coins_db;

    // Load the fully validated chainstate.
    chainman.InitializeChainstate(options.mempool, evodb, chain_helper);

    // On a reindex the caller hands us a freshly wiped EvoDB, without the
    // SNAPSHOT best-block marker that ActivateExistingSnapshot() requires, so a
    // persisted snapshot chainstate can no longer be revived. Discard it here
    // rather than letting startup fail with advice ("reindex") the user has
    // just followed, which would never recover.
    if ((options.reindex || options.reindex_chainstate) && !DeleteSnapshotChainstateFromDisk()) {
        return {ChainstateLoadStatus::FAILURE,
                _("Failed to remove the snapshot chainstate directory. Remove it manually before restarting.")};
    }

    // Load a chain created from a UTXO snapshot, if any exist.
    bilingual_str snapshot_error;
    if (!chainman.DetectSnapshotChainstate(options.mempool, snapshot_error)) {
        return {ChainstateLoadStatus::FAILURE, snapshot_error};
    }

    auto [init_status, init_error] = CompleteChainstateInitialization(chainman, cache_sizes, options, evodb, dmnman,
                                                                      llmq_ctx, chain_helper);
    if (init_status != ChainstateLoadStatus::SUCCESS) {
        return {init_status, init_error};
    }

    // If a snapshot chainstate was fully validated by a background chainstate during
    // the last run, detect it here and clean up the now-unneeded background
    // chainstate.
    //
    // Why is this cleanup done here (on subsequent restart) and not just when the
    // snapshot is actually validated? Because this entails unusual
    // filesystem operations to move leveldb data directories around, and that seems
    // too risky to do in the middle of normal runtime.
    const auto snapshot_completion = chainman.MaybeCompleteSnapshotValidation();

    if (snapshot_completion == SnapshotCompletionResult::SKIPPED) {
        // Do nothing; expected case.
    } else if (snapshot_completion == SnapshotCompletionResult::SUCCESS) {
        LogPrintf("[snapshot] cleaning up unneeded background chainstate, then reinitializing\n");
        chain_helper.reset();
        llmq_ctx.reset();
        if (!chainman.ValidatedSnapshotCleanup()) {
            return {ChainstateLoadStatus::FAILURE_FATAL, Untranslated("Background chainstate cleanup failed unexpectedly.")};
        }

        // Because ValidatedSnapshotCleanup() has torn down chainstates with
        // ChainstateManager::ResetChainstates(), reinitialize them here without
        // duplicating the blockindex work above.
        assert(chainman.GetAll().empty());
        assert(!chainman.IsSnapshotActive());
        assert(!chainman.IsSnapshotValidated());

        chainman.InitializeChainstate(options.mempool, evodb, chain_helper);

        // A reload of the block index is required to recompute setBlockIndexCandidates
        // for the fully validated chainstate.
        chainman.ActiveChainstate().ClearBlockIndexCandidates();

        std::tie(init_status, init_error) = CompleteChainstateInitialization(chainman, cache_sizes, options, evodb,
                                                                             dmnman, llmq_ctx, chain_helper);
        if (init_status != ChainstateLoadStatus::SUCCESS) {
            return {init_status, init_error};
        }
    } else {
        // The UTXO hashing inside completion takes minutes and aborts with
        // STATS_FAILED when shutdown is requested mid-way. That says nothing
        // about the snapshot, so report the interruption instead of telling
        // the user to discard a perfectly good snapshot.
        if (options.check_interrupt && options.check_interrupt()) {
            return {ChainstateLoadStatus::INTERRUPTED, {}};
        }
        return {ChainstateLoadStatus::FAILURE, _(
           "UTXO snapshot failed to validate. "
           "Restart to resume normal initial block download, or try loading a different snapshot.")};
    }

    return {ChainstateLoadStatus::SUCCESS, {}};
}

ChainstateLoadResult VerifyLoadedChainstate(ChainstateManager& chainman, const ChainstateLoadOptions& options, CEvoDB& evodb,
                                            std::function<void(bool)> notify_bls_state)
{
    auto is_coinsview_empty = [&](Chainstate* chainstate) EXCLUSIVE_LOCKS_REQUIRED(::cs_main) {
        return options.reindex || options.reindex_chainstate || chainstate->CoinsTip().GetBestBlock().IsNull();
    };

    LOCK(cs_main);

    for (Chainstate* chainstate : chainman.GetAll()) {
        if (!is_coinsview_empty(chainstate)) {
            const CBlockIndex* tip = chainstate->m_chain.Tip();
            if (tip && tip->nTime > GetTime() + MAX_FUTURE_BLOCK_TIME) {
                return {ChainstateLoadStatus::FAILURE, _("The block database contains a block which appears to be from the future. "
                                                         "This may be due to your computer's date and time being set incorrectly. "
                                                         "Only rebuild the block database if you are sure that your computer's date and time are correct")};
            }
            const bool v19active{DeploymentActiveAfter(tip, chainman, Consensus::DEPLOYMENT_V19)};
            if (v19active) {
                bls::bls_legacy_scheme.store(false);
                // TODO: remove notify_bls_state, it's alien and irrelevant once v19 activated
                if (notify_bls_state) notify_bls_state(bls::bls_legacy_scheme.load());
            }

            if (!CVerifyDB().VerifyDB(
                    *chainstate, chainman.GetConsensus(), chainstate->CoinsDB(),
                    evodb,
                    options.check_level,
                    options.check_blocks)) {
                return {ChainstateLoadStatus::FAILURE, _("Corrupted block database detected")};
            }

            // VerifyDB() disconnects blocks which might result in us switching back to legacy.
            // Make sure we use the right scheme.
            if (v19active && bls::bls_legacy_scheme.load()) {
                bls::bls_legacy_scheme.store(false);
                if (notify_bls_state) notify_bls_state(bls::bls_legacy_scheme.load());
            }

            if (options.check_level >= 3) {
                chainstate->ResetBlockFailureFlags(nullptr);
            }

        } else {
            // TODO: CEvoDB instance should probably be a part of Chainstate
            // (for multiple chainstates to actually work in parallel)
            // and not a global
            if (&chainman.ActiveChainstate() == chainstate && !evodb.IsEmpty()) {
                // EvoDB processed some blocks earlier but we have no blocks anymore, something is wrong
                return {ChainstateLoadStatus::FAILURE, _("Error initializing block database")};
            }
        }
    }

    return {ChainstateLoadStatus::SUCCESS, {}};
}
} // namespace node
