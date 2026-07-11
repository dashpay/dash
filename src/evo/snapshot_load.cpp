// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <validation.h>

#include <chain.h>
#include <chainparams.h>
#include <chainparamsbase.h>
#include <clientversion.h>
#include <deploymentstatus.h>
#include <evo/cbtx.h>
#include <evo/chainhelper.h>
#include <evo/deterministicmns.h>
#include <evo/evodb.h>
#include <evo/mnhftx.h>
#include <evo/snapshot.h>
#include <evo/specialtx.h>
#include <kernel/coinstats.h>
#include <llmq/blockprocessor.h>
#include <llmq/snapshot.h>
#include <logging.h>
#include <logging/timer.h>
#include <node/blockstorage.h>
#include <node/utxo_snapshot.h>
#include <random.h>
#include <shutdown.h>
#include <streams.h>
#include <txdb.h>
#include <util/check.h>
#include <util/system.h>
#include <versionbits.h>

#include <exception>
#include <limits>
#include <map>
#include <optional>
#include <string>
#include <utility>

using kernel::CCoinsStats;
using kernel::CoinStatsHashType;
using kernel::ComputeUTXOStats;
using node::ReadBlockFromDisk;
using node::SnapshotMetadata;

static void FlushSnapshotToDisk(CCoinsViewCache& coins_cache, bool snapshot_loaded)
{
    LOG_TIME_MILLIS_WITH_CATEGORY_MSG_ONCE(
        strprintf("%s (%.2f MB)",
                  snapshot_loaded ? "saving snapshot chainstate" : "flushing coins cache",
                  coins_cache.DynamicMemoryUsage() / (1000 * 1000)),
        BCLog::LogFlags::ALL);

    coins_cache.Flush();
}

struct StopHashingException : public std::exception
{
    const char* what() const throw() override
    {
        return "ComputeUTXOStats interrupted by shutdown.";
    }
};

static void SnapshotUTXOHashBreakpoint()
{
    if (ShutdownRequested()) throw StopHashingException();
}

