// Copyright (c) 2011-2022 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <node/blockstorage.h>

#include <arith_uint256.h>
#include <chain.h>
#include <consensus/consensus.h>
#include <consensus/validation.h>
#include <dbwrapper.h>
#include <flatfile.h>
#include <hash.h>
#include <index/addressindex_types.h>
#include <index/spentindex_types.h>
#include <index/timestampindex_types.h>
#include <kernel/chainparams.h>
#include <kernel/messagestartchars.h>
#include <kernel/notifications_interface.h>
#include <logging.h>
#include <pow.h>
#include <shutdown.h>
#include <streams.h>
#include <sync.h>
#include <txdb.h>
#include <undo.h>
#include <util/batchpriority.h>
#include <util/check.h>
#include <util/fs.h>
#include <util/fs_helpers.h>
#include <util/signalinterrupt.h>
#include <util/translation.h>
#include <util/zstd.h>
#include <validation.h>
#include <walletinitinterface.h>

#include <map>
#include <optional>
#include <ranges>
#include <tuple>
#include <unordered_map>

namespace kernel {
static constexpr uint8_t DB_BLOCK_FILES{'f'};
static constexpr uint8_t DB_BLOCK_INDEX{'b'};
static constexpr uint8_t DB_FLAG{'F'};
static constexpr uint8_t DB_REINDEX_FLAG{'R'};
static constexpr uint8_t DB_LAST_BLOCK{'l'};
static constexpr uint8_t DB_BEST_BLOCK{'B'};
// Keys used in previous version that might still be found in the DB:
// BlockTreeDB::DB_TXINDEX_BLOCK{'T'};
// BlockTreeDB::DB_TXINDEX{'t'}
// BlockTreeDB::ReadFlag("txindex")

// Old synchronous index keys (deprecated):
static constexpr uint8_t DB_ADDRESSINDEX{'a'};
static constexpr uint8_t DB_ADDRESSUNSPENTINDEX{'u'};
static constexpr uint8_t DB_SPENTINDEX{'p'};
static constexpr uint8_t DB_TIMESTAMPINDEX{'s'};

bool BlockTreeDB::ReadBlockFileInfo(int nFile, CBlockFileInfo& info)
{
    return Read(std::make_pair(DB_BLOCK_FILES, nFile), info);
}

bool BlockTreeDB::WriteReindexing(bool fReindexing)
{
    if (fReindexing) {
        return Write(DB_REINDEX_FLAG, uint8_t{'1'});
    } else {
        return Erase(DB_REINDEX_FLAG);
    }
}

void BlockTreeDB::ReadReindexing(bool& fReindexing)
{
    fReindexing = Exists(DB_REINDEX_FLAG);
}

bool BlockTreeDB::ReadLastBlockFile(int& nFile)
{
    return Read(DB_LAST_BLOCK, nFile);
}

bool BlockTreeDB::WriteBatchSync(const std::vector<std::pair<int, const CBlockFileInfo*>>& fileInfo, int nLastFile, const std::vector<const CBlockIndex*>& blockinfo)
{
    CDBBatch batch(*this);
    for (const auto& [file, info] : fileInfo) {
        batch.Write(std::make_pair(DB_BLOCK_FILES, file), *info);
    }
    batch.Write(DB_LAST_BLOCK, nLastFile);
    for (const CBlockIndex* bi : blockinfo) {
        batch.Write(std::make_pair(DB_BLOCK_INDEX, bi->GetBlockHash()), CDiskBlockIndex{bi});
    }
    return WriteBatch(batch, true);
}

bool BlockTreeDB::WriteFlag(const std::string& name, bool fValue)
{
    return Write(std::make_pair(DB_FLAG, name), fValue ? uint8_t{'1'} : uint8_t{'0'});
}

bool BlockTreeDB::ReadFlag(const std::string& name, bool& fValue)
{
    uint8_t ch;
    if (!Read(std::make_pair(DB_FLAG, name), ch)) {
        return false;
    }
    fValue = ch == uint8_t{'1'};
    return true;
}

bool BlockTreeDB::LoadBlockIndexGuts(const Consensus::Params& consensusParams, std::function<CBlockIndex*(const uint256&)> insertBlockIndex, const util::SignalInterrupt& interrupt)
{
    AssertLockHeld(::cs_main);
    std::unique_ptr<CDBIterator> pcursor(NewIterator());
    pcursor->Seek(std::make_pair(DB_BLOCK_INDEX, uint256()));

    // Load m_block_index
    while (pcursor->Valid()) {
        if (interrupt) return false;
        std::pair<uint8_t, uint256> key;
        if (pcursor->GetKey(key) && key.first == DB_BLOCK_INDEX) {
            CDiskBlockIndex diskindex;
            if (pcursor->GetValue(diskindex)) {
                // Construct block index object
                CBlockIndex* pindexNew = insertBlockIndex(diskindex.ConstructBlockHash());
                pindexNew->pprev          = insertBlockIndex(diskindex.hashPrev);
                pindexNew->nHeight        = diskindex.nHeight;
                pindexNew->nFile          = diskindex.nFile;
                pindexNew->nDataPos       = diskindex.nDataPos;
                pindexNew->nUndoPos       = diskindex.nUndoPos;
                pindexNew->nVersion       = diskindex.nVersion;
                pindexNew->hashMerkleRoot = diskindex.hashMerkleRoot;
                pindexNew->nTime          = diskindex.nTime;
                pindexNew->nBits          = diskindex.nBits;
                pindexNew->nNonce         = diskindex.nNonce;
                pindexNew->nStatus        = diskindex.nStatus;
                pindexNew->nTx            = diskindex.nTx;

                if (!CheckProofOfWork(pindexNew->GetBlockHash(), pindexNew->nBits, consensusParams)) {
                    LogError("%s: CheckProofOfWork failed: %s\n", __func__, pindexNew->ToString());
                    return false;
                }

                pcursor->Next();
            } else {
                LogError("%s: failed to read value\n", __func__);
                return false;
            }
        } else {
            break;
        }
    }

    return true;
}


/**
 * Template helper to migrate a single index type from the old block index database
 * to a new async index database.
 *
 * @tparam DbKey         The key prefix byte for this index type
 * @tparam KeyType       The index key struct type
 * @tparam ValueType     The index value type
 * @param source_db      The old database to migrate from (BlockTreeDB)
 * @param target_db      The new database to migrate to, or nullptr to discard
 *                       source entries without copying them (for stale legacy data)
 * @param index_name     Human-readable name for logging
 * @param batch_size     Maximum batch size before flushing
 * @return               Number of source entries processed, -1 on error,
 *                       or -2 if shutdown was requested mid-migration
 */
template <uint8_t DbKey, typename KeyType, typename ValueType>
static int64_t MigrateIndex(BlockTreeDB& source_db, CDBWrapper* target_db,
                            const char* index_name, size_t batch_size)
{
    using KeyPair = std::pair<uint8_t, KeyType>;

    size_t count = 0;
    std::optional<CDBBatch> new_batch;
    if (target_db) new_batch.emplace(*target_db);
    CDBBatch erase_batch(source_db);

    KeyPair start = std::make_pair(DbKey, KeyType());
    KeyPair key;
    ValueType value;

    // Compact after erasing this much data to reclaim disk space
    const size_t compact_threshold = 256 << 20; // 256 MiB
    size_t erased_since_compact = 0;

    std::unique_ptr<CDBIterator> pcursor(source_db.NewIterator());
    pcursor->Seek(start);

    while (pcursor->Valid()) {
        // Allow the user to abort a long-running migration without leaving
        // the source/target DBs in a half-applied state.
        if (ShutdownRequested()) {
            LogPrintf("Shutdown requested during %s migration, aborting before next batch\n", index_name);
            return -2;
        }
        if (pcursor->GetKey(key) && key.first == DbKey) {
            if (target_db) {
                if (!pcursor->GetValue(value)) {
                    LogPrintf("Failed to read %s value\n", index_name);
                    return -1;
                }
                new_batch->Write(key, value);
            }
            erase_batch.Erase(key);
            count++;
            pcursor->Next();

            const size_t estimated = new_batch ? new_batch->SizeEstimate() : erase_batch.SizeEstimate();
            if (estimated > batch_size) {
                LogPrintf("Processing partial batch of %s entries (%.2f MiB, %d entries)...\n",
                         index_name, estimated * (1.0 / 1048576.0), count);
                erased_since_compact += erase_batch.SizeEstimate();
                // Sync target writes before erasing source so a crash cannot leave
                // entries erased from source without durably reaching target.
                if (target_db && !target_db->WriteBatch(*new_batch, /*fSync=*/true)) {
                    LogPrintf("Failed to write %s batch to new database\n", index_name);
                    return -1;
                }
                if (!source_db.WriteBatch(erase_batch)) {
                    LogPrintf("Failed to erase old %s data\n", index_name);
                    return -1;
                }
                if (new_batch) new_batch->Clear();
                erase_batch.Clear();

                // Compact periodically to reclaim disk space
                if (erased_since_compact >= compact_threshold) {
                    // Close iterator before compaction so LevelDB can delete old SST files
                    pcursor.reset();
                    source_db.CompactRange(start, key);
                    erased_since_compact = 0;

                    // Reopen iterator - seek to start finds next unprocessed key since we erased previous ones
                    pcursor.reset(source_db.NewIterator());
                    pcursor->Seek(start);
                }
            }
        } else {
            break;
        }
    }

    // Always write final batch with sync to ensure durability
    if (target_db && !target_db->WriteBatch(*new_batch, /*fSync=*/true)) {
        LogPrintf("Failed to write final %s batch\n", index_name);
        return -1;
    }
    if (!source_db.WriteBatch(erase_batch, true)) {
        LogPrintf("Failed to erase final %s batch\n", index_name);
        return -1;
    }
    if (new_batch) new_batch->Clear();
    erase_batch.Clear();

    // Close iterator before final compaction
    pcursor.reset();

    // Compact final range if we processed any keys
    if (count > 0) {
        source_db.CompactRange(start, key);
    }

    return static_cast<int64_t>(count);
}

/**
 * Write best block locator to an index database.
 * Note: Source database compaction is done incrementally in MigrateIndex.
 */
static bool FinalizeMigration(CDBWrapper& target_db, const uint256& best_block_hash,
                              const char* index_name)
{
    if (!best_block_hash.IsNull()) {
        CBlockLocator locator;
        locator.vHave.push_back(best_block_hash);
        CDBBatch best_block_batch(target_db);
        best_block_batch.Write(DB_BEST_BLOCK, locator);
        if (!target_db.WriteBatch(best_block_batch, true)) {
            LogError("%s: Failed to write best block for %s\n", __func__, index_name);
            return false;
        }
        LogPrintf("Set %s best block to %s\n", index_name, best_block_hash.ToString());
    }

    return true;
}

// Returns true if the target index database holds migrated entries but no best-block
// locator yet, i.e. a migration copied data that was never finalized. Used to guard
// FinalizeMigration so we don't regress an already-advanced locator (e.g. left by
// ThreadSync after a previous successful migration), and so we never stamp the chain
// tip onto an empty target (e.g. index directories deleted after an earlier migration
// already moved the legacy entries), which would report an empty index as synced.
static bool TargetNeedsLocator(CDBWrapper& target_db)
{
    CBlockLocator existing;
    if (target_db.Read(DB_BEST_BLOCK, existing) && !existing.IsNull()) return false;
    return !target_db.IsEmpty();
}

bool BlockTreeDB::MigrateOldIndexData(const fs::path& data_dir, const IndexMigrationOptions& options)
{
    // Migrate old synchronous index data that was stored in the block index database
    // to new async indexes in separate databases under indexes/{timestampindex,spentindex,addressindex}/
    // This preserves existing index data so users don't need to rebuild.
    // Indexes are migrated independently since they can be enabled individually.

    // Only migrate indexes that are actually enabled
    const bool fTimestampIndex = options.timestamp_index;
    const bool fSpentIndex = options.spent_index;
    const bool fAddressIndex = options.address_index;

    // The flags read below say whether the legacy rows still cover the chain tip. Nothing
    // maintains those rows any more, so a disabled index falls behind from this run on:
    // record that now, or a later run that enables the index would adopt the stale rows.
    if (!fTimestampIndex) WriteFlag("timestampindex", false);
    if (!fSpentIndex) WriteFlag("spentindex", false);
    if (!fAddressIndex) WriteFlag("addressindex", false);

    if (!fTimestampIndex && !fSpentIndex && !fAddressIndex) {
        // No indexes enabled, skip migration entirely
        return true;
    }

    LogPrintf("Checking for old index data in block index database...\n");

    // The legacy synchronous index code persisted a flag in the block index DB whenever
    // the user changed -addressindex/-spentindex/-timestampindex, and refused to start
    // with a re-enabled index unless the user reindexed. That re-enable check is gone now,
    // so we use these flags to detect stale on-disk data: if the flag is missing or false,
    // the legacy entries don't cover the suffix of the chain that ran with the index
    // disabled, and copying them while stamping the chainstate tip as the locator would
    // mark an incomplete index as fully synced. In that case we discard the source entries
    // instead and let the new async index resync from genesis. Once an enabled index has been
    // processed its legacy entries are gone, so its flag is cleared: a later start must never
    // adopt whatever the new database then holds (for example a partial rebuild after the
    // index directory was deleted) as migrated data, and older versions see that their
    // synchronous index needs a reindex.
    bool legacy_flag = false;
    const bool addressindex_was_current   = ReadFlag("addressindex",   legacy_flag) && legacy_flag;
    const bool spentindex_was_current     = ReadFlag("spentindex",     legacy_flag) && legacy_flag;
    const bool timestampindex_was_current = ReadFlag("timestampindex", legacy_flag) && legacy_flag;

    const size_t batch_size = options.batch_size;
    size_t total_count = 0;
    const fs::path indexes_path = data_dir / "indexes";

    // Read the best block hash from coins database to set as best block for migrated indexes
    // Old synchronous indexes were updated during ConnectBlock, so they're synced to the active chain tip
    uint256 best_block_hash;
    {
        fs::path chainstate_path = data_dir / "chainstate";
        CDBWrapper coins_db{DBParams{.path = chainstate_path, .cache_bytes = 0}};
        if (!coins_db.Read(DB_BEST_BLOCK, best_block_hash)) {
            // If we can't read the best block, the indexes will resync from scratch
            LogPrintf("Warning: Could not read best block from chainstate, migrated indexes will resync\n");
            best_block_hash.SetNull();
        } else {
            LogPrintf("Migrating indexes with best block: %s\n", best_block_hash.ToString());
        }
    }

    // Returns true if migration of this index step succeeded (count >= 0); on
    // shutdown (-2) and error (-1) returns false, leaving the caller to bail
    // without finalizing — the source DB still has the un-processed remainder
    // so the next start can resume.
    auto handle_count = [](int64_t count, const char* index_name, const char* func_name,
                           bool was_current, size_t& total) -> bool {
        if (count < 0) {
            LogError("%s: Failed to migrate %s\n", func_name, index_name);
            return false;
        }
        if (count > 0) {
            LogPrintf("%s %d %s entries\n",
                      was_current ? "Migrated" : "Discarded stale", count, index_name);
            total += count;
        }
        return true;
    };

    // Migrate timestamp index (only if enabled)
    if (fTimestampIndex) {
        const fs::path db_path = indexes_path / "timestampindex";
        CDBWrapper timestamp_db{DBParams{.path = db_path, .cache_bytes = 0}};

        // Pass nullptr to discard rather than copy if legacy data is stale.
        CDBWrapper* target = timestampindex_was_current ? &timestamp_db : nullptr;
        int64_t count = MigrateIndex<DB_TIMESTAMPINDEX, CTimestampIndexKey, bool>(
            *this, target, "timestamp index", batch_size);
        if (!handle_count(count, "timestamp index", __func__, timestampindex_was_current, total_count)) {
            return false;
        }
        // Finalize even when count==0 to recover from a previous run that crashed
        // after writing target data but before writing the locator. An empty target is
        // left without a locator, so the index rebuilds from genesis.
        if (timestampindex_was_current && TargetNeedsLocator(timestamp_db)) {
            if (!FinalizeMigration(timestamp_db, best_block_hash, "timestamp index")) {
                return false;
            }
        }
        WriteFlag("timestampindex", false);
    }

    // Migrate spent index (only if enabled)
    if (fSpentIndex) {
        const fs::path db_path = indexes_path / "spentindex";
        CDBWrapper spent_db{DBParams{.path = db_path, .cache_bytes = 0}};

        CDBWrapper* target = spentindex_was_current ? &spent_db : nullptr;
        int64_t count = MigrateIndex<DB_SPENTINDEX, CSpentIndexKey, CSpentIndexValue>(
            *this, target, "spent index", batch_size);
        if (!handle_count(count, "spent index", __func__, spentindex_was_current, total_count)) {
            return false;
        }
        if (spentindex_was_current && TargetNeedsLocator(spent_db)) {
            if (!FinalizeMigration(spent_db, best_block_hash, "spent index")) {
                return false;
            }
        }
        WriteFlag("spentindex", false);
    }

    // Migrate address index (includes both address and unspent indexes) (only if enabled)
    if (fAddressIndex) {
        const fs::path db_path = indexes_path / "addressindex";
        CDBWrapper address_db{DBParams{.path = db_path, .cache_bytes = 0}};

        CDBWrapper* target = addressindex_was_current ? &address_db : nullptr;

        // Migrate address index (transaction history)
        int64_t address_count = MigrateIndex<DB_ADDRESSINDEX, CAddressIndexKey, CAmount>(
            *this, target, "address index", batch_size);
        if (!handle_count(address_count, "address index", __func__, addressindex_was_current, total_count)) {
            return false;
        }

        // Migrate address unspent index
        int64_t unspent_count = MigrateIndex<DB_ADDRESSUNSPENTINDEX, CAddressUnspentKey, CAddressUnspentValue>(
            *this, target, "address unspent index", batch_size);
        if (!handle_count(unspent_count, "address unspent index", __func__, addressindex_was_current, total_count)) {
            return false;
        }

        if (addressindex_was_current && TargetNeedsLocator(address_db)) {
            if (!FinalizeMigration(address_db, best_block_hash, "address and address unspent indexes")) {
                return false;
            }
        }
        WriteFlag("addressindex", false);
    }

    if (total_count > 0) {
        LogPrintf("Compacting remaining block index database...\n");
        CompactFull();
        LogPrintf("Successfully processed %d legacy index entries\n", total_count);
    } else {
        LogPrintf("No old index data found\n");
    }

    return true;
}
} // namespace kernel

