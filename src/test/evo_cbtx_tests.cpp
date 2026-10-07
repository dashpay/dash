// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <test/util/llmq_tests.h>
#include <test/util/setup_common.h>

#include <bls/bls.h>
#include <chain.h>
#include <chainlock/chainlock.h>
#include <chainparams.h>
#include <consensus/merkle.h>
#include <consensus/validation.h>
#include <evo/cbtx.h>
#include <evo/chainhelper.h>
#include <evo/evodb.h>
#include <evo/specialtx.h>
#include <evo/specialtxman.h>
#include <hash.h>
#include <llmq/blockprocessor.h>
#include <llmq/commitment.h>
#include <llmq/context.h>
#include <llmq/params.h>
#include <masternode/sync.h>
#include <netfulfilledman.h>
#include <node/miner.h>
#include <primitives/block.h>
#include <primitives/transaction.h>
#include <spork.h>
#include <uint256.h>
#include <validation.h>

#include <cstdint>
#include <limits>
#include <utility>
#include <vector>

#include <boost/test/unit_test.hpp>

using namespace llmq;
using namespace llmq::testutils;

BOOST_AUTO_TEST_SUITE(evo_cbtx_tests)

// Out-of-range bestCLHeightDiff (>= pindex->nHeight) must be rejected with
// "bad-cbtx-cldiff" so that the subsequent GetAncestor() call sees a valid height.
//
// The defensive nullptr branch after GetAncestor() returns "bad-cbtx-cldiff-ancestor".
// That branch is unreachable in practice (the range check guarantees the requested
// ancestor height is in [0, pindex->nHeight - 1], for which GetAncestor() never returns
// nullptr) and cannot be exercised from a unit test: a fake CBlockIndex with no pprev
// would trip GetAncestor()'s `assert(pprev)` while walking, not return nullptr.
BOOST_FIXTURE_TEST_CASE(check_cbtx_best_chainlock_rejects_excessive_height_diff, RegTestingSetup)
{
    const auto& consensus_params = Params().GetConsensus();
    const auto& chain = *WITH_LOCK(::cs_main, return &m_node.chainman->ActiveChain());
    auto& qman = *Assert(m_node.llmq_ctx)->qman;
    auto& chainlocks = *Assert(m_node.chainlocks);

    // Standalone fake block index with no predecessor, so the prevBlockCoinbaseChainlock
    // branch is skipped and the validation path under test is reached directly.
    CBlockIndex pindex;
    pindex.nHeight = 5;

    // A structurally-valid BLS signature is required for the IsValid() guard.
    CBLSSecretKey sk;
    sk.MakeNewKey();
    const bool legacy_scheme = bls::bls_legacy_scheme.load();
    CBLSSignature valid_sig = sk.Sign(uint256::ONE, legacy_scheme);
    BOOST_REQUIRE(valid_sig.IsValid());

    CCbTx cbTx;
    cbTx.nVersion = CCbTx::Version::CLSIG_AND_BALANCE;
    cbTx.bestCLSignature = valid_sig;

    // bestCLHeightDiff == nHeight: lower boundary of the rejected range.
    cbTx.bestCLHeightDiff = static_cast<uint32_t>(pindex.nHeight);
    BlockValidationState state;
    BOOST_CHECK(!CheckCbTxBestChainlock(cbTx, &pindex, consensus_params, chain, qman, chainlocks,
                                        /*enforce_branch_binding=*/false, state));
    BOOST_CHECK_EQUAL(state.GetRejectReason(), "bad-cbtx-cldiff");

    // Upper boundary: uint32_t max.
    cbTx.bestCLHeightDiff = std::numeric_limits<uint32_t>::max();
    BlockValidationState state_big;
    BOOST_CHECK(!CheckCbTxBestChainlock(cbTx, &pindex, consensus_params, chain, qman, chainlocks,
                                        /*enforce_branch_binding=*/false, state_big));
    BOOST_CHECK_EQUAL(state_big.GetRejectReason(), "bad-cbtx-cldiff");
}

