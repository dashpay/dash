// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <index/addressindex.h>

#include <chainparams.h>
#include <clientversion.h>
#include <hash.h>
#include <index/addressindex_util.h>
#include <logging.h>
#include <node/blockstorage.h>
#include <undo.h>
#include <util/system.h>

constexpr uint8_t DB_ADDRESSINDEX{'a'};
constexpr uint8_t DB_ADDRESSUNSPENTINDEX{'u'};

std::unique_ptr<AddressIndex> g_addressindex;

AddressIndex::DB::DB(size_t n_cache_size, bool f_memory, bool f_wipe) :
    BaseIndex::DB(gArgs.GetDataDirNet() / "indexes" / "addressindex", n_cache_size, f_memory, f_wipe)
{
}

bool AddressIndex::DB::WriteBatch(const std::vector<CAddressIndexEntry>& address_entries,
                                  const std::vector<CAddressUnspentIndexEntry>& unspent_entries)
{
    CDBBatch batch(*this);

    // Write address transaction history
    for (const auto& [key, value] : address_entries) {
        batch.Write(std::make_pair(DB_ADDRESSINDEX, key), value);
    }

    // Write address unspent outputs (handles both adds and deletes)
    for (const auto& [key, value] : unspent_entries) {
        if (value.IsNull()) {
            // Null value means delete entry
            batch.Erase(std::make_pair(DB_ADDRESSUNSPENTINDEX, key));
        } else {
            batch.Write(std::make_pair(DB_ADDRESSUNSPENTINDEX, key), value);
        }
    }

    return CDBWrapper::WriteBatch(batch);
}

bool AddressIndex::DB::ReadAddressIndex(const uint160& address_hash, const AddressType type,
                                        std::vector<CAddressIndexEntry>& entries, const int32_t start, const int32_t end)
{
    std::unique_ptr<CDBIterator> pcursor(NewIterator());

    if (start > 0 && end > 0) {
        pcursor->Seek(std::make_pair(DB_ADDRESSINDEX, CAddressIndexIteratorHeightKey(type, address_hash, start)));
    } else {
        pcursor->Seek(std::make_pair(DB_ADDRESSINDEX, CAddressIndexIteratorKey(type, address_hash)));
    }

    while (pcursor->Valid()) {
        std::pair<uint8_t, CAddressIndexKey> key;
        if (pcursor->GetKey(key) && key.first == DB_ADDRESSINDEX && key.second.m_address_type == type &&
            key.second.m_address_bytes == address_hash) {
            if (end > 0 && key.second.m_block_height > end) {
                break;
            }
            CAmount value;
            if (pcursor->GetValue(value)) {
                entries.emplace_back(key.second, value);
                pcursor->Next();
            } else {
                return error("failed to get address index value");
            }
        } else {
            break;
        }
    }

    return true;
}

bool AddressIndex::DB::ReadAddressUnspentIndex(const uint160& address_hash, const AddressType type,
                                               std::vector<CAddressUnspentIndexEntry>& entries, const bool height_sort)
{
    std::unique_ptr<CDBIterator> pcursor(NewIterator());

    pcursor->Seek(std::make_pair(DB_ADDRESSUNSPENTINDEX, CAddressIndexIteratorKey(type, address_hash)));

    while (pcursor->Valid()) {
        std::pair<uint8_t, CAddressUnspentKey> key;
        if (pcursor->GetKey(key) && key.first == DB_ADDRESSUNSPENTINDEX && key.second.m_address_type == type &&
            key.second.m_address_bytes == address_hash) {
            CAddressUnspentValue value;
            if (pcursor->GetValue(value)) {
                entries.emplace_back(key.second, value);
                pcursor->Next();
            } else {
                return error("failed to get address unspent value");
            }
        } else {
            break;
        }
    }

    if (height_sort) {
        std::sort(entries.begin(), entries.end(),
                  [](const CAddressUnspentIndexEntry& a, const CAddressUnspentIndexEntry& b) {
                      return a.second.m_block_height < b.second.m_block_height;
                  });
    }

    return true;
}

bool AddressIndex::DB::EraseAddressIndex(const std::vector<CAddressIndexEntry>& entries)
{
    CDBBatch batch(*this);

    for (const auto& [key, _] : entries) {
        batch.Erase(std::make_pair(DB_ADDRESSINDEX, key));
    }

    return CDBWrapper::WriteBatch(batch);
}

bool AddressIndex::DB::UpdateAddressUnspentIndex(const std::vector<CAddressUnspentIndexEntry>& entries)
{
    CDBBatch batch(*this);

    for (const auto& [key, value] : entries) {
        if (value.IsNull()) {
            batch.Erase(std::make_pair(DB_ADDRESSUNSPENTINDEX, key));
        } else {
            batch.Write(std::make_pair(DB_ADDRESSUNSPENTINDEX, key), value);
        }
    }

    return CDBWrapper::WriteBatch(batch);
}

