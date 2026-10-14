// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <consensus/validation.h>
#include <governance/collateral.h>
#include <index/govcollateralindex.h>
#include <interfaces/chain.h>
#include <key.h>
#include <primitives/transaction.h>
#include <script/standard.h>
#include <util/strencodings.h>
#include <validation.h>
#include <validationinterface.h>

#include <test/util/index.h>
#include <test/util/masternode.h>
#include <test/util/setup_common.h>

#include <boost/test/unit_test.hpp>

#include <memory>
#include <string>
#include <vector>

namespace {
CScript BurnScript(const uint256& hash) { return CScript() << OP_RETURN << ToByteVector(hash); }

struct CollateralSetup : public TestChain100Setup {
    std::unique_ptr<GovCollateralIndex> index;
    SimpleUTXOMap utxos;

    CollateralSetup()
    {
        // Matures a few coinbases so the burns below can be funded
        for (int i = 0; i < 5; ++i) {
            MineBlock({});
        }
        utxos = BuildSimpleUtxoMap(m_coinbase_txns);
    }

    ~CollateralSetup()
    {
        SyncWithValidationInterfaceQueue();
        if (index) index->Stop();
    }

    CScript ChangeScript() const { return GetScriptForDestination(PKHash(coinbaseKey.GetPubKey())); }

    CBlock MineBlock(const std::vector<CMutableTransaction>& txns)
    {
        return CreateAndProcessBlock(txns, GetScriptForRawPubKey(coinbaseKey.GetPubKey()));
    }

    CMutableTransaction MakeBurn(const uint256& hash, CAmount amount, const std::vector<CTxOut>& extra_outputs = {})
    {
        CMutableTransaction tx;
        const auto spent{FundTransaction(*m_node.chainman, tx, utxos, BurnScript(hash), amount, ChangeScript())};
        tx.vout.insert(tx.vout.end(), extra_outputs.begin(), extra_outputs.end());
        SignTransaction(tx, spent, coinbaseKey);
        return tx;
    }

    void StartIndex()
    {
        // A block notification still queued from MineBlock would reach the index after it synced past that block
        SyncWithValidationInterfaceQueue();
        index = std::make_unique<GovCollateralIndex>(interfaces::MakeChain(m_node), /*n_cache_size=*/0, /*f_memory=*/true);
        BOOST_REQUIRE(index->Start());
        IndexWaitSynced(*index);
    }
};
} // namespace

BOOST_FIXTURE_TEST_SUITE(governance_collateral_tests, BasicTestingSetup)

BOOST_AUTO_TEST_CASE(collateral_info_counts_only_exact_burn_scripts)
{
    const uint256 hash{uint256S("aa")};
    const uint256 other_hash{uint256S("bb")};
    std::vector<unsigned char> short_payload(31, 0xcc);

    CMutableTransaction tx;
    tx.vout.emplace_back(COIN, GetScriptForDestination(PKHash(uint160{})));
    tx.vout.emplace_back(2 * COIN, BurnScript(hash));
    tx.vout.emplace_back(3 * COIN, CScript() << OP_RETURN << short_payload);
    // As long as a 32-byte burn, but pushed with OP_PUSHDATA1
    CScript pushdata_burn;
    pushdata_burn << OP_RETURN << OP_PUSHDATA1;
    pushdata_burn.push_back(31);
    pushdata_burn.insert(pushdata_burn.end(), other_hash.begin(), other_hash.end() - 1);
    BOOST_REQUIRE_EQUAL(pushdata_burn.size(), BurnScript(other_hash).size());
    tx.vout.emplace_back(4 * COIN, pushdata_burn);

    const uint256 block_hash{uint256S("01")};
    const auto info{governance::GetCollateralInfo(CTransaction{tx}, block_hash)};
    BOOST_CHECK(info.block_hash == block_hash);
    BOOST_CHECK(info.outputs_standard);
    BOOST_REQUIRE_EQUAL(info.burns.size(), 1U);
    BOOST_CHECK(info.BurnsAtLeast(hash, 2 * COIN));
    BOOST_CHECK(!info.BurnsAtLeast(hash, 2 * COIN + 1));
    BOOST_CHECK(!info.BurnsAtLeast(other_hash, 1));

    tx.vout.emplace_back(COIN, GetScriptForDestination(ScriptHash(uint160{})));
    BOOST_CHECK(!governance::GetCollateralInfo(CTransaction{tx}, block_hash).outputs_standard);
}

BOOST_FIXTURE_TEST_CASE(index_records_transactions_burning_the_proposal_fee, CollateralSetup)
{
    const uint256 fee_hash{uint256S("aa")};
    const uint256 short_hash{uint256S("bb")};
    // Its smaller burn can never pay for a proposal, so the index leaves it out
    const auto fee_burn{MakeBurn(fee_hash, GOVERNANCE_PROPOSAL_FEE_TX, {CTxOut{0, BurnScript(short_hash)}})};
    const auto short_burn{MakeBurn(short_hash, GOVERNANCE_PROPOSAL_FEE_TX - 1)};
    const CBlock block{MineBlock({fee_burn, short_burn})};

    // Blocks connected before the index started
    StartIndex();
    const auto collateral{index->FindCollateral(fee_burn.GetHash())};
    BOOST_REQUIRE(collateral);
    BOOST_CHECK(collateral->block_hash == block.GetHash());
    BOOST_CHECK(collateral->outputs_standard);
    BOOST_CHECK(collateral->BurnsAtLeast(fee_hash, GOVERNANCE_PROPOSAL_FEE_TX));
    BOOST_CHECK_EQUAL(collateral->burns.size(), 1U);
    BOOST_CHECK(!index->FindCollateral(short_burn.GetHash()));
    BOOST_CHECK(!index->FindCollateral(block.vtx[0]->GetHash()));

    // Blocks connected while the index is running
    const auto later_burn{MakeBurn(uint256S("cc"), GOVERNANCE_PROPOSAL_FEE_TX)};
    const CBlock later_block{MineBlock({later_burn})};
    BOOST_REQUIRE(index->BlockUntilSyncedToCurrentChain());
    const auto later_collateral{index->FindCollateral(later_burn.GetHash())};
    BOOST_REQUIRE(later_collateral);
    BOOST_CHECK(later_collateral->block_hash == later_block.GetHash());

    // Blocks reorged away
    BlockValidationState state;
    m_node.chainman->ActiveChainstate().InvalidateBlock(state, WITH_LOCK(::cs_main, return m_node.chainman->ActiveChain().Tip()));
    MineBlock({});
    BOOST_REQUIRE(index->BlockUntilSyncedToCurrentChain());
    BOOST_CHECK(!index->FindCollateral(later_burn.GetHash()));
    BOOST_CHECK(index->FindCollateral(fee_burn.GetHash()));
}

BOOST_AUTO_TEST_SUITE_END()
