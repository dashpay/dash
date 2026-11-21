// Copyright (c) 2014-2021 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <chainparams.h>
#include <coins.h>
#include <consensus/amount.h>
#include <consensus/merkle.h>
#include <kernel/coinstats.h>
#include <net.h>
#include <primitives/block.h>
#include <primitives/transaction.h>
#include <script/script.h>
#include <txdb.h>
#include <uint256.h>
#include <util/check.h>
#include <util/chaintype.h>
#include <validation.h>
#include <version.h>

#include <set>

#include <test/util/setup_common.h>

#include <boost/test/unit_test.hpp>

BOOST_FIXTURE_TEST_SUITE(validation_tests, TestingSetup)

//! Test retrieval of valid assumeutxo values.
BOOST_AUTO_TEST_CASE(test_assumeutxo)
{
    const auto params = CreateChainParams(*m_node.args, ChainType::REGTEST);

    // These heights don't have assumeutxo configurations associated, per the contents
    // of kernel/chainparams.cpp.
    std::vector<int> bad_heights{0, 100, 111, 115, 209, 211};

    for (auto empty : bad_heights) {
        const auto out = params->AssumeutxoForHeight(empty);
        BOOST_CHECK(!out);
    }

    const auto out110 = *params->AssumeutxoForHeight(110);
    BOOST_CHECK_EQUAL(out110.hash_serialized.ToString(), "ffb210087e1ed14526c0c08a3ec3a7c8e288079eaa68acb87d3d4d9fd746079f");
    BOOST_CHECK_EQUAL(out110.nChainTx, 111U);

    const auto out110_2 = *params->AssumeutxoForBlockhash(uint256S("0x729bcb1479ff9f4968439f0276bd76bcb2de0f0720b7a16f383321f6a41cb238"));
    BOOST_CHECK_EQUAL(out110_2.hash_serialized.ToString(), "ffb210087e1ed14526c0c08a3ec3a7c8e288079eaa68acb87d3d4d9fd746079f");
    BOOST_CHECK_EQUAL(out110_2.nChainTx, 111U);

    const auto out210 = *params->AssumeutxoForHeight(200);
    BOOST_CHECK_EQUAL(out210.hash_serialized.ToString(), "16e00a64db4fa48dd989dce86d8677f41797d52044e5fc86021aa88cc22b665b");
    BOOST_CHECK_EQUAL(out210.nChainTx, 201U);
}

BOOST_AUTO_TEST_CASE(utxo_set_hash_commits_coin_height_and_coinbase)
{
    const uint256 best_block{WITH_LOCK(::cs_main, return m_node.chainman->ActiveChain().Tip()->GetBlockHash())};
    const uint256 txid{uint256S("0x1111111111111111111111111111111111111111111111111111111111111111")};
    const CTxOut txout{COIN, CScript{} << OP_TRUE};

    const auto utxo_set_hash = [&](kernel::CoinStatsHashType hash_type, uint32_t mutated_n, int height, bool coinbase) {
        CCoinsViewDB db{{.path = "utxo_set_hash", .cache_bytes = 1 << 20, .memory_only = true}, {}};
        CCoinsViewCache cache{&db};
        for (uint32_t n : {0, 1}) {
            const bool mutated{n == mutated_n};
            cache.AddCoin(COutPoint{txid, n}, Coin{txout, mutated ? height : 10, mutated && coinbase}, /*possible_overwrite=*/false);
        }
        cache.SetBestBlock(best_block);
        BOOST_REQUIRE(cache.Flush());
        return Assert(kernel::ComputeUTXOStats(hash_type, &db, m_node.chainman->m_blockman))->hashSerialized;
    };

    for (const auto hash_type : {kernel::CoinStatsHashType::HASH_SERIALIZED, kernel::CoinStatsHashType::MUHASH}) {
        const uint256 original{utxo_set_hash(hash_type, /*mutated_n=*/0, /*height=*/10, /*coinbase=*/false)};
        std::set<uint256> hashes{original};
        for (uint32_t n : {0, 1}) {
            BOOST_CHECK(hashes.insert(utxo_set_hash(hash_type, n, /*height=*/11, /*coinbase=*/false)).second);
            BOOST_CHECK(hashes.insert(utxo_set_hash(hash_type, n, /*height=*/10, /*coinbase=*/true)).second);
        }
        BOOST_CHECK_EQUAL(utxo_set_hash(hash_type, /*mutated_n=*/1, /*height=*/10, /*coinbase=*/false), original);
    }
}

