// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <index/spentindex.h>

#include <chain.h>
#include <chainparams.h>
#include <index/addressindex_util.h>
#include <logging.h>
#include <node/blockstorage.h>
#include <primitives/block.h>
#include <primitives/transaction.h>
#include <script/script.h>
#include <undo.h>
#include <util/system.h>

constexpr uint8_t DB_SPENTINDEX{'p'};

std::unique_ptr<SpentIndex> g_spentindex;

SpentIndex::DB::DB(size_t n_cache_size, bool f_memory, bool f_wipe) :
    BaseIndex::DB(gArgs.GetDataDirNet() / "indexes" / "spentindex", n_cache_size, f_memory, f_wipe)
{
}

bool SpentIndex::DB::WriteBatch(const std::vector<CSpentIndexEntry>& entries)
{
    CDBBatch batch(*this);
    for (const auto& [key, value] : entries) {
        if (value.IsNull()) {
            // Null value means delete entry (used during disconnect)
            batch.Erase(std::make_pair(DB_SPENTINDEX, key));
        } else {
            batch.Write(std::make_pair(DB_SPENTINDEX, key), value);
        }
    }
    return CDBWrapper::WriteBatch(batch);
}

bool SpentIndex::DB::ReadSpentIndex(const CSpentIndexKey& key, CSpentIndexValue& value)
{
    return Read(std::make_pair(DB_SPENTINDEX, key), value);
}

bool SpentIndex::DB::EraseSpentIndex(const std::vector<CSpentIndexKey>& keys)
{
    CDBBatch batch(*this);
    for (const auto& key : keys) {
        batch.Erase(std::make_pair(DB_SPENTINDEX, key));
    }
    return CDBWrapper::WriteBatch(batch);
}

SpentIndex::SpentIndex(size_t n_cache_size, bool f_memory, bool f_wipe) :
    m_db(std::make_unique<SpentIndex::DB>(n_cache_size, f_memory, f_wipe))
{
}

SpentIndex::~SpentIndex() = default;

bool SpentIndex::WriteBlock(const CBlock& block, const CBlockIndex* pindex)
{
    // Skip genesis block (no inputs to index)
    if (pindex->nHeight == 0) {
        return true;
    }

    // Read undo data for this block to get information about spent outputs
    CBlockUndo blockundo;
    if (!node::UndoReadFromDisk(blockundo, pindex)) {
        return error("%s: Failed to read undo data for block %s at height %d", __func__,
                     pindex->GetBlockHash().ToString(), pindex->nHeight);
    }

    std::vector<CSpentIndexEntry> entries;

    // Process each non-coinbase transaction
    // blockundo.vtxundo[i] corresponds to block.vtx[i+1] (coinbase is skipped in undo data)
    for (size_t i = 0; i < blockundo.vtxundo.size(); i++) {
        const CTransactionRef& tx = block.vtx[i + 1]; // +1 to skip coinbase
        const CTxUndo& txundo = blockundo.vtxundo[i];
        const uint256 txhash = tx->GetHash();

        // Process each input
        if (tx->vin.size() != txundo.vprevout.size()) {
            return error("%s: Undo data mismatch for tx %s", __func__, txhash.ToString());
        }

        for (size_t j = 0; j < tx->vin.size(); j++) {
            const CTxIn& input = tx->vin[j];
            const Coin& coin = txundo.vprevout[j];
            const CTxOut& prevout = coin.out;

            AddressType address_type{AddressType::UNKNOWN};
            uint160 address_bytes;
            AddressBytesFromScript(prevout.scriptPubKey, address_type, address_bytes);

            // Create spent index entry: spent output -> spending tx info
            CSpentIndexKey key(input.prevout.hash, input.prevout.n);
            CSpentIndexValue value(txhash, j, pindex->nHeight, prevout.nValue, address_type, address_bytes);

            entries.emplace_back(key, value);
        }
    }

    return m_db->WriteBatch(entries);
}

bool SpentIndex::Rewind(const CBlockIndex* current_tip, const CBlockIndex* new_tip)
{
    assert(current_tip->GetAncestor(new_tip->nHeight) == new_tip);

    // Erase spent index entries for blocks being rewound
    for (const CBlockIndex* pindex = current_tip; pindex != new_tip; pindex = pindex->pprev) {
        // Skip genesis block
        if (pindex->nHeight == 0) continue;

        // Read block to get transactions
        CBlock block;
        if (!node::ReadBlockFromDisk(block, pindex, Params().GetConsensus())) {
            return error("%s: Failed to read block %s from disk during rewind", __func__,
                         pindex->GetBlockHash().ToString());
        }

        std::vector<CSpentIndexKey> keys_to_erase;

        // Process each non-coinbase transaction
        for (size_t i = 1; i < block.vtx.size(); i++) {
            const CTransactionRef& tx = block.vtx[i];

            // Erase spent index entries for each input
            for (const CTxIn& input : tx->vin) {
                CSpentIndexKey key(input.prevout.hash, input.prevout.n);
                keys_to_erase.push_back(key);
            }
        }

        if (!keys_to_erase.empty() && !m_db->EraseSpentIndex(keys_to_erase)) {
            return error("%s: Failed to erase spent index during rewind", __func__);
        }
    }

    // Call base class Rewind to update the best block pointer
    return BaseIndex::Rewind(current_tip, new_tip);
}

void SpentIndex::BlockDisconnected(const std::shared_ptr<const CBlock>& block, const CBlockIndex* pindex)
{
    // When a block is disconnected (e.g., via invalidateblock), we need to rewind the index
    // to remove this block's data
    const CBlockIndex* best_block_index = CurrentIndex();

    // Only rewind if we have this block indexed
    if (best_block_index && best_block_index->nHeight >= pindex->nHeight && pindex->pprev) {
        if (!Rewind(best_block_index, pindex->pprev)) {
            error("%s: Failed to rewind %s to previous block after disconnect", __func__, GetName());
        }
    }
}

BaseIndex::DB& SpentIndex::GetDB() const { return *m_db; }

bool SpentIndex::GetSpentInfo(const CSpentIndexKey& key, CSpentIndexValue& value) const
{
    if (!BlockUntilSyncedToCurrentChain()) {
        return false;
    }

    return m_db->ReadSpentIndex(key, value);
}