namespace node {
std::atomic_bool fImporting(false);
std::atomic_bool fReindex(false);

bool CBlockIndexWorkComparator::operator()(const CBlockIndex* pa, const CBlockIndex* pb) const
{
    // First sort by most total work, ...
    if (pa->nChainWork > pb->nChainWork) return false;
    if (pa->nChainWork < pb->nChainWork) return true;

    // ... then by earliest time received, ...
    if (pa->nSequenceId < pb->nSequenceId) return false;
    if (pa->nSequenceId > pb->nSequenceId) return true;

    // Use pointer address as tie breaker (should only happen with blocks
    // loaded from disk, as those all have id 0).
    if (pa < pb) return false;
    if (pa > pb) return true;

    // Identical blocks.
    return false;
}

bool CBlockIndexHeightOnlyComparator::operator()(const CBlockIndex* pa, const CBlockIndex* pb) const
{
    return pa->nHeight < pb->nHeight;
}

static uint32_t ReadRecordSizeField(AutoFile& filein);

std::vector<CBlockIndex*> BlockManager::GetAllBlockIndices()
{
    AssertLockHeld(cs_main);
    std::vector<CBlockIndex*> rv;
    rv.reserve(m_block_index.size());
    for (auto& [_, block_index] : m_block_index) {
        rv.push_back(&block_index);
    }
    return rv;
}

CBlockIndex* BlockManager::LookupBlockIndex(const uint256& hash)
{
    AssertLockHeld(cs_main);
    BlockMap::iterator it = m_block_index.find(hash);
    return it == m_block_index.end() ? nullptr : &it->second;
}

const CBlockIndex* BlockManager::LookupBlockIndex(const uint256& hash) const
{
    AssertLockHeld(cs_main);
    BlockMap::const_iterator it = m_block_index.find(hash);
    return it == m_block_index.end() ? nullptr : &it->second;
}

CBlockIndex* BlockManager::AddToBlockIndex(const CBlockHeader& block, const uint256& hash, CBlockIndex*& best_header,
                                           enum BlockStatus nStatus)
{
    assert(!(nStatus & BLOCK_FAILED_MASK)); // no failed blocks allowed
    AssertLockHeld(cs_main);

