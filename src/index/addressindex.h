// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_INDEX_ADDRESSINDEX_H
#define BITCOIN_INDEX_ADDRESSINDEX_H

#include <index/addressindex_types.h>
#include <index/base.h>

#include <vector>

static constexpr bool DEFAULT_ADDRESSINDEX{false};

/**
 * AddressIndex is an async index that maintains two separate databases:
 * 1. Address transaction history (all transactions touching an address)
 * 2. Address unspent outputs (UTXO set filtered by address)
 *
 * Uses undo data to access historical UTXO information, requiring undo files
 * to be available (incompatible with pruned nodes).
 */
class AddressIndex final : public BaseIndex
{
protected:
    class DB;
    const std::unique_ptr<DB> m_db;

    class DB : public BaseIndex::DB
    {
    public:
        explicit DB(size_t n_cache_size, bool f_memory = false, bool f_wipe = false);

        /// Write a batch of address index entries
        bool WriteBatch(const std::vector<CAddressIndexEntry>& address_entries,
                        const std::vector<CAddressUnspentIndexEntry>& unspent_entries);

        /// Read address transaction history
        bool ReadAddressIndex(const uint160& address_hash, const AddressType type,
                              std::vector<CAddressIndexEntry>& entries, const int32_t start = 0, const int32_t end = 0);

        /// Read address unspent outputs
        bool ReadAddressUnspentIndex(const uint160& address_hash, const AddressType type,
                                     std::vector<CAddressUnspentIndexEntry>& entries, const bool height_sort = false);

        /// Erase address transaction history entries
        bool EraseAddressIndex(const std::vector<CAddressIndexEntry>& entries);

        /// Update address unspent index (handles both adds and deletes)
        bool UpdateAddressUnspentIndex(const std::vector<CAddressUnspentIndexEntry>& entries);
    };

    /// Override to return false - we need undo data
    bool AllowPrune() const override { return false; }

    /// Write block data to the index databases
    bool WriteBlock(const CBlock& block, const CBlockIndex* pindex) override;

    /// Custom rewind to handle both transaction history and unspent index
    bool Rewind(const CBlockIndex* current_tip, const CBlockIndex* new_tip) override;

    /// Handle block disconnections (e.g., from invalidateblock)
    void BlockDisconnected(const std::shared_ptr<const CBlock>& block, const CBlockIndex* pindex) override;

    BaseIndex::DB& GetDB() const override;

    const char* GetName() const override { return "addressindex"; }

public:
    /// Constructs the index, which becomes available to be queried
    explicit AddressIndex(size_t n_cache_size, bool f_memory = false, bool f_wipe = false);

    /// Destructor
    virtual ~AddressIndex() override;

    /// Query address transaction history
    bool GetAddressIndex(const uint160& address_hash, const AddressType type, std::vector<CAddressIndexEntry>& entries,
                         const int32_t start = 0, const int32_t end = 0) const;

    /// Query address unspent outputs
    bool GetAddressUnspentIndex(const uint160& address_hash, const AddressType type,
                                std::vector<CAddressUnspentIndexEntry>& entries, const bool height_sort = false) const;
};

/// Global AddressIndex instance
extern std::unique_ptr<AddressIndex> g_addressindex;

#endif // BITCOIN_INDEX_ADDRESSINDEX_H