namespace {
CTransactionRef MakeCommitmentTx(const CFinalCommitment& qc, int height)
{
    CFinalCommitmentTxPayload payload;
    payload.nHeight = height;
    payload.commitment = qc;

    CMutableTransaction tx;
    tx.nVersion = 3;
    tx.nType = TRANSACTION_QUORUM_COMMITMENT;
    SetTxPayload(tx, payload);
    return MakeTransactionRef(std::move(tx));
}

uint256 CalcQuorumMerkleRootForCommitment(const CFinalCommitment& qc)
{
    std::vector<uint256> hashes{::SerializeHash(qc)};
    bool mutated{false};
    return ComputeMerkleRoot(hashes, &mutated);
}

CFinalCommitment MakeDistinctCommitment(const Consensus::LLMQParams& params, const uint256& quorum_hash, uint8_t salt)
{
    CFinalCommitment qc = CreateValidCommitment(params, quorum_hash);
    // Force a deterministic difference even if random BLS material collides.
    qc.quorumVvecHash = uint256{std::vector<unsigned char>(32, salt)};
    return qc;
}

CBlock MakeEmptyBlock()
{
    CBlock block;
    block.vtx.emplace_back(MakeTransactionRef(CMutableTransaction{}));
    return block;
}

void ExpectQuorumMerkleRoot(const CBlock& block, const CBlockIndex* pindex, const CQuorumBlockProcessor& qblockman,
                            const CFinalCommitment& qc)
{
    uint256 merkle_root;
    BlockValidationState state;
    BOOST_REQUIRE(CalcCbTxMerkleRootQuorums(block, pindex, qblockman, merkle_root, state));
    BOOST_CHECK_EQUAL(merkle_root.ToString(), CalcQuorumMerkleRootForCommitment(qc).ToString());
}

const CBlockIndex* GenesisIndex(const node::NodeContext& node)
{
    LOCK(cs_main);
    return node.chainman->ActiveChain()[0];
}
} // anonymous namespace

// Activate DIP0003 immediately so GetCommitmentsFromBlock accepts the payload
// at a low height without a long fake chain.
struct Dip3ActiveSetup : public RegTestingSetup {
    Dip3ActiveSetup() :
        RegTestingSetup({"-dip3params=1:1"})
    {
    }
};

// End to end: disconnecting the block that mined a commitment must make a replacement
// commitment for the same quorum base visible, even though the active base-block list
// that keys the caches is unchanged across the swap.
BOOST_FIXTURE_TEST_CASE(qc_hash_cache_invalidated_by_undoblock, Dip3ActiveSetup)
{
    auto& evoDb = *Assert(m_node.evodb);
    auto& qblockman = *Assert(m_node.llmq_ctx)->quorum_block_processor;
    const auto& params = GetLLMQParams(Consensus::LLMQType::LLMQ_TEST);

    const CBlockIndex* pindex_genesis = GenesisIndex(m_node);
    BOOST_REQUIRE(pindex_genesis != nullptr);
    const uint256 quorum_hash = pindex_genesis->GetBlockHash();

    const CFinalCommitment qc_a = MakeDistinctCommitment(params, quorum_hash, /*salt=*/0x33);
    const CFinalCommitment qc_b = MakeDistinctCommitment(params, quorum_hash, /*salt=*/0x44);
    BOOST_REQUIRE(::SerializeHash(qc_a) != ::SerializeHash(qc_b));

    const uint256 mined_hash_a = GetTestBlockHash(11);
    const uint256 mined_hash_b = GetTestBlockHash(12);
    constexpr int mined_height = 1;

    {
        LOCK(::cs_main);
        auto dbTx = evoDb.BeginTransaction();
        WriteMinedCommitment(evoDb, qc_a, mined_hash_a, mined_height, /*quorum_height=*/0);
        dbTx->Commit();
    }

    CBlockIndex pindex_mined;
    pindex_mined.nHeight = mined_height;
    pindex_mined.pprev = const_cast<CBlockIndex*>(pindex_genesis);
    pindex_mined.phashBlock = &mined_hash_a;

    CBlock block_with_qc = MakeEmptyBlock();
    block_with_qc.vtx.emplace_back(MakeCommitmentTx(qc_a, mined_height));
    const CBlock empty_block = MakeEmptyBlock();

    ExpectQuorumMerkleRoot(empty_block, &pindex_mined, qblockman, qc_a);

    {
        LOCK(cs_main);
        auto dbTx = evoDb.BeginTransaction();
        BOOST_REQUIRE(qblockman.UndoBlock(m_node.chainman->ActiveChainstate(), block_with_qc, &pindex_mined));
        // Install the replacement while the disconnect transaction is still open.
        WriteMinedCommitment(evoDb, qc_b, mined_hash_b, mined_height, /*quorum_height=*/0);
        dbTx->Commit();
    }

    ExpectQuorumMerkleRoot(empty_block, &pindex_mined, qblockman, qc_b);
}