//! Regtest accepts extra exact assumeutxo entries, each bound to its block.
BOOST_AUTO_TEST_CASE(assumeutxodata_option)
{
    const std::string utxo_hash(64, '1');
    const std::string evo_hash(64, '2');
    const std::string block_hash(64, '3');
    const auto params_for = [](const std::string& value) {
        ArgsManager args;
        SetupChainParamsOptions(args);
        args.ForceSetArg("-assumeutxodata", value);
        return CreateChainParams(args, ChainType::REGTEST);
    };

    const auto params{params_for(strprintf("219:%s:%s:230:%s", utxo_hash, evo_hash, block_hash))};
    const auto entry{params->AssumeutxoForHeight(219)};
    BOOST_REQUIRE(entry);
    BOOST_CHECK_EQUAL(entry->hash_serialized.ToString(), utxo_hash);
    BOOST_CHECK_EQUAL(entry->evo_hash.ToString(), evo_hash);
    BOOST_CHECK_EQUAL(entry->nChainTx, 230U);
    BOOST_CHECK_EQUAL(entry->blockhash.ToString(), block_hash);
    // The built-in entries stay.
    BOOST_CHECK(params->AssumeutxoForHeight(110));

    // The option authorizes nothing outside regtest.
    for (const auto& chain : {ChainType::MAIN, ChainType::TESTNET}) {
        ArgsManager args;
        SetupChainParamsOptions(args);
        args.ForceSetArg("-assumeutxodata", strprintf("219:%s:%s:230:%s", utxo_hash, evo_hash, block_hash));
        BOOST_CHECK(!CreateChainParams(args, chain)->AssumeutxoForHeight(219));
    }

    for (const std::string& invalid : {
             strprintf("219:%s:%s:230", utxo_hash, evo_hash),
             strprintf("0:%s:%s:230:%s", utxo_hash, evo_hash, block_hash),
             strprintf("219:%s:%s:230:%s", std::string(64, '0'), evo_hash, block_hash),
             strprintf("219:%s:%s:230:%s", utxo_hash, std::string(64, '0'), block_hash),
             strprintf("219:%s:%s:0:%s", utxo_hash, evo_hash, block_hash),
             strprintf("219:%s:%s:230:%s", utxo_hash, evo_hash, std::string(63, '3')),
             // Height 110 already has an entry.
             strprintf("110:%s:%s:230:%s", utxo_hash, evo_hash, block_hash),
         }) {
        BOOST_CHECK_THROW(params_for(invalid), std::runtime_error);
    }
}

//! Test the Dash (non-witness) IsBlockMutated() predicate directly.
BOOST_AUTO_TEST_CASE(block_malleation)
{
    // Calls IsBlockMutated and clears the CBlock validity-cache flags so the
    // same block object can be re-tested.
    auto is_mutated = [](CBlock& block) {
        bool mutated{IsBlockMutated(block)};
        block.fChecked = false;
        block.m_checked_merkle_root = false;
        return mutated;
    };
    auto is_not_mutated = [&is_mutated](CBlock& block) {
        return !is_mutated(block);
    };

    auto create_coinbase_tx = []() {
        CMutableTransaction coinbase;
        coinbase.vin.resize(1);
        coinbase.vout.resize(1);
        coinbase.vout[0].scriptPubKey.resize(4);
        auto tx = MakeTransactionRef(coinbase);
        assert(tx->IsCoinBase());
        return tx;
    };

    CBlock block;

    // An empty block is expected to have a merkle root of 0x0.
    BOOST_CHECK(block.vtx.empty());
    block.hashMerkleRoot = uint256::ONE;
    BOOST_CHECK(is_mutated(block));
    block.hashMerkleRoot = uint256();
    BOOST_CHECK(is_not_mutated(block));

    // A block with a single coinbase tx is mutated if the merkle root does not
    // equal the coinbase tx's hash.
    block.vtx.push_back(create_coinbase_tx());
    BOOST_CHECK(block.vtx[0]->GetHash() != block.hashMerkleRoot);
    BOOST_CHECK(is_mutated(block));
    block.hashMerkleRoot = BlockMerkleRoot(block);
    BOOST_CHECK(is_not_mutated(block));

    // A block with two transactions is mutated if the merkle root does not
    // match the transactions.
    block.vtx.push_back(MakeTransactionRef(CMutableTransaction{}));
    BOOST_CHECK(is_mutated(block));
    block.hashMerkleRoot = BlockMerkleRoot(block);
    BOOST_CHECK(is_not_mutated(block));

    // A block with a duplicated transaction (CVE-2012-2459) is mutated even
    // when hashMerkleRoot is set to the value the malleable tree computes.
    block.vtx[1] = block.vtx[0];
    bool mutated{false};
    block.hashMerkleRoot = BlockMerkleRoot(block, &mutated);
    BOOST_CHECK(mutated);
    BOOST_CHECK(is_mutated(block));

    // A block with a single 64-byte coinbase transaction is NOT considered
    // mutated: the 64-byte malleation guard is only applied when the first
    // transaction is not a coinbase.
    block.vtx.clear();
    {
        CMutableTransaction mtx;
        mtx.vin.resize(1);
        mtx.vout.resize(1);
        mtx.vout[0].scriptPubKey.resize(4);
        auto tx = MakeTransactionRef(mtx);
        assert(tx->IsCoinBase());
        assert(::GetSerializeSize(*tx, PROTOCOL_VERSION) == 64);
        block.vtx.push_back(tx);
        block.hashMerkleRoot = BlockMerkleRoot(block);
    }
    BOOST_CHECK(is_not_mutated(block));

    // Conversely, a coinbase-less block that contains a 64-byte transaction is
    // treated as mutated (the retained CVE-2012-2459 / 64-byte guard; see
    // "Weaknesses in Bitcoin's Merkle Root Construction").
    block.vtx.clear();
    {
        CMutableTransaction mtx;
        mtx.vin.resize(1);
        mtx.vin[0].prevout = COutPoint(uint256::ONE, 0); // non-null -> not a coinbase
        mtx.vout.resize(1);
        mtx.vout[0].scriptPubKey.resize(4);
        auto tx = MakeTransactionRef(mtx);
        assert(!tx->IsCoinBase());
        assert(::GetSerializeSize(*tx, PROTOCOL_VERSION) == 64);
        block.vtx.push_back(tx);
        block.hashMerkleRoot = BlockMerkleRoot(block);
    }
    BOOST_CHECK(is_mutated(block));
}

BOOST_AUTO_TEST_SUITE_END()