AddressIndex::AddressIndex(size_t n_cache_size, bool f_memory, bool f_wipe) :
    m_db(std::make_unique<AddressIndex::DB>(n_cache_size, f_memory, f_wipe))
{
}

AddressIndex::~AddressIndex() = default;

bool AddressIndex::WriteBlock(const CBlock& block, const CBlockIndex* pindex)
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

    std::vector<CAddressIndexEntry> addressIndex;
    std::vector<CAddressUnspentIndexEntry> addressUnspentIndex;

    // Process each non-coinbase transaction
    // blockundo.vtxundo[i] corresponds to block.vtx[i+1] (coinbase is skipped in undo data)
    if (blockundo.vtxundo.size() != block.vtx.size() - 1) {
        return error("%s: Undo data size mismatch for block %s (expected %zu, got %zu)", __func__,
                     pindex->GetBlockHash().ToString(), block.vtx.size() - 1, blockundo.vtxundo.size());
    }

    for (size_t i = 0; i < blockundo.vtxundo.size(); i++) {
        const CTransactionRef& tx = block.vtx[i + 1]; // +1 to skip coinbase
        const CTxUndo& txundo = blockundo.vtxundo[i];
        const uint256 txhash = tx->GetHash();

        // Verify undo data matches transaction
        if (tx->vin.size() != txundo.vprevout.size()) {
            return error("%s: Undo data mismatch for tx %s", __func__, txhash.ToString());
        }

        // Process inputs (spending activity)
        for (size_t j = 0; j < tx->vin.size(); j++) {
            const CTxIn& input = tx->vin[j];
            const Coin& coin = txundo.vprevout[j];
            const CTxOut& prevout = coin.out;

            AddressType address_type{AddressType::UNKNOWN};
            uint160 address_bytes;
            if (!AddressBytesFromScript(prevout.scriptPubKey, address_type, address_bytes)) {
                continue;
            }

            // Record spending activity
            addressIndex.emplace_back(CAddressIndexKey(address_type, address_bytes, pindex->nHeight, i + 1, txhash, j, true),
                                      prevout.nValue * -1);

            // Remove from unspent index
            addressUnspentIndex.emplace_back(CAddressUnspentKey(address_type, address_bytes, input.prevout.hash,
                                                                input.prevout.n),
                                             CAddressUnspentValue() // Null value means delete
            );
        }

        // Process outputs (receiving activity)
        for (size_t k = 0; k < tx->vout.size(); k++) {
            const CTxOut& out = tx->vout[k];

            AddressType address_type{AddressType::UNKNOWN};
            uint160 address_bytes;
            if (!AddressBytesFromScript(out.scriptPubKey, address_type, address_bytes)) {
                continue;
            }

            // Record receiving activity
            addressIndex.emplace_back(CAddressIndexKey(address_type, address_bytes, pindex->nHeight, i + 1, txhash, k, false),
                                      out.nValue);

            // Add to unspent index
            addressUnspentIndex.emplace_back(CAddressUnspentKey(address_type, address_bytes, txhash, k),
                                             CAddressUnspentValue(out.nValue, out.scriptPubKey, pindex->nHeight));
        }
    }

    // Also process coinbase outputs (receiving activity only)
    const CTransactionRef& coinbase = block.vtx[0];
    const uint256 coinbase_hash = coinbase->GetHash();
    for (size_t k = 0; k < coinbase->vout.size(); k++) {
        const CTxOut& out = coinbase->vout[k];

        AddressType address_type{AddressType::UNKNOWN};
        uint160 address_bytes;
        if (!AddressBytesFromScript(out.scriptPubKey, address_type, address_bytes)) {
            continue;
        }

        // Record receiving activity for coinbase
        addressIndex.emplace_back(CAddressIndexKey(address_type, address_bytes, pindex->nHeight, 0, coinbase_hash, k, false),
                                  out.nValue);

        // Add coinbase outputs to unspent index
        addressUnspentIndex.emplace_back(CAddressUnspentKey(address_type, address_bytes, coinbase_hash, k),
                                         CAddressUnspentValue(out.nValue, out.scriptPubKey, pindex->nHeight));
    }

    return m_db->WriteBatch(addressIndex, addressUnspentIndex);
}