// Certificates installed through the public manager model already-authenticated ingress.
// This fixture does not construct a signing quorum or a network certificate.
BOOST_FIXTURE_TEST_CASE(chainlock_shortcuts_bind_candidate_ancestry, RegTestingSetup)
{
    const auto& params = Params().GetConsensus();
    const auto& chain = *WITH_LOCK(cs_main, return &m_node.chainman->ActiveChain());
    auto& chainlocks = *m_node.chainlocks;
    auto& qman = *m_node.llmq_ctx->qman;
    std::vector<CBlockIndex> indexes(6);
    std::vector<uint256> hashes(indexes.size());
    for (size_t i = 0; i < indexes.size(); ++i) {
        hashes[i] = GetTestBlockHash(i);
        indexes[i].nHeight = i;
        indexes[i].phashBlock = &hashes[i];
        if (i) indexes[i].pprev = &indexes[i - 1];
        indexes[i].BuildSkip();
    }
    for (const int height : {4, 2}) {
        for (const bool same_branch : {false, true}) {
            chainlocks.ResetChainlock();
            const auto cert = CreateChainLock(height, same_branch ? hashes[height] : GetTestBlockHash(100));
            BOOST_REQUIRE(chainlocks.UpdateBestChainlock(SerializeHash(cert), cert, nullptr));
            CCbTx cbtx;
            cbtx.nVersion = CCbTx::Version::CLSIG_AND_BALANCE;
            cbtx.bestCLHeightDiff = indexes.back().nHeight - height - 1;
            cbtx.bestCLSignature = cert.getSig();
            BlockValidationState historical;
            BOOST_CHECK(CheckCbTxBestChainlock(cbtx, &indexes.back(), params, chain, qman, chainlocks, false, historical));
            BlockValidationState bound;
            BOOST_CHECK_EQUAL(CheckCbTxBestChainlock(cbtx, &indexes.back(), params, chain, qman, chainlocks, true, bound),
                              same_branch);
            if (!same_branch) BOOST_CHECK_EQUAL(bound.GetRejectReason(), "bad-cbtx-invalid-clsig");
            uint32_t miner_diff{0};
            CBLSSignature miner_sig;
            BOOST_CHECK(node::CalcCbTxBestChainlock(chainlocks, indexes.back().pprev, miner_diff, miner_sig, false));
            BOOST_CHECK_EQUAL(node::CalcCbTxBestChainlock(chainlocks, indexes.back().pprev, miner_diff, miner_sig, true),
                              same_branch);
            if (same_branch) {
                BOOST_CHECK_EQUAL(miner_diff, cbtx.bestCLHeightDiff);
                BOOST_CHECK(miner_sig == cert.getSig());
            }
        }
    }
}

struct BranchBindingSetup : RegTestingSetup {
    BranchBindingSetup() :
        RegTestingSetup({"-vbparams=chainlock_branch_binding:-1:-1"})
    {
    }
};

