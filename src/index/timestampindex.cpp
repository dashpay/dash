// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <index/timestampindex.h>

#include <chain.h>
#include <logging.h>
#include <util/system.h>

constexpr uint8_t DB_TIMESTAMPINDEX{'s'};

std::unique_ptr<TimestampIndex> g_timestampindex;

TimestampIndex::DB::DB(size_t n_cache_size, bool f_memory, bool f_wipe) :
    BaseIndex::DB(gArgs.GetDataDirNet() / "indexes" / "timestampindex", n_cache_size, f_memory, f_wipe)
{
}

bool TimestampIndex::DB::Write(const CTimestampIndexKey& key)
{
    return CDBWrapper::Write(std::make_pair(DB_TIMESTAMPINDEX, key), true);
}

bool TimestampIndex::DB::ReadRange(uint32_t high, uint32_t low, std::vector<uint256>& hashes)
{
    std::unique_ptr<CDBIterator> pcursor(NewIterator());

    // Seek to the starting timestamp
    pcursor->Seek(std::make_pair(DB_TIMESTAMPINDEX, CTimestampIndexIteratorKey(low)));

    // Iterate through all entries in the timestamp range
    while (pcursor->Valid()) {
        std::pair<uint8_t, CTimestampIndexKey> key;
        if (pcursor->GetKey(key) && key.first == DB_TIMESTAMPINDEX && key.second.m_block_time <= high) {
            hashes.push_back(key.second.m_block_hash);
            pcursor->Next();
        } else {
            break;
        }
    }

    return true;
}

bool TimestampIndex::DB::EraseTimestampIndex(const CTimestampIndexKey& key)
{
    return CDBWrapper::Erase(std::make_pair(DB_TIMESTAMPINDEX, key));
}

TimestampIndex::TimestampIndex(size_t n_cache_size, bool f_memory, bool f_wipe) :
    m_db(std::make_unique<TimestampIndex::DB>(n_cache_size, f_memory, f_wipe))
{
}

TimestampIndex::~TimestampIndex() = default;

bool TimestampIndex::WriteBlock(const CBlock& block, const CBlockIndex* pindex)
{
    // Skip genesis block
    if (pindex->nHeight == 0) return true;

    // Create timestamp index key from block metadata
    CTimestampIndexKey key(pindex->nTime, pindex->GetBlockHash());

    // Write to database
    return m_db->Write(key);
}

bool TimestampIndex::Rewind(const CBlockIndex* current_tip, const CBlockIndex* new_tip)
{
    assert(current_tip->GetAncestor(new_tip->nHeight) == new_tip);

    // Erase timestamp index entries for blocks being rewound
    for (const CBlockIndex* pindex = current_tip; pindex != new_tip; pindex = pindex->pprev) {
        // Skip genesis block
        if (pindex->nHeight == 0) continue;

        CTimestampIndexKey key(pindex->nTime, pindex->GetBlockHash());
        if (!m_db->EraseTimestampIndex(key)) {
            return error("%s: Failed to erase timestamp index for block %s during rewind", __func__,
                         pindex->GetBlockHash().ToString());
        }
    }

    // Call base class Rewind to update the best block pointer
    return BaseIndex::Rewind(current_tip, new_tip);
}

void TimestampIndex::BlockDisconnected(const std::shared_ptr<const CBlock>& block, const CBlockIndex* pindex)
{
    // When a block is disconnected (e.g., via invalidateblock), we need to rewind the index
    // to remove this block's data
    const CBlockIndex* best_block_index = CurrentIndex();

    // Only rewind if we have this block indexed and it's not the genesis block
    if (best_block_index && best_block_index->nHeight >= pindex->nHeight && pindex->pprev) {
        if (!Rewind(best_block_index, pindex->pprev)) {
            error("%s: Failed to rewind %s to previous block after disconnect", __func__, GetName());
        }
    }
}

BaseIndex::DB& TimestampIndex::GetDB() const { return *m_db; }

bool TimestampIndex::GetBlockHashes(uint32_t high, uint32_t low, std::vector<uint256>& hashes) const
{
    if (!BlockUntilSyncedToCurrentChain()) {
        return false;
    }

    return m_db->ReadRange(high, low, hashes);
}
