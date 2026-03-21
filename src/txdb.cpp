// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2021 The Bitcoin Core developers
// Copyright (c) 2025 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <txdb.h>

#include <chain.h>
#include <index/addressindex_types.h>
#include <index/spentindex_types.h>
#include <index/timestampindex_types.h>
#include <logging.h>
#include <pow.h>
#include <primitives/block.h>
#include <primitives/transaction.h>
#include <random.h>
#include <shutdown.h>
#include <uint256.h>
#include <util/system.h>
#include <util/translation.h>
#include <util/vector.h>

#include <cassert>
#include <cstdlib>
#include <iterator>

static constexpr uint8_t DB_COIN{'C'};
static constexpr uint8_t DB_BLOCK_FILES{'f'};
static constexpr uint8_t DB_BLOCK_INDEX{'b'};

static constexpr uint8_t DB_BEST_BLOCK{'B'};
static constexpr uint8_t DB_HEAD_BLOCKS{'H'};
static constexpr uint8_t DB_FLAG{'F'};
static constexpr uint8_t DB_REINDEX_FLAG{'R'};
static constexpr uint8_t DB_LAST_BLOCK{'l'};

// Keys used in previous version that might still be found in the DB:
static constexpr uint8_t DB_COINS{'c'};
// CBlockTreeDB::DB_TXINDEX_BLOCK{'T'};
// CBlockTreeDB::DB_TXINDEX{'t'}
// CBlockTreeDB::ReadFlag("txindex")

// Old synchronous index keys (deprecated):
static constexpr uint8_t DB_ADDRESSINDEX{'a'};
static constexpr uint8_t DB_ADDRESSUNSPENTINDEX{'u'};
static constexpr uint8_t DB_SPENTINDEX{'p'};
static constexpr uint8_t DB_TIMESTAMPINDEX{'s'};

bool CCoinsViewDB::NeedsUpgrade()
{
    std::unique_ptr<CDBIterator> cursor{m_db->NewIterator()};
    // DB_COINS was deprecated in v0.15.0, commit
    // 1088b02f0ccd7358d2b7076bb9e122d59d502d02
    cursor->Seek(std::make_pair(DB_COINS, uint256{}));
    return cursor->Valid();
}

namespace {

struct CoinEntry {
    COutPoint* outpoint;
    uint8_t key;
    explicit CoinEntry(const COutPoint* ptr) : outpoint(const_cast<COutPoint*>(ptr)), key(DB_COIN)  {}

    SERIALIZE_METHODS(CoinEntry, obj) { READWRITE(obj.key, obj.outpoint->hash, VARINT(obj.outpoint->n)); }
};

} // namespace

CCoinsViewDB::CCoinsViewDB(fs::path ldb_path, size_t nCacheSize, bool fMemory, bool fWipe) :
    m_db(std::make_unique<CDBWrapper>(ldb_path, nCacheSize, fMemory, fWipe, true)),
    m_ldb_path(ldb_path),
    m_is_memory(fMemory) { }

void CCoinsViewDB::ResizeCache(size_t new_cache_size)
{
    // We can't do this operation with an in-memory DB since we'll lose all the coins upon
    // reset.
    if (!m_is_memory) {
        // Have to do a reset first to get the original `m_db` state to release its
        // filesystem lock.
        m_db.reset();
        m_db = std::make_unique<CDBWrapper>(
            m_ldb_path, new_cache_size, m_is_memory, /*fWipe=*/false, /*obfuscate=*/true);
    }
}

bool CCoinsViewDB::GetCoin(const COutPoint &outpoint, Coin &coin) const {
    return m_db->Read(CoinEntry(&outpoint), coin);
}

bool CCoinsViewDB::HaveCoin(const COutPoint &outpoint) const {
    return m_db->Exists(CoinEntry(&outpoint));
}

uint256 CCoinsViewDB::GetBestBlock() const {
    uint256 hashBestChain;
    if (!m_db->Read(DB_BEST_BLOCK, hashBestChain))
        return uint256();
    return hashBestChain;
}