static void CheckTreasuryWaiver(RegTestingSetup& setup, bool enforce_branch_binding)
{
    constexpr const char* REGTEST_SPORK_PRIVKEY{"cP4EKFyJsHT39LDqgdcB43Y3YXjNyjb5Fuas1GQSeAtjnZWmZEQK"};
    auto& helper = *setup.m_node.chain_helper;
    auto& chainlocks = *setup.m_node.chainlocks;
    const auto hash = GetTestBlockHash(30);
    const auto other_hash = GetTestBlockHash(31);
    CBlockIndex candidate;
    candidate.nHeight = 5;
    candidate.phashBlock = &hash;
    const bool active = DeploymentActiveAt(candidate, *setup.m_node.chainman,
                                           Consensus::DEPLOYMENT_CHAINLOCK_BRANCH_BINDING);
    BOOST_REQUIRE_EQUAL(active, enforce_branch_binding);
    CBlockIndex other;
    other.nHeight = 5;
    other.phashBlock = &other_hash;
    BOOST_CHECK(!helper.IsSuperblockValidationRequired(&candidate, active)); // independent unsynced waiver
    BOOST_REQUIRE(setup.m_node.netfulfilledman->LoadCache(/*load_cache=*/true));
    setup.m_node.mn_sync->SwitchToNextAsset();
    setup.m_node.mn_sync->SwitchToNextAsset();
    BOOST_REQUIRE(setup.m_node.mn_sync->IsSynced());
    BOOST_REQUIRE(setup.m_node.sporkman->SetSporkAddress(Params().SporkAddress()));
    BOOST_REQUIRE(setup.m_node.sporkman->SetPrivKey(REGTEST_SPORK_PRIVKEY));
    BOOST_REQUIRE(
        setup.m_node.sporkman->UpdateSpork(SPORK_19_CHAINLOCKS_ENABLED, std::numeric_limits<int64_t>::max()).has_value());
    BOOST_REQUIRE(!chainlocks.IsEnabled());
    BOOST_CHECK(helper.IsSuperblockValidationRequired(&candidate, active));
    const auto descendant_hash = GetTestBlockHash(34);
    const auto other_descendant_hash = GetTestBlockHash(35);
    CBlockIndex descendant;
    descendant.nHeight = 6;
    descendant.phashBlock = &descendant_hash;
    descendant.pprev = &candidate;
    CBlockIndex other_descendant;
    other_descendant.nHeight = 6;
    other_descendant.phashBlock = &other_descendant_hash;
    other_descendant.pprev = &other;
    for (const CBlockIndex* certified : {&other, &candidate, &other_descendant, &descendant}) {
        chainlocks.ResetChainlock();
        const auto cert = CreateChainLock(certified->nHeight, certified->GetBlockHash());
        BOOST_REQUIRE(chainlocks.UpdateBestChainlock(SerializeHash(cert), cert, certified));
        BOOST_CHECK_EQUAL(helper.IsSuperblockValidationRequired(&candidate, active),
                          enforce_branch_binding && certified != &candidate && certified != &descendant);
        const auto unknown = CreateChainLock(7, GetTestBlockHash(32));
        BOOST_REQUIRE(chainlocks.UpdateBestChainlock(SerializeHash(unknown), unknown, nullptr));
        BOOST_CHECK_EQUAL(helper.IsSuperblockValidationRequired(&candidate, active),
                          enforce_branch_binding && certified != &candidate && certified != &descendant);
    }
    chainlocks.ResetChainlock();
    const auto unknown = CreateChainLock(7, GetTestBlockHash(33));
    BOOST_REQUIRE(chainlocks.UpdateBestChainlock(SerializeHash(unknown), unknown, nullptr));
    BOOST_CHECK_EQUAL(helper.IsSuperblockValidationRequired(&candidate, active), enforce_branch_binding);
}

BOOST_FIXTURE_TEST_CASE(treasury_waiver_requires_known_candidate_ancestry, BranchBindingSetup)
{
    CheckTreasuryWaiver(*this, true);
}

BOOST_FIXTURE_TEST_CASE(treasury_waiver_preserves_historical_height_rule, RegTestingSetup)
{
    CheckTreasuryWaiver(*this, false);
}

BOOST_AUTO_TEST_SUITE_END()