bool AddressIndex::Rewind(const CBlockIndex* current_tip, const CBlockIndex* new_tip)
{
    assert(current_tip->GetAncestor(new_tip->nHeight) == new_tip);

    // Rewind the unspent index by processing blocks in reverse
    // We need to undo all operations from current_tip back to (but not including) new_tip
    for (const CBlockIndex* pindex = current_tip; pindex != new_tip; pindex = pindex->pprev) {
        CBlock block;
        if (!node::ReadBlockFromDisk(block, pindex, Params().GetConsensus())) {
            return error("%s: Failed to read block %s from disk during rewind", __func__,
                         pindex->GetBlockHash().ToString());
        }

        CBlockUndo blockundo;
        if (pindex->nHeight > 0 && !node::UndoReadFromDisk(blockundo, pindex)) {
            return error("%s: Failed to read undo data for block %s during rewind", __func__,
                         pindex->GetBlockHash().ToString());
        }

        std::vector<CAddressIndexEntry> addressIndex;
        std::vector<CAddressUnspentIndexEntry> addressUnspentIndex;

        // Process transactions in reverse to undo them
        // blockundo.vtxundo[i] corresponds to block.vtx[i+1] (coinbase skipped)
        for (size_t i = 0; i < blockundo.vtxundo.size(); i++) {
            const CTransactionRef& tx = block.vtx[i + 1];
            const CTxUndo& txundo = blockundo.vtxundo[i];
            const uint256 txhash = tx->GetHash();

            // Undo outputs (remove from unspent index and transaction history)
            for (size_t k = 0; k < tx->vout.size(); k++) {
                const CTxOut& out = tx->vout[k];

                AddressType address_type{AddressType::UNKNOWN};
                uint160 address_bytes;

                if (!AddressBytesFromScript(out.scriptPubKey, address_type, address_bytes)) {
                    continue;
                }

                // Remove receiving activity from history
                addressIndex.push_back(std::make_pair(CAddressIndexKey(address_type, address_bytes, pindex->nHeight,
                                                                       i + 1, txhash, k, false),
                                                      out.nValue));

                // Remove from unspent index (mark for deletion)
                addressUnspentIndex.push_back(std::make_pair(CAddressUnspentKey(address_type, address_bytes, txhash, k),
                                                             CAddressUnspentValue() // null value signals deletion
                                                             ));
            }

            // Undo inputs (restore to unspent index, remove spending from history)
            for (size_t j = 0; j < tx->vin.size(); j++) {
                const CTxIn& input = tx->vin[j];
                const Coin& coin = txundo.vprevout[j];
                const CTxOut& prevout = coin.out;

                AddressType address_type{AddressType::UNKNOWN};
                uint160 address_bytes;

                if (!AddressBytesFromScript(prevout.scriptPubKey, address_type, address_bytes)) {
                    continue;
                }

                // Remove spending activity from history
                addressIndex.push_back(
                    std::make_pair(CAddressIndexKey(address_type, address_bytes, pindex->nHeight, i + 1, txhash, j, true),
                                   prevout.nValue * -1));

                // Restore to unspent index
                addressUnspentIndex.push_back(
                    std::make_pair(CAddressUnspentKey(address_type, address_bytes, input.prevout.hash, input.prevout.n),
                                   CAddressUnspentValue(prevout.nValue, prevout.scriptPubKey, coin.nHeight)));
            }
        }

        // Process coinbase outputs (remove from indices)
        if (!block.vtx.empty()) {
            const CTransactionRef& coinbase_tx = block.vtx[0];
            const uint256 cb_hash = coinbase_tx->GetHash();

            for (size_t k = 0; k < coinbase_tx->vout.size(); k++) {
                const CTxOut& out = coinbase_tx->vout[k];

                AddressType address_type{AddressType::UNKNOWN};
                uint160 address_bytes;

                if (!AddressBytesFromScript(out.scriptPubKey, address_type, address_bytes)) {
                    continue;
                }

                // Remove coinbase receiving activity
                addressIndex.push_back(
                    std::make_pair(CAddressIndexKey(address_type, address_bytes, pindex->nHeight, 0, cb_hash, k, false),
                                   out.nValue));

                // Remove from unspent index
                addressUnspentIndex.push_back(std::make_pair(CAddressUnspentKey(address_type, address_bytes, cb_hash, k),
                                                             CAddressUnspentValue() // null value signals deletion
                                                             ));
            }
        }

        // Erase address history entries and update unspent index
        if (!m_db->EraseAddressIndex(addressIndex)) {
            return error("%s: Failed to erase address index during rewind", __func__);
        }

        if (!m_db->UpdateAddressUnspentIndex(addressUnspentIndex)) {
            return error("%s: Failed to update address unspent index during rewind", __func__);
        }
    }

    // Call base class Rewind to update the best block pointer
    return BaseIndex::Rewind(current_tip, new_tip);
}

void AddressIndex::BlockDisconnected(const std::shared_ptr<const CBlock>& block, const CBlockIndex* pindex)
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

BaseIndex::DB& AddressIndex::GetDB() const { return *m_db; }

bool AddressIndex::GetAddressIndex(const uint160& address_hash, const AddressType type,
                                   std::vector<CAddressIndexEntry>& entries, const int32_t start, const int32_t end) const
{
    if (!BlockUntilSyncedToCurrentChain()) {
        return false;
    }

    return m_db->ReadAddressIndex(address_hash, type, entries, start, end);
}

bool AddressIndex::GetAddressUnspentIndex(const uint160& address_hash, const AddressType type,
                                          std::vector<CAddressUnspentIndexEntry>& entries, const bool height_sort) const
{
    if (!BlockUntilSyncedToCurrentChain()) {
        return false;
    }

    return m_db->ReadAddressUnspentIndex(address_hash, type, entries, height_sort);
}
