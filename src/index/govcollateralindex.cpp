// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <index/govcollateralindex.h>

#include <chain.h>
#include <chainparams.h>
#include <logging.h>
#include <node/blockstorage.h>
#include <primitives/block.h>
#include <util/system.h>
#include <validation.h>

#include <cassert>
#include <vector>

constexpr uint8_t DB_GOVCOLLATERAL{'g'};

namespace {
//! The transactions of a block that can pay for a proposal, with their collateral summaries
std::vector<std::pair<uint256, governance::CollateralInfo>> GetCollaterals(const CBlock& block, const uint256& block_hash)
{
    std::vector<std::pair<uint256, governance::CollateralInfo>> collaterals;
    for (const auto& tx : block.vtx) {
        auto info{governance::GetCollateralInfo(*tx, block_hash)};
        // Smaller burns can never pay for a proposal
        std::erase_if(info.burns, [](const auto& burn) { return burn.second < GOVERNANCE_PROPOSAL_FEE_TX; });
        if (!info.burns.empty()) {
            collaterals.emplace_back(tx->GetHash(), std::move(info));
        }
    }
    return collaterals;
}
} // namespace

class GovCollateralIndex::DB : public BaseIndex::DB
{
public:
    explicit DB(size_t n_cache_size, bool f_memory = false, bool f_wipe = false);

    bool ReadCollateral(const uint256& txid, governance::CollateralInfo& info) const;

    [[nodiscard]] bool WriteCollaterals(const std::vector<std::pair<uint256, governance::CollateralInfo>>& collaterals);

    void EraseCollateral(CDBBatch& batch, const uint256& txid);
};

GovCollateralIndex::DB::DB(size_t n_cache_size, bool f_memory, bool f_wipe) :
    BaseIndex::DB(gArgs.GetDataDirNet() / "indexes" / "govcollateralindex", n_cache_size, f_memory, f_wipe)
{
}

bool GovCollateralIndex::DB::ReadCollateral(const uint256& txid, governance::CollateralInfo& info) const
{
    return Read(std::make_pair(DB_GOVCOLLATERAL, txid), info);
}

bool GovCollateralIndex::DB::WriteCollaterals(const std::vector<std::pair<uint256, governance::CollateralInfo>>& collaterals)
{
    CDBBatch batch(*this);
    for (const auto& [txid, info] : collaterals) {
        batch.Write(std::make_pair(DB_GOVCOLLATERAL, txid), info);
    }
    return WriteBatch(batch);
}

void GovCollateralIndex::DB::EraseCollateral(CDBBatch& batch, const uint256& txid)
{
    batch.Erase(std::make_pair(DB_GOVCOLLATERAL, txid));
}

GovCollateralIndex::GovCollateralIndex(std::unique_ptr<interfaces::Chain> chain, size_t n_cache_size, bool f_memory,
                                       bool f_wipe) :
    BaseIndex(std::move(chain), "govcollateralindex"),
    m_db(std::make_unique<GovCollateralIndex::DB>(n_cache_size, f_memory, f_wipe))
{
}

GovCollateralIndex::~GovCollateralIndex() = default;

bool GovCollateralIndex::CustomInit(const std::optional<interfaces::BlockKey>& block)
{
    // BaseIndex only locks pruning once a new index has read its first block, so pruning at startup could
    // still delete the blocks it is about to read.
    if (!block) {
        WITH_LOCK(::cs_main, m_chainstate->m_blockman.UpdatePruneLock(GetName(), node::PruneLockInfo{/*height_first=*/0}));
    }
    return true;
}

bool GovCollateralIndex::CustomAppend(const interfaces::BlockInfo& block)
{
    // Like the transaction index, leave out the genesis block, whose outputs are not spendable.
    if (block.height == 0) return true;

    assert(block.data);
    const auto collaterals{GetCollaterals(*block.data, block.hash)};
    return collaterals.empty() || m_db->WriteCollaterals(collaterals);
}

bool GovCollateralIndex::CustomRewind(const interfaces::BlockKey& current_tip, const interfaces::BlockKey& new_tip)
{
    // Otherwise burns in blocks that were reorged away would stay in the index for good. They are erased in the
    // same batch that moves the best block back: erased first on their own, a crash in between would leave the
    // index claiming blocks whose collaterals are gone, and it would never index them again.
    CDBBatch batch(*m_db);
    const CBlockIndex* new_tip_index{WITH_LOCK(::cs_main, return m_chainstate->m_blockman.LookupBlockIndex(new_tip.hash))};
    for (const CBlockIndex* pindex{WITH_LOCK(::cs_main, return m_chainstate->m_blockman.LookupBlockIndex(current_tip.hash))};
         pindex != new_tip_index; pindex = pindex->pprev) {
        CBlock block;
        if (!node::ReadBlockFromDisk(block, pindex, Params().GetConsensus())) {
            LogError("%s: Failed to read block %s from disk\n", __func__, pindex->GetBlockHash().ToString());
            return false;
        }
        for (const auto& [txid, _] : GetCollaterals(block, pindex->GetBlockHash())) {
            m_db->EraseCollateral(batch, txid);
        }
    }
    m_db->WriteBestBlock(batch, GetLocator(new_tip_index));
    return m_db->WriteBatch(batch);
}

BaseIndex::DB& GovCollateralIndex::GetDB() const { return *m_db; }

bool GovCollateralIndex::CanSync(Chainstate& chainstate) const
{
    AssertLockHeld(::cs_main);
    CBlockLocator locator;
    const CBlockIndex* best_block{nullptr};
    if (m_db->ReadBestBlock(locator) && !locator.IsNull()) {
        best_block = chainstate.FindForkInGlobalIndex(locator);
    }
    for (const CBlockIndex* block{chainstate.m_chain.Tip()}; block != best_block; block = block->pprev) {
        if (!(block->nStatus & BLOCK_HAVE_DATA)) return false;
    }
    return true;
}

bool GovCollateralIndex::IsCollateralSynced() const { return GetSummary().synced; }

std::optional<governance::CollateralInfo> GovCollateralIndex::FindCollateral(const uint256& txid) const
{
    governance::CollateralInfo info;
    if (!m_db->ReadCollateral(txid, info)) return std::nullopt;
    return info;
}