bool ChainstateManager::PopulateAndValidateSnapshot(
    Chainstate& snapshot_chainstate,
    AutoFile& coins_file,
    const SnapshotMetadata& metadata)
{
    // It's okay to release cs_main before we're done using `coins_cache` because we know
    // that nothing else will be referencing the newly created snapshot_chainstate yet.
    CCoinsViewCache& coins_cache = WITH_LOCK(::cs_main, return snapshot_chainstate.CoinsTip());

    uint256 base_blockhash = metadata.m_base_blockhash;

    CBlockIndex* snapshot_start_block = WITH_LOCK(::cs_main, return m_blockman.LookupBlockIndex(base_blockhash));

    if (!snapshot_start_block) {
        // Needed for ComputeUTXOStats and ExpectedAssumeutxo to determine the
        // height and to avoid a crash when base_blockhash.IsNull()
        LogPrintf("[snapshot] Did not find snapshot start blockheader %s\n",
                  base_blockhash.ToString());
        return false;
    }

    // Protect the full base block before the long-running population step.
    // Snapshot activation is not visible yet, so use the resolved base directly.
    WITH_LOCK(::cs_main, m_blockman.UpdatePruneLock(
        "assumeutxo", {.height_first = snapshot_start_block->nHeight}));

    int base_height = snapshot_start_block->nHeight;
    auto maybe_au_data = ExpectedAssumeutxo(base_height, GetParams());

    if (!maybe_au_data) {
        LogPrintf("[snapshot] assumeutxo height in snapshot metadata not recognized " /* Continued */
                  "(%d) - refusing to load snapshot\n", base_height);
        return false;
    }

    const AssumeutxoData& au_data = *maybe_au_data;

    COutPoint outpoint;
    Coin coin;
    const uint64_t coins_count = metadata.m_coins_count;
    uint64_t coins_left = metadata.m_coins_count;

    LogPrintf("[snapshot] loading coins from snapshot %s\n", base_blockhash.ToString());
    int64_t coins_processed{0};

    while (coins_left > 0) {
        try {
            coins_file >> outpoint;
            coins_file >> coin;
        } catch (const std::ios_base::failure&) {
            LogPrintf("[snapshot] bad snapshot format or truncated snapshot after deserializing %d coins\n",
                      coins_count - coins_left);
            return false;
        }
        if (coin.nHeight > base_height ||
            outpoint.n >= std::numeric_limits<decltype(outpoint.n)>::max() // Avoid integer wrap-around in coinstats.cpp:ApplyHash
        ) {
            LogPrintf("[snapshot] bad snapshot data after deserializing %d coins\n",
                      coins_count - coins_left);
            return false;
        }

        coins_cache.EmplaceCoinInternalDANGER(std::move(outpoint), std::move(coin));

        --coins_left;
        ++coins_processed;

        if (coins_processed % 1000000 == 0) {
            LogPrintf("[snapshot] %d coins loaded (%.2f%%, %.2f MB)\n",
                coins_processed,
                static_cast<float>(coins_processed) * 100 / static_cast<float>(coins_count),
                coins_cache.DynamicMemoryUsage() / (1000 * 1000));
        }

        // Batch write and flush (if we need to) every so often.
        //
        // If our average Coin size is roughly 41 bytes, checking every 120,000 coins
        // means <5MB of memory imprecision.
        if (coins_processed % 120000 == 0) {
            if (ShutdownRequested()) {
                return false;
            }

            const auto snapshot_cache_state = WITH_LOCK(::cs_main,
                return snapshot_chainstate.GetCoinsCacheSizeState());

            if (snapshot_cache_state >= CoinsCacheSizeState::CRITICAL) {
                // This is a hack - we don't know what the actual best block is, but that
                // doesn't matter for the purposes of flushing the cache here. We'll set this
                // to its correct value (`base_blockhash`) below after the coins are loaded.
                coins_cache.SetBestBlock(GetRandHash());

                // No need to acquire cs_main since this chainstate isn't being used yet.
                FlushSnapshotToDisk(coins_cache, /*snapshot_loaded=*/false);
            }
        }
    }

    // Important that we set this. This and the coins_cache accesses above are
    // sort of a layer violation, but either we reach into the innards of
    // CCoinsViewCache here or we have to invert some of the Chainstate to
    // embed them in a snapshot-activation-specific CCoinsViewCache bulk load
    // method.
    coins_cache.SetBestBlock(base_blockhash);

    std::optional<evo::CEvoSnapshot> evo_snapshot;
    uint64_t evo_marker{0};
    try {
        coins_file >> evo_marker;
    } catch (const std::ios_base::failure&) {
        if (DeploymentActiveAt(*snapshot_start_block, GetConsensus(), Consensus::DEPLOYMENT_DIP0003)) {
            LogPrintf("[snapshot] missing evo section at DIP3-active base\n");
            return false;
        }
    }
    if (evo_marker != 0) {
        if (evo_marker != evo::EVO_SNAPSHOT_MARKER) {
            LogPrintf("[snapshot] bad evo section marker (or coins left over) after %d coins\n", coins_count);
            return false;
        }
        try {
            evo_snapshot.emplace();
            OverrideStream<AutoFile> evo_file{&coins_file, SER_DISK, CLIENT_VERSION};
            evo_file >> *evo_snapshot;
        } catch (const std::ios_base::failure&) {
            LogPrintf("[snapshot] truncated or invalid evo section\n");
            return false;
        }
        try {
            uint8_t trailing;
            coins_file >> trailing;
            LogPrintf("[snapshot] trailing data after evo section\n");
            return false;
        } catch (const std::ios_base::failure&) {
            // EOF immediately after a completely decoded CEvoSnapshot is required.
        }
    }

    if (!evo_snapshot && DeploymentActiveAt(*snapshot_start_block, GetConsensus(), Consensus::DEPLOYMENT_DIP0003)) {
        LogPrintf("[snapshot] UTXO-only snapshot refused at DIP3-active base\n");
        return false;
    }

    LogPrintf("[snapshot] loaded %d (%.2f MB) coins from snapshot %s\n",
        coins_count,
        coins_cache.DynamicMemoryUsage() / (1000 * 1000),
        base_blockhash.ToString());

    // No need to acquire cs_main since this chainstate isn't being used yet.
    FlushSnapshotToDisk(coins_cache, /*snapshot_loaded=*/true);

    assert(coins_cache.GetBestBlock() == base_blockhash);

    // As above, okay to immediately release cs_main here since no other context knows
    // about the snapshot_chainstate.
    CCoinsViewDB* snapshot_coinsdb = WITH_LOCK(::cs_main, return &snapshot_chainstate.CoinsDB());

    std::optional<CCoinsStats> maybe_stats;

    try {
        maybe_stats = ComputeUTXOStats(
            CoinStatsHashType::HASH_SERIALIZED, snapshot_coinsdb, m_blockman, SnapshotUTXOHashBreakpoint);
    } catch (StopHashingException const&) {
        return false;
    }
    if (!maybe_stats.has_value()) {
        LogPrintf("[snapshot] failed to generate coins stats\n");
        return false;
    }

    // Assert that the deserialized chainstate contents match the expected assumeutxo value.
    if (AssumeutxoHash{maybe_stats->hashSerialized} != au_data.hash_serialized) {
        LogPrintf("[snapshot] bad snapshot content hash: expected %s, got %s\n",
            au_data.hash_serialized.ToString(), maybe_stats->hashSerialized.ToString());
        return false;
    }

    if (evo_snapshot) {
        std::string evo_error;
        {
            LOCK(::cs_main);
            if (!evo::ValidateEvoSnapshotAgainstChain(*evo_snapshot, *this, snapshot_start_block, evo_error)) {
                LogPrintf("[snapshot] bad evo snapshot chain data: %s\n", evo_error);
                return false;
            }
        }
        const uint256 actual_evo_hash{evo::GetEvoSnapshotHash(*evo_snapshot)};
        // Regtest entries use a null hash as an intentional M7 parameter slot.
        // All structural/chain checks and the available CbTx checks still run.
        if (au_data.evo_hash == EvoSnapshotHash{uint256::ZERO} &&
            GetParams().NetworkIDString() != CBaseChainParams::REGTEST) {
            LogPrintf("[snapshot] null evo snapshot hash is only permitted on regtest\n");
            return false;
        }
        if (au_data.evo_hash != EvoSnapshotHash{uint256::ZERO} &&
            EvoSnapshotHash{actual_evo_hash} != au_data.evo_hash) {
            LogPrintf("[snapshot] bad evo snapshot hash: expected %s, got %s\n",
                      au_data.evo_hash.ToString(), actual_evo_hash.ToString());
            return false;
        }

        // CbTx is part of the full base block, not its header. Check it when the
        // block is locally available; otherwise background validation's M3
        // canonical base-state comparison remains the load-time backstop.
        const bool base_block_available{WITH_LOCK(::cs_main, return (snapshot_start_block->nStatus & BLOCK_HAVE_DATA) != 0;)};
        if (DeploymentActiveAt(*snapshot_start_block, GetConsensus(), Consensus::DEPLOYMENT_DIP0003) &&
            base_block_available) {
            CBlock base_block;
            if (!ReadBlockFromDisk(base_block, snapshot_start_block, GetConsensus()) || base_block.vtx.empty()) {
                LogPrintf("[snapshot] failed to read available base block for evo CbTx check\n");
                return false;
            }
            const auto cbtx{GetTxPayload<CCbTx>(*base_block.vtx[0])};
            if (!cbtx || !evo::VerifyEvoSnapshotCbTx(*evo_snapshot, *cbtx, evo_error)) {
                LogPrintf("[snapshot] evo CbTx cross-check failed: %s\n", evo_error);
                return false;
            }
        } else if (DeploymentActiveAt(*snapshot_start_block, GetConsensus(), Consensus::DEPLOYMENT_DIP0003)) {
            LogPrintf("[snapshot] base block data unavailable; deferring evo CbTx cross-check to background validation\n");
        }
    }

    snapshot_chainstate.m_chain.SetTip(*snapshot_start_block);

    // The remainder of this function requires modifying data protected by cs_main.
    LOCK(::cs_main);

    // Fake various pieces of CBlockIndex state:
    CBlockIndex* index = nullptr;

    // Don't make any modifications to the genesis block.
    // This is especially important because we don't want to erroneously
    // apply BLOCK_ASSUMED_VALID to genesis, which would happen if we didn't skip
    // it here (since it apparently isn't BLOCK_VALID_SCRIPTS).
    constexpr int AFTER_GENESIS_START{1};

    for (int i = AFTER_GENESIS_START; i <= snapshot_chainstate.m_chain.Height(); ++i) {
        index = snapshot_chainstate.m_chain[i];

        // Fake nTx so that LoadBlockIndex() loads assumed-valid CBlockIndex
        // entries (among other things)
        if (!index->nTx) {
            index->nTx = 1;
        }
        // Fake nChainTx so that GuessVerificationProgress reports accurately
        index->nChainTx = index->pprev->nChainTx + index->nTx;

        // Mark unvalidated block index entries beneath the snapshot base block as assumed-valid.
        if (!index->IsValid(BLOCK_VALID_SCRIPTS)) {
            // This flag will be removed once the block is fully validated by a
            // background chainstate.
            index->nStatus |= BLOCK_ASSUMED_VALID;
        }

        m_blockman.m_dirty_blockindex.insert(index);
        // Changes to the block index will be flushed to disk after this call
        // returns in `ActivateSnapshot()`, when `MaybeRebalanceCaches()` is
        // called, since we've added a snapshot chainstate and therefore will
        // have to downsize the IBD chainstate, which will result in a call to
        // `FlushStateToDisk(ALWAYS)`.
    }

    assert(index);
    index->nChainTx = au_data.nChainTx;
    snapshot_chainstate.setBlockIndexCandidates.insert(snapshot_start_block);

    // Before DIP3, the snapshot has no evo payload. Capture the base list only
    // when the background chain has independently reached it; a cold-start
    // lookup would otherwise fabricate and cache an empty initial list.
    std::optional<uint256> base_mn_list_hash;
    if (const CBlockIndex* ibd_tip = m_ibd_chainstate->m_chain.Tip();
        ibd_tip != nullptr && ibd_tip->GetBlockHash() == base_blockhash) {
        base_mn_list_hash =
            snapshot_chainstate.ChainHelper().GetDeterministicMNListHash(snapshot_start_block);
        auto db_tx = snapshot_chainstate.m_evoDb.BeginTransaction(::EvoDbIdentity::NORMAL);
        snapshot_chainstate.m_evoDb.WriteBackgroundMNListHash(base_blockhash, *base_mn_list_hash);
        db_tx->Commit();
    }

    // Snapshot lifecycle recovery depends on the background chainstate's
    // independently captured MN-list hash. Make all preceding NORMAL writes
    // durable before publishing the snapshot markers.
    if (!snapshot_chainstate.m_evoDb.CommitRootTransaction(EvoDbIdentity::NORMAL, /*sync=*/true)) {
        LogPrintf("[snapshot] failed to sync background EvoDB state\n");
        return false;
    }
    std::vector<uint256> required_work_blocks;
    if (evo_snapshot) {
        required_work_blocks.reserve(evo_snapshot->historical_mn_list_diffs.size());
        for (const auto& entry : evo_snapshot->historical_mn_list_diffs) {
            required_work_blocks.emplace_back(entry.block_hash);
        }
        std::sort(required_work_blocks.begin(), required_work_blocks.end());
    }
    // Usually the background chain has not reached these blocks yet. If it
    // has, hash its already-connected ordinary state now, before any snapshot
    // seeds enter the shared EvoDB namespace.
    std::map<uint256, uint256> existing_background_work_hashes;
    auto& background_dmnman{m_ibd_chainstate->ChainHelper().DeterministicMNManager()};
    for (const auto& block_hash : required_work_blocks) {
        const CBlockIndex* index{m_blockman.LookupBlockIndex(block_hash)};
        assert(index != nullptr);
        if (m_ibd_chainstate->m_chain.Contains(index)) {
            existing_background_work_hashes.emplace(
                block_hash, evo::CanonicalMNListHash(background_dmnman.GetListForBlock(index)));
        }
    }
    {
        auto db_tx = snapshot_chainstate.m_evoDb.BeginTransaction(EvoDbIdentity::SNAPSHOT);
        if (evo_snapshot) {
            auto& helper{snapshot_chainstate.ChainHelper()};
            auto& dmnman{helper.DeterministicMNManager()};
            auto& qblockman{helper.QuorumBlockProcessor()};
            auto& qsnapman{helper.QuorumSnapshotManager()};
            if (!dmnman.SeedListForBlock(evo_snapshot->mn_list)) {
                LogPrintf("[snapshot] failed to seed base deterministic MN list\n");
                return false;
            }
            std::map<uint256, CDeterministicMNList> historical_lists;
            std::string reconstruction_error;
            if (!evo::ReconstructHistoricalMNLists(*evo_snapshot, historical_lists, reconstruction_error)) {
                LogPrintf("[snapshot] failed to reconstruct historical deterministic MN lists: %s\n",
                          reconstruction_error);
                return false;
            }
            for (const auto& [_, historical_list] : historical_lists) {
                if (!dmnman.SeedListForBlock(historical_list)) {
                    LogPrintf("[snapshot] failed to seed historical deterministic MN list\n");
                    return false;
                }
            }
            for (const auto& modifier : evo_snapshot->quorum_modifiers) {
                if (!qsnapman.SeedQuorumModifier(modifier.llmq_type, modifier.work_block_hash,
                                                  modifier.modifier)) {
                    LogPrintf("[snapshot] failed to seed quorum score modifier\n");
                    return false;
                }
            }
            for (const auto& quorum_data : evo_snapshot->quorums) {
                const auto seed_commitments = [&](const auto& commitments) {
                    LOCK(::cs_main);
                    for (const auto& entry : commitments) {
                        if (!qblockman.SeedMinedCommitment(quorum_data.llmq_type, entry.quorum_base_block_hash,
                                                          entry.commitment, entry.mined_block_hash)) return false;
                    }
                    return true;
                };
                if (!seed_commitments(quorum_data.active_commitments) ||
                    !seed_commitments(quorum_data.safety_commitments)) {
                    LogPrintf("[snapshot] failed to seed mined quorum commitment\n");
                    return false;
                }
                for (const auto& rotation : quorum_data.rotation_snapshots) {
                    const CBlockIndex* cycle_index{m_blockman.LookupBlockIndex(rotation.cycle_base_block_hash)};
                    assert(cycle_index != nullptr);
                    if (!qsnapman.SeedSnapshotForBlock(quorum_data.llmq_type, cycle_index, rotation.snapshot)) {
                        LogPrintf("[snapshot] failed to seed quorum rotation snapshot\n");
                        return false;
                    }
                }
            }
            if (!helper.credit_pool_manager->SeedSnapshot(snapshot_start_block, evo_snapshot->credit_pool) ||
                !helper.ehf_manager->SeedSignals(snapshot_start_block, evo_snapshot->mnhf_signals)) {
                LogPrintf("[snapshot] failed to seed credit-pool/MNHF state\n");
                return false;
            }
            if (!snapshot_chainstate.m_evoDb.WriteDerived(EVODB_SNAPSHOT_EVO_SECTION, *evo_snapshot)) {
                LogPrintf("[snapshot] failed to retain evo section for deferred CbTx validation\n");
                return false;
            }
        }
        snapshot_chainstate.m_evoDb.WriteBestBlock(EvoDbIdentity::SNAPSHOT, base_blockhash);
        if (evo_snapshot) {
            snapshot_chainstate.m_evoDb.WriteSnapshotBaseMNListHash(
                evo::CanonicalMNListHash(evo_snapshot->mn_list));
        } else if (base_mn_list_hash.has_value()) {
            snapshot_chainstate.m_evoDb.WriteSnapshotBaseMNListHash(*base_mn_list_hash);
        }
        snapshot_chainstate.m_evoDb.WriteDualChainstateMarker();
        db_tx->Commit();
    }
    {
        // This bounded required set and its independently computed captures
        // belong to the NORMAL identity. Future background connects consult
        // the in-memory mirror before recording their canonical hash.
        auto db_tx = snapshot_chainstate.m_evoDb.BeginTransaction(EvoDbIdentity::NORMAL);
        snapshot_chainstate.m_evoDb.WriteRequiredWorkMNListHashes(required_work_blocks);
        for (const auto& [block_hash, mn_list_hash] : existing_background_work_hashes) {
            snapshot_chainstate.m_evoDb.WriteBackgroundWorkMNListHash(block_hash, mn_list_hash);
        }
        db_tx->Commit();
    }
    if (!snapshot_chainstate.m_evoDb.CommitRootTransaction(EvoDbIdentity::NORMAL, /*sync=*/true)) {
        LogPrintf("[snapshot] failed to commit required historical MN-list marker\n");
        return false;
    }
    m_ibd_chainstate->SetRequiredBackgroundMNListHashes(required_work_blocks);
    if (!snapshot_chainstate.m_evoDb.CommitRootTransaction(EvoDbIdentity::SNAPSHOT, /*sync=*/true)) {
        LogPrintf("[snapshot] failed to commit snapshot EvoDB marker\n");
        return false;
    }
    if (evo_snapshot) {
        auto& helper{snapshot_chainstate.ChainHelper()};
        auto& dmnman{helper.DeterministicMNManager()};
        dmnman.InvalidateListCacheForBlock(base_blockhash);
        for (const auto& historical : evo_snapshot->historical_mn_list_diffs) {
            dmnman.InvalidateListCacheForBlock(historical.block_hash);
        }
        for (const auto& quorum_data : evo_snapshot->quorums) {
            for (const auto& rotation : quorum_data.rotation_snapshots) {
                helper.QuorumSnapshotManager().InvalidateSnapshotCacheForBlock(
                    quorum_data.llmq_type, rotation.cycle_base_block_hash);
            }
        }
    }

    LogPrintf("[snapshot] validated snapshot (%.2f MB)\n",
        coins_cache.DynamicMemoryUsage() / (1000 * 1000));
    return true;
}