std::vector<uint256> CCoinsViewDB::GetHeadBlocks() const {
    std::vector<uint256> vhashHeadBlocks;
    if (!m_db->Read(DB_HEAD_BLOCKS, vhashHeadBlocks)) {
        return std::vector<uint256>();
    }
    return vhashHeadBlocks;
}

bool CCoinsViewDB::BatchWrite(CCoinsMap &mapCoins, const uint256 &hashBlock, bool erase) {
    CDBBatch batch(*m_db);
    size_t count = 0;
    size_t changed = 0;
    size_t batch_size = (size_t)gArgs.GetIntArg("-dbbatchsize", nDefaultDbBatchSize);
    int crash_simulate = gArgs.GetIntArg("-dbcrashratio", 0);
    assert(!hashBlock.IsNull());

    uint256 old_tip = GetBestBlock();
    if (old_tip.IsNull()) {
        // We may be in the middle of replaying.
        std::vector<uint256> old_heads = GetHeadBlocks();
        if (old_heads.size() == 2) {
            if (old_heads[0] != hashBlock) {
                LogPrintLevel(BCLog::COINDB, BCLog::Level::Error, "The coins database detected an inconsistent state, likely due to a previous crash or shutdown. You will need to restart bitcoind with the -reindex-chainstate or -reindex configuration option.\n");
            }
            assert(old_heads[0] == hashBlock);
            old_tip = old_heads[1];
        }
    }

    // In the first batch, mark the database as being in the middle of a
    // transition from old_tip to hashBlock.
    // A vector is used for future extensibility, as we may want to support
    // interrupting after partial writes from multiple independent reorgs.
    batch.Erase(DB_BEST_BLOCK);
    batch.Write(DB_HEAD_BLOCKS, Vector(hashBlock, old_tip));

    for (CCoinsMap::iterator it = mapCoins.begin(); it != mapCoins.end();) {
        if (it->second.flags & CCoinsCacheEntry::DIRTY) {
            CoinEntry entry(&it->first);
            if (it->second.coin.IsSpent())
                batch.Erase(entry);
            else
                batch.Write(entry, it->second.coin);
            changed++;
        }
        count++;
        it = erase ? mapCoins.erase(it) : std::next(it);
        if (batch.SizeEstimate() > batch_size) {
            LogPrint(BCLog::COINDB, "Writing partial batch of %.2f MiB\n", batch.SizeEstimate() * (1.0 / 1048576.0));
            m_db->WriteBatch(batch);
            batch.Clear();
            if (crash_simulate) {
                static FastRandomContext rng;
                if (rng.randrange(crash_simulate) == 0) {
                    LogPrintf("Simulating a crash. Goodbye.\n");
                    _Exit(0);
                }
            }
        }
    }

    // In the last batch, mark the database as consistent with hashBlock again.
    batch.Erase(DB_HEAD_BLOCKS);
    batch.Write(DB_BEST_BLOCK, hashBlock);

    LogPrint(BCLog::COINDB, "Writing final batch of %.2f MiB\n", batch.SizeEstimate() * (1.0 / 1048576.0));
    bool ret = m_db->WriteBatch(batch);
    LogPrint(BCLog::COINDB, "Committed %u changed transaction outputs (out of %u) to coin database...\n", (unsigned int)changed, (unsigned int)count);
    return ret;
}

size_t CCoinsViewDB::EstimateSize() const
{
    return m_db->EstimateSize(DB_COIN, uint8_t(DB_COIN + 1));
}

CBlockTreeDB::CBlockTreeDB(size_t nCacheSize, bool fMemory, bool fWipe) : CDBWrapper(gArgs.GetDataDirNet() / "blocks" / "index", nCacheSize, fMemory, fWipe) {
}

bool CBlockTreeDB::ReadBlockFileInfo(int nFile, CBlockFileInfo &info) {
    return Read(std::make_pair(DB_BLOCK_FILES, nFile), info);
}

bool CBlockTreeDB::WriteReindexing(bool fReindexing) {
    if (fReindexing)
        return Write(DB_REINDEX_FLAG, uint8_t{'1'});
    else
        return Erase(DB_REINDEX_FLAG);
}