    auto [mi, inserted] = m_block_index.try_emplace(hash, block);
    if (!inserted) {
        return &mi->second;
    }
    CBlockIndex* pindexNew = &(*mi).second;

    // We assign the sequence id to blocks only when the full data is available,
    // to avoid miners withholding blocks but broadcasting headers, to get a
    // competitive advantage.
    pindexNew->nSequenceId = 0;

    pindexNew->phashBlock = &((*mi).first);
    BlockMap::iterator miPrev = m_block_index.find(block.hashPrevBlock);
    if (miPrev != m_block_index.end()) {
        pindexNew->pprev = &(*miPrev).second;
        pindexNew->nHeight = pindexNew->pprev->nHeight + 1;
        pindexNew->BuildSkip();
    }
    pindexNew->nTimeMax = (pindexNew->pprev ? std::max(pindexNew->pprev->nTimeMax, pindexNew->nTime) : pindexNew->nTime);
    pindexNew->nChainWork = (pindexNew->pprev ? pindexNew->pprev->nChainWork : 0) + GetBlockProof(*pindexNew);
    if (nStatus & BLOCK_VALID_MASK) {
        pindexNew->RaiseValidity(nStatus);
        if (best_header == nullptr || best_header->nChainWork < pindexNew->nChainWork) {
            best_header = pindexNew;
        }
    } else {
        pindexNew->RaiseValidity(BLOCK_VALID_TREE); // required validity level
        pindexNew->nStatus |= nStatus;
    }

    m_dirty_blockindex.insert(pindexNew);

    // track prevBlockHash -> pindex (multimap)
    if (pindexNew->pprev) {
        m_prev_block_index.emplace(pindexNew->pprev->GetBlockHash(), pindexNew);
    }

