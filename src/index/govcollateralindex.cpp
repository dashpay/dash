// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <chain.h>
#include <chainparams.h>
#include <common/args.h>
#include <index/govcollateralindex.h>
#include <logging.h>
#include <node/blockstorage.h>
#include <primitives/block.h>
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
    AssertLockHeld(::cs_main);
    // BaseIndex only locks pruning once a new index has read its first block, so pruning at startup could
    // still delete the blocks it is about to read.
    if (!block) {
        m_chainstate->m_blockman.UpdatePruneLock(GetName(), node::PruneLockInfo{/*height_first=*/0});
    }
    m_indexed_chainstate = m_chainstate;
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
        if (!m_chainstate->m_blockman.ReadBlockFromDisk(block, *pindex)) {
            LogError("%s: Failed to read block %s from disk\n", __func__, pindex->GetBlockHash().ToString());
            return false;
        }
        for (const auto& [txid, _] : GetCollaterals(block, pindex->GetBlockHash())) {
            m_db->EraseCollateral(batch, txid);
        }
    }
    WriteRewindLocator(batch, new_tip_index);
    return m_db->WriteBatch(batch);
}

BaseIndex::DB& GovCollateralIndex::GetDB() const { return *m_db; }

bool GovCollateralIndex::CanSync(Chainstate& chainstate) const
{
    AssertLockHeld(::cs_main);
    const auto locator{m_db->ReadBestBlock()};
    const CBlockIndex* best_block{nullptr};
    const CBlockIndex* locator_block{nullptr};
    if (!locator.IsNull()) {
        best_block = chainstate.FindForkInGlobalIndex(locator);
        locator_block = chainstate.m_blockman.LookupBlockIndex(locator.vHave.front());
    }
    // Startup resumes from the uncommitted tip instead when there is one, and only needs the blocks above it. It
    // fails without the locator's block, so the uncommitted tip can't stand in for that.
    if (locator.IsNull() || locator_block) {
        const CBlockIndex* resume_block{ResumeBlock(chainstate.m_blockman, locator_block)};
        if (resume_block != locator_block) best_block = chainstate.m_chain.FindFork(resume_block);
    }
    for (const CBlockIndex* block{chainstate.m_chain.Tip()}; block != best_block; block = block->pprev) {
        if (!(block->nStatus & BLOCK_HAVE_DATA)) return false;
    }
    return true;
}

bool GovCollateralIndex::IsCollateralSynced() const
{
    // While a snapshot is validated in the background, the index follows the background chainstate.
    LOCK(::cs_main);
    return m_indexed_chainstate && m_indexed_chainstate == &m_indexed_chainstate->m_chainman.ActiveChainstate() &&
           GetSummary().synced;
}

std::optional<governance::CollateralInfo> GovCollateralIndex::FindCollateral(const uint256& txid) const
{
    governance::CollateralInfo info;
    if (!m_db->ReadCollateral(txid, info)) return std::nullopt;
    return info;
}