SnapshotCompletionResult ChainstateManager::MaybeCompleteSnapshotValidation(
      std::function<void(bilingual_str)> shutdown_fnc)
{
    AssertLockHeld(cs_main);
    if (m_ibd_chainstate.get() == &this->ActiveChainstate() ||
            !this->IsUsable(m_snapshot_chainstate.get()) ||
            !this->IsUsable(m_ibd_chainstate.get()) ||
            !m_ibd_chainstate->m_chain.Tip()) {
       // Nothing to do - this function only applies to the background
       // validation chainstate.
       return SnapshotCompletionResult::SKIPPED;
    }
    const auto snapshot_base_height_opt = this->GetSnapshotBaseHeight();
    if (!snapshot_base_height_opt) {
        if (!m_snapshot_chainstate->CoinsDB().StoragePath()) {
            // Some Dash unit fixtures construct a synthetic in-memory snapshot
            // chainstate before inserting its base block into the block index.
            return SnapshotCompletionResult::SKIPPED;
        }
        LogPrintf("[snapshot] on-disk snapshot base block is missing from the block index\n");
        return SnapshotCompletionResult::BASE_BLOCKHASH_MISMATCH;
    }
    const int snapshot_base_height = *snapshot_base_height_opt;
    const CBlockIndex& index_new = *Assert(m_ibd_chainstate->m_chain.Tip());

    if (index_new.nHeight < snapshot_base_height) {
        // Background IBD not complete yet.
        return SnapshotCompletionResult::SKIPPED;
    }

    assert(SnapshotBlockhash());
    uint256 snapshot_blockhash = *Assert(SnapshotBlockhash());

    // Completion is serialized by cs_main. Flush each identity in sequence so
    // CEvoDB's single-open-transaction invariant is preserved and marker
    // promotion can atomically operate on fully committed transaction trees.
    m_ibd_chainstate->ForceFlushStateToDisk();
    m_snapshot_chainstate->ForceFlushStateToDisk();
    if (!m_ibd_chainstate->m_evoDb.CommitRootTransaction(EvoDbIdentity::NORMAL, /*sync=*/true) ||
        !m_snapshot_chainstate->m_evoDb.CommitRootTransaction(EvoDbIdentity::SNAPSHOT, /*sync=*/true)) {
        // ConnectTip discards this result and would not retry after the
        // background tip reached the base, so treat the write failure as the
        // unrecoverable database error it is.
        AbortNode("Failed to sync EvoDB state for snapshot completion");
        return SnapshotCompletionResult::STATS_FAILED;
    }

    auto handle_invalid_snapshot = [&](const std::string& reason = "snapshot completion mismatch")
        EXCLUSIVE_LOCKS_REQUIRED(::cs_main) {
        const bool handled{HandleSnapshotStateMismatch(reason, shutdown_fnc)};
        assert(handled);
    };

    if (index_new.GetBlockHash() != snapshot_blockhash) {
        LogPrintf("[snapshot] supposed base block %s does not match the " /* Continued */
          "snapshot base block %s (height %d). Snapshot is not valid.",
          index_new.ToString(), snapshot_blockhash.ToString(), snapshot_base_height);
        handle_invalid_snapshot();
        return SnapshotCompletionResult::BASE_BLOCKHASH_MISMATCH;
    }

    assert(index_new.nHeight == snapshot_base_height);

    int curr_height = m_ibd_chainstate->m_chain.Height();

    assert(snapshot_base_height == curr_height);
    assert(this->IsUsable(m_snapshot_chainstate.get()));
    assert(this->GetAll().size() == 2);

    CCoinsViewDB& ibd_coins_db = m_ibd_chainstate->CoinsDB();

    auto maybe_au_data = ExpectedAssumeutxo(curr_height, ::Params());
    if (!maybe_au_data) {
        LogPrintf("[snapshot] assumeutxo data not found for height " /* Continued */
            "(%d) - refusing to validate snapshot\n", curr_height);
        handle_invalid_snapshot();
        return SnapshotCompletionResult::MISSING_CHAINPARAMS;
    }

    const AssumeutxoData& au_data = *maybe_au_data;
    std::optional<CCoinsStats> maybe_ibd_stats;
    LogPrintf("[snapshot] computing UTXO stats for background chainstate to validate " /* Continued */
        "snapshot - this could take a few minutes\n");
    try {
        maybe_ibd_stats = ComputeUTXOStats(
            CoinStatsHashType::HASH_SERIALIZED,
            &ibd_coins_db,
            m_blockman,
            SnapshotUTXOHashBreakpoint);
    } catch (StopHashingException const&) {
        return SnapshotCompletionResult::STATS_FAILED;
    }

    // XXX note that this function is slow and will hold cs_main for potentially minutes.
    if (!maybe_ibd_stats) {
        LogPrintf("[snapshot] failed to generate stats for validation coins db\n");
        // While this isn't a problem with the snapshot per se, this condition
        // prevents us from validating the snapshot, so we should shut down and let the
        // user handle the issue manually.
        handle_invalid_snapshot();
        return SnapshotCompletionResult::STATS_FAILED;
    }
    const auto& ibd_stats = *maybe_ibd_stats;

    // Compare the background validation chainstate's UTXO set hash against the hard-coded
    // assumeutxo hash we expect.
    //
    // TODO: For belt-and-suspenders, we could cache the UTXO set
    // hash for the snapshot when it's loaded in its chainstate's leveldb. We could then
    // reference that here for an additional check.
    if (AssumeutxoHash{ibd_stats.hashSerialized} != au_data.hash_serialized) {
        LogPrintf("[snapshot] hash mismatch: actual=%s, expected=%s\n",
            ibd_stats.hashSerialized.ToString(),
            au_data.hash_serialized.ToString());
        handle_invalid_snapshot();
        return SnapshotCompletionResult::HASH_MISMATCH;
    }

    // The base block is necessarily available after background validation
    // reaches it. The assumeutxo prune lock is held until this check completes,
    // so the shared BlockManager cannot prune the base out from under the
    // snapshot chainstate. Complete any CbTx checks deferred at snapshot load.
    assert(index_new.nStatus & BLOCK_HAVE_DATA);
    if (DeploymentActiveAt(index_new, GetConsensus(), Consensus::DEPLOYMENT_DIP0003)) {
        evo::CEvoSnapshot retained_snapshot;
        CBlock base_block;
        std::string evo_error;
        if (!m_ibd_chainstate->m_evoDb.Read(EVODB_SNAPSHOT_EVO_SECTION, retained_snapshot) ||
            !ReadBlockFromDisk(base_block, &index_new, GetConsensus()) || base_block.vtx.empty()) {
            LogPrintf("[snapshot] missing retained evo section/base block at completion\n");
            handle_invalid_snapshot();
            return SnapshotCompletionResult::EVO_STATE_MISMATCH;
        }
        const auto cbtx{GetTxPayload<CCbTx>(*base_block.vtx[0])};
        std::map<uint256, CDeterministicMNList> reconstructed_history;
        bool history_matches{evo::ReconstructHistoricalMNLists(retained_snapshot, reconstructed_history, evo_error)};
        if (history_matches) {
            for (const auto& [block_hash, reconstructed_list] : reconstructed_history) {
                uint256 background_hash;
                if (!m_ibd_chainstate->m_evoDb.ReadBackgroundWorkMNListHash(block_hash, background_hash) ||
                    background_hash != evo::CanonicalMNListHash(reconstructed_list)) {
                    evo_error = "missing or mismatched background historical MN-list capture";
                    history_matches = false;
                    break;
                }
            }
        }
        if (!history_matches ||
            !evo::ValidateEvoSnapshotAgainstChain(retained_snapshot, *this, &index_new, evo_error) ||
            !cbtx || !evo::VerifyEvoSnapshotCbTx(retained_snapshot, *cbtx, evo_error)) {
            LogPrintf("[snapshot] deferred evo CbTx cross-check failed: %s\n", evo_error);
            handle_invalid_snapshot();
            return SnapshotCompletionResult::EVO_STATE_MISMATCH;
        }
    }

    // The snapshot marker records the derived deterministic-MN state that was
    // available when the snapshot chainstate began using the base block. Compare
    // it with the state independently derived by background validation.
    uint256 snapshot_mn_list_hash;
    if (!m_ibd_chainstate->m_evoDb.ReadSnapshotBaseMNListHash(snapshot_mn_list_hash)) {
        if (DeploymentActiveAt(index_new, GetConsensus(), Consensus::DEPLOYMENT_DIP0003)) {
            LogPrintf("[snapshot] missing deterministic MN-list marker for evo snapshot\n");
            handle_invalid_snapshot("missing deterministic MN-list marker");
            return SnapshotCompletionResult::EVO_STATE_MISMATCH;
        }
        // A pre-DIP3 cold-start snapshot has no evo payload from which to
        // derive this marker. Its UTXO-set hash remains the completion check.
        LogPrintf("[snapshot] no pre-DIP3 MN-list marker was captured at activation; skipping deterministic MN-list comparison\n");
    } else {
        uint256 background_mn_list_block;
        uint256 background_mn_list_hash;
        if (!m_ibd_chainstate->m_evoDb.ReadBackgroundMNListHash(
                background_mn_list_block, background_mn_list_hash) ||
            background_mn_list_block != snapshot_blockhash ||
            snapshot_mn_list_hash != background_mn_list_hash) {
            LogPrintf("[snapshot] deterministic MN list mismatch at base block: captured_block=%s, actual=%s, expected=%s\n",
                      background_mn_list_block.ToString(), background_mn_list_hash.ToString(),
                      snapshot_mn_list_hash.ToString());
            handle_invalid_snapshot();
            return SnapshotCompletionResult::EVO_STATE_MISMATCH;
        }
    }

    const uint256 snapshot_tip = m_snapshot_chainstate->CoinsTip().GetBestBlock();
    if (!m_ibd_chainstate->m_evoDb.VerifyBestBlock(EvoDbIdentity::NORMAL, snapshot_blockhash) ||
        !m_snapshot_chainstate->m_evoDb.VerifyBestBlock(EvoDbIdentity::SNAPSHOT, snapshot_tip)) {
        LogPrintf("[snapshot] EvoDB best-block markers do not match their chainstate tips\n");
        handle_invalid_snapshot();
        return SnapshotCompletionResult::EVO_STATE_MISMATCH;
    }

    LogPrintf("[snapshot] snapshot beginning at %s has been fully validated\n",
        snapshot_blockhash.ToString());

    m_ibd_chainstate->m_disabled = true;
    ReleaseSnapshotPruneLock();
    this->MaybeRebalanceCaches();

    return SnapshotCompletionResult::SUCCESS;
}
