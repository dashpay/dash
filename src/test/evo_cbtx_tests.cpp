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
#include <evo/evodb.h>
#include <evo/specialtx.h>
#include <evo/specialtxman.h>
#include <hash.h>
#include <llmq/blockprocessor.h>
#include <llmq/commitment.h>
#include <llmq/context.h>
#include <llmq/params.h>
#include <primitives/block.h>
#include <primitives/transaction.h>
#include <streams.h>
#include <uint256.h>
#include <validation.h>

#include <algorithm>
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
    cbTx.bestCLSignature.Set(valid_sig, legacy_scheme);

    // bestCLHeightDiff == nHeight: lower boundary of the rejected range.
    cbTx.bestCLHeightDiff = static_cast<uint32_t>(pindex.nHeight);
    BlockValidationState state;
    BOOST_CHECK(
        !CheckCbTxBestChainlock(cbTx, &pindex, consensus_params, chain, qman, chainlocks, *m_node.special_tx, state));
    BOOST_CHECK_EQUAL(state.GetRejectReason(), "bad-cbtx-cldiff");

    // Upper boundary: uint32_t max.
    cbTx.bestCLHeightDiff = std::numeric_limits<uint32_t>::max();
    BlockValidationState state_big;
    BOOST_CHECK(!CheckCbTxBestChainlock(cbTx, &pindex, consensus_params, chain, qman, chainlocks, *m_node.special_tx,
                                        state_big));
    BOOST_CHECK_EQUAL(state_big.GetRejectReason(), "bad-cbtx-cldiff");

    // The signature is deserialized lazily, so the payload parser accepts any bytes in its
    // place and CheckCbTx rejects an encoding that is not a signature; the valid signature
    // above and the null encoding pass.
    TxValidationState tx_state;
    BOOST_CHECK(CheckCbTx(cbTx, /*pindexPrev=*/nullptr, /*is_v24_active=*/false, tx_state));
    std::vector<uint8_t> sig_bytes(CBLSSignature::SerSize, 0xff);
    CDataStream(sig_bytes, SER_NETWORK, PROTOCOL_VERSION) >> cbTx.bestCLSignature;
    BOOST_CHECK(!CheckCbTx(cbTx, /*pindexPrev=*/nullptr, /*is_v24_active=*/false, tx_state));
    BOOST_CHECK_EQUAL(tx_state.GetRejectReason(), "bad-cbtx-payload");
    std::fill(sig_bytes.begin(), sig_bytes.end(), 0);
    CDataStream(sig_bytes, SER_NETWORK, PROTOCOL_VERSION) >> cbTx.bestCLSignature;
    TxValidationState null_state;
    BOOST_CHECK(CheckCbTx(cbTx, /*pindexPrev=*/nullptr, /*is_v24_active=*/false, null_state));
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

BOOST_AUTO_TEST_SUITE_END()