    return pindexNew;
}

void BlockManager::PruneOneBlockFile(const int fileNumber)
{
    AssertLockHeld(cs_main);
    LOCK(cs_LastBlockFile);

    for (auto& entry : m_block_index) {
        CBlockIndex* pindex = &entry.second;
        if (pindex->nFile == fileNumber) {
            pindex->nStatus &= ~BLOCK_HAVE_DATA;
            pindex->nStatus &= ~BLOCK_HAVE_UNDO;
            pindex->nFile = 0;
            pindex->nDataPos = 0;
            pindex->nUndoPos = 0;
            m_dirty_blockindex.insert(pindex);

            // Prune from m_blocks_unlinked -- any block we prune would have
            // to be downloaded again in order to consider its chain, at which
            // point it would be considered as a candidate for
            // m_blocks_unlinked or setBlockIndexCandidates.
            auto range = m_blocks_unlinked.equal_range(pindex->pprev);
            while (range.first != range.second) {
                std::multimap<CBlockIndex*, CBlockIndex*>::iterator _it = range.first;
                range.first++;
                if (_it->second == pindex) {
                    m_blocks_unlinked.erase(_it);
                }
            }
        }
    }

    m_blockfile_info.at(fileNumber) = CBlockFileInfo{};
    m_dirty_fileinfo.insert(fileNumber);
}

void BlockManager::FindFilesToPruneManual(std::set<int>& setFilesToPrune, int nManualPruneHeight, int chain_tip_height)
{
    assert(IsPruneMode() && nManualPruneHeight > 0);

    LOCK2(cs_main, cs_LastBlockFile);
    if (chain_tip_height < 0) {
        return;
    }

    // last block to prune is the lesser of (user-specified height, MinBlocksToKeep() from the tip)
    unsigned int nLastBlockWeCanPrune = std::min((unsigned)nManualPruneHeight,
                                                 static_cast<unsigned>(chain_tip_height - GetParams().MinBlocksToKeep()));
    int count = 0;
    for (int fileNumber = 0; fileNumber < m_last_blockfile; fileNumber++) {
        if (m_blockfile_info[fileNumber].nSize == 0 || m_blockfile_info[fileNumber].nHeightLast > nLastBlockWeCanPrune) {
            continue;
        }
        PruneOneBlockFile(fileNumber);
        setFilesToPrune.insert(fileNumber);
        count++;
    }
    LogPrintf("Prune (Manual): prune_height=%d removed %d blk/rev pairs\n", nLastBlockWeCanPrune, count);
}

void BlockManager::FindFilesToPrune(std::set<int>& setFilesToPrune, uint64_t nPruneAfterHeight, int chain_tip_height, int prune_height, bool is_ibd)
{
    LOCK2(cs_main, cs_LastBlockFile);
    if (chain_tip_height < 0 || GetPruneTarget() == 0) {
        return;
    }
    if ((uint64_t)chain_tip_height <= nPruneAfterHeight) {
        return;
    }

    unsigned int nLastBlockWeCanPrune{(unsigned)std::min(prune_height, chain_tip_height - GetParams().MinBlocksToKeep())};
    uint64_t nCurrentUsage = CalculateCurrentUsage();
    // We don't check to prune until after we've allocated new space for files
    // So we should leave a buffer under our target to account for another allocation
    // before the next pruning.
    uint64_t nBuffer = BLOCKFILE_CHUNK_SIZE + UNDOFILE_CHUNK_SIZE;
    uint64_t nBytesToPrune;
    int count = 0;

    if (nCurrentUsage + nBuffer >= GetPruneTarget()) {
        // On a prune event, the chainstate DB is flushed.
        // To avoid excessive prune events negating the benefit of high dbcache
        // values, we should not prune too rapidly.
        // So when pruning in IBD, increase the buffer a bit to avoid a re-prune too soon.
        if (is_ibd) {
            // Since this is only relevant during IBD, we use a fixed 10%
            nBuffer += GetPruneTarget() / 10;
        }

        for (int fileNumber = 0; fileNumber < m_last_blockfile; fileNumber++) {
            nBytesToPrune = m_blockfile_info[fileNumber].nSize + m_blockfile_info[fileNumber].nUndoSize;

            if (m_blockfile_info[fileNumber].nSize == 0) {
                continue;
            }

            if (nCurrentUsage + nBuffer < GetPruneTarget()) { // are we below our target?
                break;
            }

            // don't prune files that could have a block within MinBlocksToKeep() of the main chain's tip but keep scanning
            if (m_blockfile_info[fileNumber].nHeightLast > nLastBlockWeCanPrune) {
                continue;
            }

            PruneOneBlockFile(fileNumber);
            // Queue up the files for removal
            setFilesToPrune.insert(fileNumber);
            nCurrentUsage -= nBytesToPrune;
            count++;
        }
    }

    LogPrint(BCLog::PRUNE, "target=%dMiB actual=%dMiB diff=%dMiB max_prune_height=%d removed %d blk/rev pairs\n",
             GetPruneTarget() / 1024 / 1024, nCurrentUsage / 1024 / 1024,
             (int64_t(GetPruneTarget()) - int64_t(nCurrentUsage)) / 1024 / 1024,
             nLastBlockWeCanPrune, count);
}

void BlockManager::UpdatePruneLock(const std::string& name, const PruneLockInfo& lock_info) {
    AssertLockHeld(::cs_main);
    m_prune_locks[name] = lock_info;
}

CBlockIndex* BlockManager::InsertBlockIndex(const uint256& hash)
{
    AssertLockHeld(cs_main);

    if (hash.IsNull()) {
        return nullptr;
    }

    const auto [mi, inserted]{m_block_index.try_emplace(hash)};
    CBlockIndex* pindex = &(*mi).second;
    if (inserted) {
        pindex->phashBlock = &((*mi).first);
    }
    return pindex;
}

bool BlockManager::LoadBlockIndex()
{
    // Snapshot promotion reloads the block index a second time in the same
    // process (see node::LoadChainstate), and both containers below are rebuilt
    // from m_block_index by this function. They are multimaps, so without
    // clearing them the second load would duplicate every parent/child edge and
    // every unlinked entry for the lifetime of the process.
    m_prev_block_index.clear();
    m_blocks_unlinked.clear();

    if (!m_block_tree_db->LoadBlockIndexGuts(GetConsensus(), [this](const uint256& hash) EXCLUSIVE_LOCKS_REQUIRED(cs_main) { return this->InsertBlockIndex(hash); }, m_interrupt)) {
        return false;
    }

    for (auto& [_, block_index] : m_block_index) {
        // build m_blockman.m_prev_block_index
        if (block_index.pprev) {
            m_prev_block_index.emplace(block_index.pprev->GetBlockHash(), &block_index);
        }
    }

    // Calculate nChainWork
    std::vector<CBlockIndex*> vSortedByHeight{GetAllBlockIndices()};
    std::sort(vSortedByHeight.begin(), vSortedByHeight.end(),
              CBlockIndexHeightOnlyComparator());

    CBlockIndex* previous_index{nullptr};
    for (CBlockIndex* pindex : vSortedByHeight) {
        if (m_interrupt) return false;
        if (previous_index && pindex->nHeight > previous_index->nHeight + 1) {
            LogError("%s: block index is non-contiguous, index of height %d missing\n", __func__, previous_index->nHeight + 1);
            return false;
        }
        previous_index = pindex;
        pindex->nChainWork = (pindex->pprev ? pindex->pprev->nChainWork : 0) + GetBlockProof(*pindex);
        pindex->nTimeMax = (pindex->pprev ? std::max(pindex->pprev->nTimeMax, pindex->nTime) : pindex->nTime);

        // We can link the chain of blocks for which we've received transactions at some point, or
        // blocks that are assumed-valid on the basis of snapshot load (see
        // PopulateAndValidateSnapshot()).
        // Pruned nodes may have deleted the block.
        if (pindex->nTx > 0) {
            if (pindex->pprev) {
                if (pindex->pprev->nChainTx > 0) {
                    pindex->nChainTx = pindex->pprev->nChainTx + pindex->nTx;
                } else {
                    pindex->nChainTx = 0;
                    m_blocks_unlinked.insert(std::make_pair(pindex->pprev, pindex));
                }
            } else {
                pindex->nChainTx = pindex->nTx;
            }
        }
        if (!(pindex->nStatus & BLOCK_FAILED_MASK) && pindex->pprev && (pindex->pprev->nStatus & BLOCK_FAILED_MASK)) {
            pindex->nStatus |= BLOCK_FAILED_CHILD;
            m_dirty_blockindex.insert(pindex);
        }
        if (pindex->pprev) {
            pindex->BuildSkip();
        }
    }

    return true;
}

bool BlockManager::WriteBlockIndexDB()
{
    AssertLockHeld(::cs_main);
    std::vector<std::pair<int, const CBlockFileInfo*>> vFiles;
    vFiles.reserve(m_dirty_fileinfo.size());
    for (std::set<int>::iterator it = m_dirty_fileinfo.begin(); it != m_dirty_fileinfo.end();) {
        vFiles.emplace_back(*it, &m_blockfile_info[*it]);
        m_dirty_fileinfo.erase(it++);
    }
    std::vector<const CBlockIndex*> vBlocks;
    vBlocks.reserve(m_dirty_blockindex.size());
    for (std::set<CBlockIndex*>::iterator it = m_dirty_blockindex.begin(); it != m_dirty_blockindex.end();) {
        vBlocks.push_back(*it);
        m_dirty_blockindex.erase(it++);
    }
    if (m_have_compressed_records && !m_block_tree_db->WriteFlag("compressedrecords", true)) {
        return false;
    }
    if (!m_block_tree_db->WriteBatchSync(vFiles, m_last_blockfile, vBlocks)) {
        return false;
    }
    return true;
}

bool BlockManager::LoadBlockIndexDB()
{
    if (!LoadBlockIndex()) {
        return false;
    }

    // Load block file info
    m_block_tree_db->ReadLastBlockFile(m_last_blockfile);
    m_blockfile_info.resize(m_last_blockfile + 1);
    LogPrintf("%s: last block file = %i\n", __func__, m_last_blockfile);
    for (int nFile = 0; nFile <= m_last_blockfile; nFile++) {
        m_block_tree_db->ReadBlockFileInfo(nFile, m_blockfile_info[nFile]);
    }
    LogPrintf("%s: last block file info: %s\n", __func__, m_blockfile_info[m_last_blockfile].ToString());
    for (int nFile = m_last_blockfile + 1; true; nFile++) {
        CBlockFileInfo info;
        if (m_block_tree_db->ReadBlockFileInfo(nFile, info)) {
            m_blockfile_info.push_back(info);
        } else {
            break;
        }
    }

    // Check presence of blk files
    LogPrintf("Checking all blk files are present...\n");
    std::set<int> setBlkDataFiles;
    for (const auto& [_, block_index] : m_block_index) {
        if (block_index.nStatus & BLOCK_HAVE_DATA) {
            setBlkDataFiles.insert(block_index.nFile);
        }
    }
    for (std::set<int>::iterator it = setBlkDataFiles.begin(); it != setBlkDataFiles.end(); it++) {
        FlatFilePos pos(*it, 0);
        if (OpenBlockFile(pos, true).IsNull()) {
            return false;
        }
    }

    // Check whether we have ever pruned block & undo files
    m_block_tree_db->ReadFlag("prunedblockfiles", m_have_pruned);
    m_block_tree_db->ReadFlag("compressedrecords", m_have_compressed_records);
    if (m_have_pruned) {
        LogPrintf("LoadBlockIndexDB(): Block files have previously been pruned\n");
    }

    // Check whether we need to continue reindexing
    bool fReindexing = false;
    m_block_tree_db->ReadReindexing(fReindexing);
    if (fReindexing) fReindex = true;

    return true;
}

void BlockManager::ScanAndUnlinkAlreadyPrunedFiles()
{
    AssertLockHeld(::cs_main);
    if (!m_have_pruned) {
        return;
    }

    std::set<int> block_files_to_prune;
    for (int file_number = 0; file_number < m_last_blockfile; file_number++) {
        if (m_blockfile_info[file_number].nSize == 0) {
            block_files_to_prune.insert(file_number);
        }
    }

    UnlinkPrunedFiles(block_files_to_prune);
}

const CBlockIndex* BlockManager::GetLastCheckpoint(const CCheckpointData& data)
{
    const MapCheckpoints& checkpoints = data.mapCheckpoints;

    for (const MapCheckpoints::value_type& i : checkpoints | std::views::reverse) {
        const uint256& hash = i.second;
        const CBlockIndex* pindex = LookupBlockIndex(hash);
        if (pindex) {
            return pindex;
        }
    }
    return nullptr;
}

bool BlockManager::IsBlockPruned(const CBlockIndex* pblockindex)
{
    AssertLockHeld(::cs_main);
    return (m_have_pruned && !(pblockindex->nStatus & BLOCK_HAVE_DATA) && pblockindex->nTx > 0);
}

const CBlockIndex* BlockManager::GetFirstBlock(const CBlockIndex& upper_block, uint32_t status_mask, const CBlockIndex* lower_block) const
{
    AssertLockHeld(::cs_main);
    const CBlockIndex* last_block = &upper_block;
    assert((last_block->nStatus & status_mask) == status_mask); // 'upper_block' must satisfy the status mask
    while (last_block->pprev && ((last_block->pprev->nStatus & status_mask) == status_mask)) {
        if (lower_block) {
            // Return if we reached the lower_block
            if (last_block == lower_block) return lower_block;
            // if range was surpassed, means that 'lower_block' is not part of the 'upper_block' chain
            // and so far this is not allowed.
            assert(last_block->nHeight >= lower_block->nHeight);
        }
        last_block = last_block->pprev;
    }
    assert(last_block != nullptr);
    return last_block;
}

bool BlockManager::CheckBlockDataAvailability(const CBlockIndex& upper_block, const CBlockIndex& lower_block)
{
    if (!(upper_block.nStatus & BLOCK_HAVE_DATA)) return false;
    return GetFirstBlock(upper_block, BLOCK_HAVE_DATA, &lower_block) == &lower_block;
}

// If we're using -prune with -reindex, then delete block files that will be ignored by the
// reindex.  Since reindexing works by starting at block file 0 and looping until a blockfile
// is missing, do the same here to delete any later block files after a gap.  Also delete all
// rev files since they'll be rewritten by the reindex anyway.  This ensures that m_blockfile_info
// is in sync with what's actually on disk by the time we start downloading, so that pruning
// works correctly.
void BlockManager::CleanupBlockRevFiles() const
{
    std::map<std::string, fs::path> mapBlockFiles;

    // Glob all blk?????.dat and rev?????.dat files from the blocks directory.
    // Remove the rev files immediately and insert the blk file paths into an
    // ordered map keyed by block file index.
    LogPrintf("Removing unusable blk?????.dat and rev?????.dat files for -reindex with -prune\n");
    for (fs::directory_iterator it(m_opts.blocks_dir); it != fs::directory_iterator(); it++) {
        const std::string path = fs::PathToString(it->path().filename());
        if (fs::is_regular_file(*it) &&
            path.length() == 12 &&
            path.substr(8,4) == ".dat")
        {
            if (path.substr(0, 3) == "blk") {
                mapBlockFiles[path.substr(3, 5)] = it->path();
            } else if (path.substr(0, 3) == "rev") {
                remove(it->path());
            }
        }
    }

    // Remove all block files that aren't part of a contiguous set starting at
    // zero by walking the ordered map (keys are block file indices) by
    // keeping a separate counter.  Once we hit a gap (or if 0 doesn't exist)
    // start removing block files.
    int nContigCounter = 0;
    for (const std::pair<const std::string, fs::path>& item : mapBlockFiles) {
        if (LocaleIndependentAtoi<int>(item.first) == nContigCounter) {
            nContigCounter++;
            continue;
        }
        remove(item.second);
    }
}

CBlockFileInfo* BlockManager::GetBlockFileInfo(size_t n)
{
    LOCK(cs_LastBlockFile);

    return &m_blockfile_info.at(n);
}

bool BlockManager::UndoWriteToDisk(const CBlockUndo& blockundo, const std::optional<std::vector<std::byte>>& compressed, uint32_t size_field, FlatFilePos& pos, const uint256& hashBlock) const
{
    // Open history file to append
    AutoFile fileout{OpenUndoFile(pos)};
    if (fileout.IsNull()) {
        LogError("%s: OpenUndoFile failed\n", __func__);
        return false;
    }

    // Write index header
    fileout << GetParams().MessageStart() << size_field;

    // Write undo data
    const int64_t fileOutPos{FileTell64(fileout.Get())};
    if (fileOutPos < 0) {
        LogError("%s: ftell failed\n", __func__);
        return false;
    }
    pos.nPos = fileOutPos;
    if (compressed) {
        fileout.write(*compressed);
    } else {
        fileout << blockundo;
    }

    // calculate & write checksum over the previous block hash and the uncompressed undo data, whichever way it is stored
    HashWriter hasher{};
    hasher << hashBlock;
    hasher << blockundo;
    fileout << hasher.GetHash();

    return true;
}

bool BlockManager::UndoReadFromDisk(CBlockUndo& blockundo, const CBlockIndex& index) const
{
    const FlatFilePos pos{WITH_LOCK(::cs_main, return index.GetUndoPos())};

    if (pos.IsNull()) {
        LogError("%s: no undo data available\n", __func__);
        return false;
    }

    // Open history file to read
    AutoFile filein{OpenUndoFile(RecordHeaderPos(pos), true)};
    if (filein.IsNull()) {
        LogError("%s: OpenUndoFile failed\n", __func__);
        return false;
    }

    // Read block
    uint256 hashChecksum;
    uint256 computed;
    try {
        const uint32_t size_field{ReadRecordSizeField(filein)};
        const uint32_t payload_size{size_field & BLOCK_RECORD_SIZE_MASK};
        const bool compressed{(size_field & BLOCK_RECORD_COMPRESSED) != 0};
        util::ZstdFileReader undo_in{filein, payload_size, compressed};
        HashVerifier verifier{undo_in}; // Use HashVerifier as reserializing may lose data, c.f. commit d342424301013ec47dc146a4beb49d5c9319d80a
        verifier << index.pprev->GetBlockHash();
        verifier >> blockundo;
        // the decompressor may stop before the end of the frame, the checksum follows the stored payload
        if (compressed && FileSeek64(filein.Get(), pos.nPos + payload_size, SEEK_SET) != 0) {
            throw std::ios_base::failure("fseek to undo checksum failed");
        }
        filein >> hashChecksum;
        computed = verifier.GetHash();
    } catch (const std::exception& e) {
        LogError("%s: Deserialize or I/O error - %s\n", __func__, e.what());
        return false;
    }

    // Verify checksum
    if (hashChecksum != computed) {
        LogError("%s: Checksum mismatch\n", __func__);
        return false;
    }

    return true;
}

bool BlockManager::FlushUndoFile(int block_file, bool finalize)
{
    FlatFilePos undo_pos_old(block_file, m_blockfile_info[block_file].nUndoSize);
    if (!UndoFileSeq().Flush(undo_pos_old, finalize)) {
        m_opts.notifications.flushError("Flushing undo file to disk failed. This is likely the result of an I/O error.");
        return false;
    }
    return true;
}

bool BlockManager::FlushBlockFile(bool fFinalize, bool finalize_undo)
{
    bool success = true;
    LOCK(cs_LastBlockFile);

    if (m_blockfile_info.size() < 1) {
        // Return if we haven't loaded any blockfiles yet. This happens during
        // chainstate init, when we call ChainstateManager::MaybeRebalanceCaches() (which
        // then calls FlushStateToDisk()), resulting in a call to this function before we
        // have populated `m_blockfile_info` via LoadBlockIndexDB().
        return true;
    }
    assert(static_cast<int>(m_blockfile_info.size()) > m_last_blockfile);

    FlatFilePos block_pos_old(m_last_blockfile, m_blockfile_info[m_last_blockfile].nSize);
    if (!BlockFileSeq().Flush(block_pos_old, fFinalize)) {
        m_opts.notifications.flushError("Flushing block file to disk failed. This is likely the result of an I/O error.");
        success = false;
    }
    // we do not always flush the undo file, as the chain tip may be lagging behind the incoming blocks,
    // e.g. during IBD or a sync after a node going offline
    if (!fFinalize || finalize_undo) {
        if (!FlushUndoFile(m_last_blockfile, finalize_undo)) {
            success = false;
        }
    }
    return success;
}

uint64_t BlockManager::CalculateCurrentUsage()
{
    LOCK(cs_LastBlockFile);

    uint64_t retval = 0;
    for (const CBlockFileInfo& file : m_blockfile_info) {
        retval += file.nSize + file.nUndoSize;
    }
    return retval;
}

void BlockManager::UnlinkPrunedFiles(const std::set<int>& setFilesToPrune) const
{
    std::error_code ec;
    for (std::set<int>::iterator it = setFilesToPrune.begin(); it != setFilesToPrune.end(); ++it) {
        FlatFilePos pos(*it, 0);
        const bool removed_blockfile{fs::remove(BlockFileSeq().FileName(pos), ec)};
        const bool removed_undofile{fs::remove(UndoFileSeq().FileName(pos), ec)};
        if (removed_blockfile || removed_undofile) {
            LogPrint(BCLog::BLOCKSTORAGE, "Prune: %s deleted blk/rev (%05u)\n", __func__, *it);
        }
    }
}

FlatFileSeq BlockManager::BlockFileSeq() const
{
    return FlatFileSeq(m_opts.blocks_dir, "blk",
                       m_opts.fast_prune ? 0x4000 /* 16kb */ :
                        (m_opts.tiny_blk ? 0x10000 /* 64kb */ : BLOCKFILE_CHUNK_SIZE));
}

FlatFileSeq BlockManager::UndoFileSeq() const
{
    return FlatFileSeq(m_opts.blocks_dir, "rev", m_opts.tiny_blk ? 0x10000 /* 64kb */ : UNDOFILE_CHUNK_SIZE);
}

AutoFile BlockManager::OpenBlockFile(const FlatFilePos& pos, bool fReadOnly) const
{
    return AutoFile{BlockFileSeq().Open(pos, fReadOnly)};
}

/** Open an undo file (rev?????.dat) */
AutoFile BlockManager::OpenUndoFile(const FlatFilePos& pos, bool fReadOnly) const
{
    return AutoFile{UndoFileSeq().Open(pos, fReadOnly)};
}

fs::path BlockManager::GetBlockPosFilename(const FlatFilePos& pos) const
{
    return BlockFileSeq().FileName(pos);
}

bool BlockManager::FindBlockPos(FlatFilePos& pos, unsigned int nAddSize, unsigned int nHeight, uint64_t nTime, bool fKnown)
{
    LOCK(cs_LastBlockFile);

    unsigned int nFile = fKnown ? pos.nFile : m_last_blockfile;
    if (m_blockfile_info.size() <= nFile) {
        m_blockfile_info.resize(nFile + 1);
    }

    bool finalize_undo = false;
    if (!fKnown) {
        unsigned int max_blockfile_size{MAX_BLOCKFILE_SIZE};
        // Use smaller blockfiles in test-only -fastprune mode - but avoid
        // the possibility of having a block not fit into the block file.
        if (m_opts.fast_prune) {
            max_blockfile_size = 0x10000; // 64kiB
            if (nAddSize >= max_blockfile_size) {
                // dynamically adjust the blockfile size to be larger than the added size
                max_blockfile_size = nAddSize + 1;
            }
        }
        assert(nAddSize < max_blockfile_size);
        while (m_blockfile_info[nFile].nSize + nAddSize >= max_blockfile_size) {
            // when the undo file is keeping up with the block file, we want to flush it explicitly
            // when it is lagging behind (more blocks arrive than are being connected), we let the
            // undo block write case handle it
            finalize_undo = (m_blockfile_info[nFile].nHeightLast == m_undo_height_in_last_blockfile);
            nFile++;
            if (m_blockfile_info.size() <= nFile) {
                m_blockfile_info.resize(nFile + 1);
            }
        }
        pos.nFile = nFile;
        pos.nPos = m_blockfile_info[nFile].nSize;
    }

    if ((int)nFile != m_last_blockfile) {
        if (!fKnown) {
            LogPrint(BCLog::BLOCKSTORAGE, "Leaving block file %i: %s\n", m_last_blockfile, m_blockfile_info[m_last_blockfile].ToString());
        }

        // Do not propagate the return code. The flush concerns a previous block
        // and undo file that has already been written to. If a flush fails
        // here, and we crash, there is no expected additional block data
        // inconsistency arising from the flush failure here. However, the undo
        // data may be inconsistent after a crash if the flush is called during
        // a reindex. A flush error might also leave some of the data files
        // untrimmed.
        if (!FlushBlockFile(!fKnown, finalize_undo)) {
            LogPrintLevel(BCLog::BLOCKSTORAGE, BCLog::Level::Warning,
                          "Failed to flush previous block file %05i (finalize=%i, finalize_undo=%i) before opening new block file %05i\n",
                          m_last_blockfile, !fKnown, finalize_undo, nFile);
        }
        m_last_blockfile = nFile;
        m_undo_height_in_last_blockfile = 0; // No undo data yet in the new file, so reset our undo-height tracking.
    }

    m_blockfile_info[nFile].AddBlock(nHeight, nTime);
    if (fKnown) {
        m_blockfile_info[nFile].nSize = std::max(pos.nPos + nAddSize, m_blockfile_info[nFile].nSize);
    } else {
        m_blockfile_info[nFile].nSize += nAddSize;
    }

    if (!fKnown) {
        bool out_of_space;
        size_t bytes_allocated = BlockFileSeq().Allocate(pos, nAddSize, out_of_space);
        if (out_of_space) {
            m_opts.notifications.fatalError("Disk space is too low!", _("Disk space is too low!"));
            return false;
        }
        if (bytes_allocated != 0 && IsPruneMode()) {
            m_check_for_pruning = true;
        }
    }

    m_dirty_fileinfo.insert(nFile);
    return true;
}

bool BlockManager::FindUndoPos(BlockValidationState& state, int nFile, FlatFilePos& pos, unsigned int nAddSize)
{
    pos.nFile = nFile;

    LOCK(cs_LastBlockFile);

    pos.nPos = m_blockfile_info[nFile].nUndoSize;
    m_blockfile_info[nFile].nUndoSize += nAddSize;
    m_dirty_fileinfo.insert(nFile);

    bool out_of_space;
    size_t bytes_allocated = UndoFileSeq().Allocate(pos, nAddSize, out_of_space);
    if (out_of_space) {
        return FatalError(m_opts.notifications, state, "Disk space is too low!", _("Disk space is too low!"));
    }
    if (bytes_allocated != 0 && IsPruneMode()) {
        m_check_for_pruning = true;
    }

    return true;
}

bool BlockManager::WriteBlockToDisk(Span<const std::byte> payload, uint32_t size_field, FlatFilePos& pos) const
{
    // Open history file to append
    AutoFile fileout{OpenBlockFile(pos)};
    if (fileout.IsNull()) {
        LogError("WriteBlockToDisk: OpenBlockFile failed\n");
        return false;
    }

    // Write index header
    fileout << GetParams().MessageStart() << size_field;

    // Write block
    const int64_t fileOutPos{FileTell64(fileout.Get())};
    if (fileOutPos < 0) {
        LogError("WriteBlockToDisk: ftell failed\n");
        return false;
    }
    pos.nPos = fileOutPos;
    fileout.write(payload);

    return true;
}

/** Serialized data as a zstd frame, when compression is enabled and makes it smaller */
static std::optional<std::vector<std::byte>> CompressRecordPayload(Span<const std::byte> data, int level)
{
    if (level <= 0) return std::nullopt;
    std::vector<std::byte> compressed{util::ZstdCompress(data, level)};
    if (compressed.empty() || compressed.size() >= data.size()) return std::nullopt;
    return compressed;
}

/** Store serialized data as a zstd frame when that is smaller. Returns the record payload and its size field. */
static std::pair<DataStream, uint32_t> BuildRecordPayload(DataStream data, int level)
{
    if (const auto compressed{CompressRecordPayload(MakeByteSpan(data), level)}) {
        const uint32_t size_field{static_cast<uint32_t>(compressed->size()) | BLOCK_RECORD_COMPRESSED};
        return {DataStream{*compressed}, size_field};
    }
    const uint32_t size_field{static_cast<uint32_t>(data.size())};
    return {std::move(data), size_field};
}

static std::pair<DataStream, uint32_t> BuildBlockRecordPayload(const CBlock& block, int level)
{
    DataStream ss;
    ss << TX_NO_WITNESS(block);
    return BuildRecordPayload(std::move(ss), level);
}

bool BlockManager::WriteUndoDataForBlock(const CBlockUndo& blockundo, BlockValidationState& state, CBlockIndex& block)
{
    AssertLockHeld(::cs_main);
    // Write undo information to disk
    if (block.GetUndoPos().IsNull()) {
        // only compression needs the serialized undo data in memory, a plain record is streamed to the file
        std::optional<std::vector<std::byte>> compressed;
        if (m_opts.block_compression > 0) {
            DataStream undo_data;
            undo_data << blockundo;
            compressed = CompressRecordPayload(MakeByteSpan(undo_data), m_opts.block_compression);
        }
        const uint32_t size_field{compressed ? static_cast<uint32_t>(compressed->size()) | BLOCK_RECORD_COMPRESSED : static_cast<uint32_t>(::GetSerializeSize(blockundo))};
        FlatFilePos _pos;
        if (!FindUndoPos(state, block.nFile, _pos, (size_field & BLOCK_RECORD_SIZE_MASK) + BLOCK_SERIALIZATION_HEADER_SIZE + uint256::size())) {
            LogError("ConnectBlock(): FindUndoPos failed\n");
            return false;
        }
        if (compressed) m_have_compressed_records = true;
        if (!UndoWriteToDisk(blockundo, compressed, size_field, _pos, block.pprev->GetBlockHash())) {
            return FatalError(m_opts.notifications, state, "Failed to write undo data");
        }
        // rev files are written in block height order, whereas blk files are written as blocks come in (often out of order)
        // we want to flush the rev (undo) file once we've written the last block, which is indicated by the last height
        // in the block file info as below; note that this does not catch the case where the undo writes are keeping up
        // with the block writes (usually when a synced up node is getting newly mined blocks) -- this case is caught in
        // the FindBlockPos function
        if (_pos.nFile < m_last_blockfile && static_cast<uint32_t>(block.nHeight) == m_blockfile_info[_pos.nFile].nHeightLast) {
            // Do not propagate the return code, a failed flush here should not
            // be an indication for a failed write. If it were propagated here,
            // the caller would assume the undo data not to be written, when in
            // fact it is. Note though, that a failed flush might leave the data
            // file untrimmed.
            if (!FlushUndoFile(_pos.nFile, true)) {
                LogPrintLevel(BCLog::BLOCKSTORAGE, BCLog::Level::Warning, "Failed to flush undo file %05i\n", _pos.nFile);
            }
        } else if (_pos.nFile == m_last_blockfile && static_cast<uint32_t>(block.nHeight) > m_undo_height_in_last_blockfile) {
            m_undo_height_in_last_blockfile = block.nHeight;
        }
        // update nUndoPos in block index
        block.nUndoPos = _pos.nPos;
        block.nStatus |= BLOCK_HAVE_UNDO;
        m_dirty_blockindex.insert(&block);
    }

    return true;
}

FlatFilePos RecordHeaderPos(const FlatFilePos& pos)
{
    if (pos.nPos < BLOCK_SERIALIZATION_HEADER_SIZE) return {};
    return {pos.nFile, pos.nPos - BLOCK_SERIALIZATION_HEADER_SIZE};
}

/** Size field of the record the file is positioned at; the file is left at the payload */
static uint32_t ReadRecordSizeField(AutoFile& filein)
{
    uint32_t size_field;
    filein.ignore(std::tuple_size_v<MessageStartChars>);
    filein >> size_field;
    return size_field;
}

util::ZstdFileReader OpenRecordPayload(AutoFile& filein)
{
    const uint32_t size_field{ReadRecordSizeField(filein)};
    return {filein, size_field & BLOCK_RECORD_SIZE_MASK, (size_field & BLOCK_RECORD_COMPRESSED) != 0, MaxBlockSize()};
}

std::optional<uint256> BlockManager::ReadBlockFromDisk(CBlock& block, const FlatFilePos& pos) const
{
    block.SetNull();

    // Open history file to read
    AutoFile filein{OpenBlockFile(RecordHeaderPos(pos), true)};
    if (filein.IsNull()) {
        LogError("ReadBlockFromDisk: OpenBlockFile failed for %s\n", pos.ToString());
        return std::nullopt;
    }

    // Read block
    try {
        util::ZstdFileReader block_in{OpenRecordPayload(filein)};
        block_in >> TX_NO_WITNESS(block);
    } catch (const std::exception& e) {
        LogError("ReadBlockFromDisk: Deserialize or I/O error - %s at %s\n", e.what(), pos.ToString());
        return std::nullopt;
    }

    // Check the header
    const uint256 hash{block.GetHash()};
    if (!CheckProofOfWork(hash, block.nBits, GetConsensus())) {
        LogError("ReadBlockFromDisk: Errors in block header at %s\n", pos.ToString());
        return std::nullopt;
    }

    return hash;
}

bool BlockManager::ReadBlockFromDisk(CBlock& block, const CBlockIndex& index) const
{
    const FlatFilePos block_pos{WITH_LOCK(cs_main, return index.GetBlockPos())};

    const auto hash{ReadBlockFromDisk(block, block_pos)};
    if (!hash) {
        return false;
    }
    if (*hash != index.GetBlockHash()) {
        LogError("ReadBlockFromDisk(CBlock&, CBlockIndex*): GetHash() doesn't match index for %s at %s\n",
                     index.ToString(), block_pos.ToString());
        return false;
    }
    return true;
}

std::optional<uint32_t> BlockManager::StoredBlockSizeField(const FlatFilePos& pos) const
{
    AutoFile filein{OpenBlockFile(RecordHeaderPos(pos), true)};
    if (filein.IsNull()) return std::nullopt;
    try {
        return ReadRecordSizeField(filein);
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

FlatFilePos BlockManager::SaveBlockToDisk(const CBlock& block, int nHeight, const FlatFilePos* dbp)
{
    unsigned int nBlockSize = ::GetSerializeSize(TX_NO_WITNESS(block));
    FlatFilePos blockPos;
    const auto position_known {dbp != nullptr};
    DataStream payload;
    uint32_t size_field{0};
    if (position_known) {
        blockPos = *dbp;
        // the stored record may be smaller than the serialized block
        if (const auto stored{StoredBlockSizeField(blockPos)}) {
            nBlockSize = *stored & BLOCK_RECORD_SIZE_MASK;
            if (*stored & BLOCK_RECORD_COMPRESSED) m_have_compressed_records = true;
        }
    } else {
        std::tie(payload, size_field) = BuildBlockRecordPayload(block, m_opts.block_compression);
        nBlockSize = payload.size();
        // when known, blockPos.nPos points at the offset of the block data in the blk file. that already accounts for
        // the serialization header present in the file (the 4 magic message start bytes + the 4 length bytes = 8 bytes = BLOCK_SERIALIZATION_HEADER_SIZE).
        // we add BLOCK_SERIALIZATION_HEADER_SIZE only for new blocks since they will have the serialization header added when written to disk.
        nBlockSize += static_cast<unsigned int>(BLOCK_SERIALIZATION_HEADER_SIZE);
    }
    if (!FindBlockPos(blockPos, nBlockSize, nHeight, block.GetBlockTime(), position_known)) {
        LogError("%s: FindBlockPos failed\n", __func__);
        return FlatFilePos();
    }
    if (!position_known) {
        if (size_field & BLOCK_RECORD_COMPRESSED) m_have_compressed_records = true;
        if (!WriteBlockToDisk(MakeByteSpan(payload), size_field, blockPos)) {
            m_opts.notifications.fatalError("Failed to write block");
            return FlatFilePos();
        }
    }
    return blockPos;
}

class ImportingNow
{
    std::atomic<bool>& m_importing;

public:
    ImportingNow(std::atomic<bool>& importing) : m_importing{importing}
    {
        assert(m_importing == false);
        m_importing = true;
    }
    ~ImportingNow()
    {
        assert(m_importing == true);
        m_importing = false;
    }
};

void ImportBlocks(ChainstateManager& chainman, std::vector<fs::path> vImportFiles)
{
    ScheduleBatchPriority();

    {
        ImportingNow imp{fImporting};

        // -reindex
        if (fReindex) {
            int nFile = 0;
            // Map of disk positions for blocks with unknown parent (only used for reindex);
            // parent hash -> child disk position, multiple children can have the same parent.
            std::multimap<uint256, FlatFilePos> blocks_with_unknown_parent;
            while (true) {
                FlatFilePos pos(nFile, 0);
                if (!fs::exists(chainman.m_blockman.GetBlockPosFilename(pos))) {
                    break; // No block files left to reindex
                }
                AutoFile file{chainman.m_blockman.OpenBlockFile(pos, true)};
                if (file.IsNull()) {
                    break; // This error is logged in OpenBlockFile
                }
                LogPrintf("Reindexing block file blk%05u.dat...\n", (unsigned int)nFile);
                chainman.LoadExternalBlockFile(file, &pos, &blocks_with_unknown_parent);
                if (chainman.m_interrupt) {
                    LogPrintf("Interrupt requested. Exit %s\n", __func__);
                    return;
                }
                nFile++;
            }
            WITH_LOCK(::cs_main, chainman.m_blockman.m_block_tree_db->WriteReindexing(false));
            fReindex = false;
            LogPrintf("Reindexing finished\n");
            // To avoid ending up in a situation without genesis block, re-try initializing (no-op if reindexing worked):
            chainman.ActiveChainstate().LoadGenesisBlock();
        }

        // -loadblock=
        for (const fs::path& path : vImportFiles) {
            AutoFile file{fsbridge::fopen(path, "rb")};
            if (!file.IsNull()) {
                LogPrintf("Importing blocks file %s...\n", fs::PathToString(path));
                chainman.LoadExternalBlockFile(file);
                if (chainman.m_interrupt) {
                    LogPrintf("Interrupt requested. Exit %s\n", __func__);
                    return;
                }
            } else {
                LogPrintf("Warning: Could not open blocks file %s\n", fs::PathToString(path));
            }
        }

        // scan for better chains in the block chain database, that are not yet connected in the active best chain

        // We can't hold cs_main during ActivateBestChain even though we're accessing
        // the chainman unique_ptrs since ABC requires us not to be holding cs_main, so retrieve
        // the relevant pointers before the ABC call.
        for (Chainstate* chainstate : WITH_LOCK(::cs_main, return chainman.GetAll())) {
            BlockValidationState state;
            if (!chainstate->ActivateBestChain(state, nullptr)) {
                chainman.GetNotifications().fatalError(strprintf("Failed to connect best block (%s)", state.ToString()));
                return;
            }
        }
    } // End scope of ImportingNow
}
} // namespace node