void CBlockTreeDB::ReadReindexing(bool &fReindexing) {
    fReindexing = Exists(DB_REINDEX_FLAG);
}

bool CBlockTreeDB::ReadLastBlockFile(int &nFile) {
    return Read(DB_LAST_BLOCK, nFile);
}

/** Specialization of CCoinsViewCursor to iterate over a CCoinsViewDB */
class CCoinsViewDBCursor: public CCoinsViewCursor
{
public:
    // Prefer using CCoinsViewDB::Cursor() since we want to perform some
    // cache warmup on instantiation.
    CCoinsViewDBCursor(CDBIterator* pcursorIn, const uint256&hashBlockIn):
        CCoinsViewCursor(hashBlockIn), pcursor(pcursorIn) {}
    ~CCoinsViewDBCursor() = default;

    bool GetKey(COutPoint &key) const override;
    bool GetValue(Coin &coin) const override;

    bool Valid() const override;
    void Next() override;

private:
    std::unique_ptr<CDBIterator> pcursor;
    std::pair<uint8_t, COutPoint> keyTmp;

    friend class CCoinsViewDB;
};

std::unique_ptr<CCoinsViewCursor> CCoinsViewDB::Cursor() const
{
    auto i = std::make_unique<CCoinsViewDBCursor>(
        const_cast<CDBWrapper&>(*m_db).NewIterator(), GetBestBlock());
    /* It seems that there are no "const iterators" for LevelDB.  Since we
       only need read operations on it, use a const-cast to get around
       that restriction.  */
    i->pcursor->Seek(DB_COIN);
    // Cache key of first record
    if (i->pcursor->Valid()) {
        CoinEntry entry(&i->keyTmp.second);
        i->pcursor->GetKey(entry);
        i->keyTmp.first = entry.key;
    } else {
        i->keyTmp.first = 0; // Make sure Valid() and GetKey() return false
    }
    return i;
}

bool CCoinsViewDBCursor::GetKey(COutPoint &key) const
{
    // Return cached key
    if (keyTmp.first == DB_COIN) {
        key = keyTmp.second;
        return true;
    }
    return false;
}

bool CCoinsViewDBCursor::GetValue(Coin &coin) const
{
    return pcursor->GetValue(coin);
}

bool CCoinsViewDBCursor::Valid() const
{
    return keyTmp.first == DB_COIN;
}

void CCoinsViewDBCursor::Next()
{
    pcursor->Next();
    CoinEntry entry(&keyTmp.second);
    if (!pcursor->Valid() || !pcursor->GetKey(entry)) {
        keyTmp.first = 0; // Invalidate cached key after last record so that Valid() and GetKey() return false
    } else {
        keyTmp.first = entry.key;
    }
}

bool CBlockTreeDB::WriteBatchSync(const std::vector<std::pair<int, const CBlockFileInfo*> >& fileInfo, int nLastFile, const std::vector<const CBlockIndex*>& blockinfo) {
    CDBBatch batch(*this);
    for (std::vector<std::pair<int, const CBlockFileInfo*> >::const_iterator it=fileInfo.begin(); it != fileInfo.end(); it++) {
        batch.Write(std::make_pair(DB_BLOCK_FILES, it->first), *it->second);
    }
    batch.Write(DB_LAST_BLOCK, nLastFile);
    for (std::vector<const CBlockIndex*>::const_iterator it=blockinfo.begin(); it != blockinfo.end(); it++) {
        batch.Write(std::make_pair(DB_BLOCK_INDEX, (*it)->GetBlockHash()), CDiskBlockIndex(*it));
    }
    return WriteBatch(batch, true);
}

bool CBlockTreeDB::WriteFlag(const std::string &name, bool fValue) {
    return Write(std::make_pair(DB_FLAG, name), fValue ? uint8_t{'1'} : uint8_t{'0'});
}

