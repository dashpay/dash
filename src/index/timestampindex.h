// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_INDEX_TIMESTAMPINDEX_H
#define BITCOIN_INDEX_TIMESTAMPINDEX_H

#include <index/base.h>
#include <index/timestampindex_types.h>

static constexpr bool DEFAULT_TIMESTAMPINDEX{false};

/**
 * TimestampIndex is used to map block timestamps to block hashes.
 * This allows efficient querying of blocks within a timestamp range.
 *
 * The index maintains a separate LevelDB database at <datadir>/indexes/timestampindex/
 */
class TimestampIndex final : public BaseIndex
{
protected:
    class DB;

private:
    const std::unique_ptr<DB> m_db;

protected:
    class DB : public BaseIndex::DB
    {
    public:
        explicit DB(size_t n_cache_size, bool f_memory = false, bool f_wipe = false);

        /// Write a timestamp index entry to the database
        bool Write(const CTimestampIndexKey& key);

        /// Read timestamp index entries within the given range
        bool ReadRange(uint32_t high, uint32_t low, std::vector<uint256>& hashes);

        /// Erase timestamp index entry
        bool EraseTimestampIndex(const CTimestampIndexKey& key);
    };

    bool WriteBlock(const CBlock& block, const CBlockIndex* pindex) override;

    /// Custom rewind to handle timestamp index cleanup
    bool Rewind(const CBlockIndex* current_tip, const CBlockIndex* new_tip) override;

    /// Handle block disconnections (e.g., from invalidateblock)
    void BlockDisconnected(const std::shared_ptr<const CBlock>& block, const CBlockIndex* pindex) override;

    BaseIndex::DB& GetDB() const override;
    const char* GetName() const override { return "timestampindex"; }

    /// TimestampIndex works with pruned nodes since it only stores block metadata
    bool AllowPrune() const override { return true; }

public:
    /// Constructs a new TimestampIndex.
    explicit TimestampIndex(size_t n_cache_size, bool f_memory = false, bool f_wipe = false);

    /// Destructor
    virtual ~TimestampIndex() override;

    /// Retrieve block hashes within the given timestamp range [low, high]
    bool GetBlockHashes(uint32_t high, uint32_t low, std::vector<uint256>& hashes) const;
};

/// Global TimestampIndex instance
extern std::unique_ptr<TimestampIndex> g_timestampindex;

#endif // BITCOIN_INDEX_TIMESTAMPINDEX_H
