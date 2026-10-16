// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_INDEX_GOVCOLLATERALINDEX_H
#define BITCOIN_INDEX_GOVCOLLATERALINDEX_H

#include <governance/collateral.h>
#include <index/base.h>

#include <optional>

class Chainstate;

/**
 * GovCollateralIndex records every transaction that burns at least the proposal fee to an
 * `OP_RETURN <32-byte hash>` output, i.e. everything that could be a governance proposal's
 * collateral. Unlike the transaction index it stores what the collateral check needs rather
 * than a position in the block files, so it keeps working after those blocks are pruned.
 *
 * The index is written to a LevelDB database at <datadir>/indexes/govcollateralindex/
 */
class GovCollateralIndex final : public BaseIndex, public governance::CollateralSource
{
protected:
    class DB;

private:
    const std::unique_ptr<DB> m_db;

    bool AllowPrune() const override { return true; }

protected:
    bool CustomInit(const std::optional<interfaces::BlockKey>& block) override;

    bool CustomAppend(const interfaces::BlockInfo& block) override;

    bool CustomRewind(const interfaces::BlockKey& current_tip, const interfaces::BlockKey& new_tip) override;

    BaseIndex::DB& GetDB() const override;

public:
    explicit GovCollateralIndex(std::unique_ptr<interfaces::Chain> chain, size_t n_cache_size, bool f_memory = false,
                                bool f_wipe = false);

    // Destructor is declared because this class contains a unique_ptr to an incomplete type.
    virtual ~GovCollateralIndex() override;

    /// Whether the blocks the index still has to read are stored, i.e. whether Start() would succeed.
    bool CanSync(Chainstate& chainstate) const EXCLUSIVE_LOCKS_REQUIRED(::cs_main);

    bool IsCollateralSynced() const override;

    std::optional<governance::CollateralInfo> FindCollateral(const uint256& txid) const override;
};

#endif // BITCOIN_INDEX_GOVCOLLATERALINDEX_H