bool CBlockTreeDB::ReadFlag(const std::string &name, bool &fValue) {
    uint8_t ch;
    if (!Read(std::make_pair(DB_FLAG, name), ch))
        return false;
    fValue = ch == uint8_t{'1'};
    return true;
}

bool CBlockTreeDB::LoadBlockIndexGuts(const Consensus::Params& consensusParams, std::function<CBlockIndex*(const uint256&)> insertBlockIndex)
{
    AssertLockHeld(::cs_main);
    std::unique_ptr<CDBIterator> pcursor(NewIterator());
    pcursor->Seek(std::make_pair(DB_BLOCK_INDEX, uint256()));

    // Load m_block_index
    while (pcursor->Valid()) {
        if (ShutdownRequested()) return false;
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
                    return error("%s: CheckProofOfWork failed: %s", __func__, pindexNew->ToString());
                }

                pcursor->Next();
            } else {
                return error("%s: failed to read value", __func__);
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
 * @param source_db      The old database to migrate from (CBlockTreeDB)
 * @param target_db      The new database to migrate to
 * @param index_name     Human-readable name for logging
 * @param batch_size     Maximum batch size before flushing
 * @return               Number of entries migrated, or -1 on error
 */
template <uint8_t DbKey, typename KeyType, typename ValueType>
static int64_t MigrateIndex(CBlockTreeDB& source_db, CDBWrapper& target_db,
                            const char* index_name, size_t batch_size)
{
    using KeyPair = std::pair<uint8_t, KeyType>;

    size_t count = 0;
    CDBBatch new_batch(target_db);
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
        if (pcursor->GetKey(key) && key.first == DbKey) {
            if (!pcursor->GetValue(value)) {
                LogPrintf("Failed to read %s value\n", index_name);
                return -1;
            }

            new_batch.Write(key, value);
            erase_batch.Erase(key);
            count++;
            pcursor->Next();

            if (new_batch.SizeEstimate() > batch_size) {
                LogPrintf("Migrating partial batch of %s entries (%.2f MiB, %d entries)...\n",
                         index_name, new_batch.SizeEstimate() * (1.0 / 1048576.0), count);
                erased_since_compact += erase_batch.SizeEstimate();
                if (!target_db.WriteBatch(new_batch)) {
                    LogPrintf("Failed to write %s batch to new database\n", index_name);
                    return -1;
                }
                if (!source_db.WriteBatch(erase_batch)) {
                    LogPrintf("Failed to erase old %s data\n", index_name);
                    return -1;
                }
                new_batch.Clear();
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
    if (!target_db.WriteBatch(new_batch, true)) {
        LogPrintf("Failed to write final %s batch\n", index_name);
        return -1;
    }
    if (!source_db.WriteBatch(erase_batch, true)) {
        LogPrintf("Failed to erase final %s batch\n", index_name);
        return -1;
    }
    new_batch.Clear();
    erase_batch.Clear();

    // Close iterator before final compaction
    pcursor.reset();

    // Compact final range if we migrated any keys
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
            return error("%s: Failed to write best block for %s", __func__, index_name);
        }
        LogPrintf("Set %s best block to %s\n", index_name, best_block_hash.ToString());
    }

    return true;
}

bool CBlockTreeDB::MigrateOldIndexData()
{
    // Migrate old synchronous index data that was stored in the block index database
    // to new async indexes in separate databases under indexes/{timestampindex,spentindex,addressindex}/
    // This preserves existing index data so users don't need to rebuild.
    // Indexes are migrated independently since they can be enabled individually.

    // Only migrate indexes that are actually enabled via command-line flags
    // NOTE: not using DEFAULT_* constants here to avoid circular dependencies
    const bool fTimestampIndex = gArgs.GetBoolArg("-timestampindex", false);
    const bool fSpentIndex = gArgs.GetBoolArg("-spentindex", false);
    const bool fAddressIndex = gArgs.GetBoolArg("-addressindex", false);

    if (!fTimestampIndex && !fSpentIndex && !fAddressIndex) {
        // No indexes enabled, skip migration entirely
        return true;
    }

    LogPrintf("Checking for old index data in block index database...\n");

    size_t batch_size = (size_t)gArgs.GetIntArg("-dbbatchsize", nDefaultDbBatchSize);
    size_t total_count = 0;
    const fs::path indexes_path = gArgs.GetDataDirNet() / "indexes";

    // Read the best block hash from coins database to set as best block for migrated indexes
    // Old synchronous indexes were updated during ConnectBlock, so they're synced to the active chain tip
    uint256 best_block_hash;
    {
        fs::path chainstate_path = gArgs.GetDataDirNet() / "chainstate";
        CDBWrapper coins_db(chainstate_path, 0, false, false);
        if (!coins_db.Read(DB_BEST_BLOCK, best_block_hash)) {
            // If we can't read the best block, the indexes will resync from scratch
            LogPrintf("Warning: Could not read best block from chainstate, migrated indexes will resync\n");
            best_block_hash.SetNull();
        } else {
            LogPrintf("Migrating indexes with best block: %s\n", best_block_hash.ToString());
        }
    }

    // Migrate timestamp index (only if enabled)
    if (fTimestampIndex) {
        const fs::path db_path = indexes_path / "timestampindex";
        CDBWrapper timestamp_db(db_path, 0, false, false);

        int64_t count = MigrateIndex<DB_TIMESTAMPINDEX, CTimestampIndexKey, bool>(
            *this, timestamp_db, "timestamp index", batch_size);
        if (count < 0) {
            return error("%s: Failed to migrate timestamp index", __func__);
        }
        if (count > 0) {
            LogPrintf("Migrated %d timestamp index entries\n", count);
            total_count += count;
            if (!FinalizeMigration(timestamp_db, best_block_hash, "timestamp index")) {
                return false;
            }
        }
    }

    // Migrate spent index (only if enabled)
    if (fSpentIndex) {
        const fs::path db_path = indexes_path / "spentindex";
        CDBWrapper spent_db(db_path, 0, false, false);

        int64_t count = MigrateIndex<DB_SPENTINDEX, CSpentIndexKey, CSpentIndexValue>(
            *this, spent_db, "spent index", batch_size);
        if (count < 0) {
            return error("%s: Failed to migrate spent index", __func__);
        }
        if (count > 0) {
            LogPrintf("Migrated %d spent index entries\n", count);
            total_count += count;
            if (!FinalizeMigration(spent_db, best_block_hash, "spent index")) {
                return false;
            }
        }
    }

    // Migrate address index (includes both address and unspent indexes) (only if enabled)
    if (fAddressIndex) {
        const fs::path db_path = indexes_path / "addressindex";
        CDBWrapper address_db(db_path, 0, false, false);

        // Migrate address index (transaction history)
        int64_t address_count = MigrateIndex<DB_ADDRESSINDEX, CAddressIndexKey, CAmount>(
            *this, address_db, "address index", batch_size);
        if (address_count < 0) {
            return error("%s: Failed to migrate address index", __func__);
        }
        if (address_count > 0) {
            LogPrintf("Migrated %d address index entries\n", address_count);
            total_count += address_count;
        }

        // Migrate address unspent index
        int64_t unspent_count = MigrateIndex<DB_ADDRESSUNSPENTINDEX, CAddressUnspentKey, CAddressUnspentValue>(
            *this, address_db, "address unspent index", batch_size);
        if (unspent_count < 0) {
            return error("%s: Failed to migrate address unspent index", __func__);
        }
        if (unspent_count > 0) {
            LogPrintf("Migrated %d address unspent index entries\n", unspent_count);
            total_count += unspent_count;
        }

        // Write best block locator if we migrated any address index data
        if (address_count > 0 || unspent_count > 0) {
            if (!FinalizeMigration(address_db, best_block_hash, "address and address unspent indexes")) {
                return false;
            }
        }
    }

    if (total_count > 0) {
        LogPrintf("Compacting remaining block index database...\n");
        CompactFull();
        LogPrintf("Successfully migrated %d index entries to new databases\n", total_count);
    } else {
        LogPrintf("No old index data found\n");
    }

    return true;
}
