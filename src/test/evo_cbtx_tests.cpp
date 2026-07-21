// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <test/util/llmq_tests.h>
#include <test/util/setup_common.h>

#include <bls/bls.h>
#include <chain.h>
#include <chainlock/chainlock.h>
#include <chainparams.h>
#include <compat/endian.h>
#include <consensus/merkle.h>
#include <consensus/validation.h>
#include <evo/cbtx.h>
#include <evo/cbtx_cache.h>
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
#include <uint256.h>
#include <validation.h>

#include <cstdint>
#include <limits>
#include <string>
#include <tuple>
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
    BOOST_CHECK(!CheckCbTxBestChainlock(cbTx, &pindex, consensus_params, chain, qman, chainlocks, state));
    BOOST_CHECK_EQUAL(state.GetRejectReason(), "bad-cbtx-cldiff");

    // Upper boundary: uint32_t max.
    cbTx.bestCLHeightDiff = std::numeric_limits<uint32_t>::max();
    BlockValidationState state_big;
    BOOST_CHECK(!CheckCbTxBestChainlock(cbTx, &pindex, consensus_params, chain, qman, chainlocks, state_big));
    BOOST_CHECK_EQUAL(state_big.GetRejectReason(), "bad-cbtx-cldiff");
}

namespace {
// Mirrors private DB keys in llmq/blockprocessor.cpp so tests can install
// mined-commitment state without a full DKG/mining path.
static const std::string DB_MINED_COMMITMENT = "q_mc";
static const std::string DB_MINED_COMMITMENT_BY_INVERSED_HEIGHT = "q_mcih";

std::tuple<std::string, Consensus::LLMQType, uint32_t> BuildInversedHeightKey(Consensus::LLMQType llmqType, int nMinedHeight)
{
    return std::make_tuple(DB_MINED_COMMITMENT_BY_INVERSED_HEIGHT, llmqType,
                           htobe32_internal(std::numeric_limits<uint32_t>::max() - nMinedHeight));
}

// Store a mined commitment as if it was mined at `mined_height` for the genesis
// quorum base (quorumHeight 0). GetMinedCommitmentsUntilBlock iterates inverted-
// height keys in [pindex->nHeight, 0), so scan height must be >= mined_height
// and mined_height must be > 0 for the entry to be returned.
void WriteMinedCommitment(CEvoDB& evoDb, const CFinalCommitment& qc, const uint256& mined_block_hash, int mined_height)
{
    assert(mined_height > 0);
    evoDb.Write(std::make_pair(DB_MINED_COMMITMENT, std::make_pair(qc.llmqType, qc.quorumHash)),
                std::make_pair(qc, mined_block_hash));
    evoDb.Write(BuildInversedHeightKey(qc.llmqType, mined_height), /*quorumHeight=*/0);
}

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

struct QcHashCacheCleanupGuard {
    ~QcHashCacheCleanupGuard() { InvalidateCachedQcHashes(); }
};
} // anonymous namespace

// Outer cache keys on active quorum base blocks; inner LRU keys on base hashes.
// Neither includes the mined CFinalCommitment, so without InvalidateCachedQcHashes
// a stale hash survives an evoDb commitment swap for the same base list.
BOOST_FIXTURE_TEST_CASE(qc_hash_cache_invalidated_on_commitment_branch_change, RegTestingSetup)
{
    InvalidateCachedQcHashes();
    const QcHashCacheCleanupGuard cache_cleanup;

    auto& evoDb = *Assert(m_node.evodb);
    auto& qblockman = *Assert(m_node.llmq_ctx)->quorum_block_processor;
    const auto& params = GetLLMQParams(Consensus::LLMQType::LLMQ_TEST);

    const CBlockIndex* pindex_genesis = GenesisIndex(m_node);
    BOOST_REQUIRE(pindex_genesis != nullptr);
    const uint256 quorum_hash = pindex_genesis->GetBlockHash();

    const CFinalCommitment qc_a = MakeDistinctCommitment(params, quorum_hash, /*salt=*/0x11);
    const CFinalCommitment qc_b = MakeDistinctCommitment(params, quorum_hash, /*salt=*/0x22);
    BOOST_REQUIRE(::SerializeHash(qc_a) != ::SerializeHash(qc_b));

    const uint256 mined_hash_a = GetTestBlockHash(1);
    const uint256 mined_hash_b = GetTestBlockHash(2);
    const uint256 scan_hash = GetTestBlockHash(3);
    constexpr int mined_height = 1;

    CBlockIndex pindex_scan;
    pindex_scan.nHeight = mined_height;
    pindex_scan.pprev = const_cast<CBlockIndex*>(pindex_genesis);
    pindex_scan.phashBlock = &scan_hash;

    {
        auto dbTx = evoDb.BeginTransaction();
        WriteMinedCommitment(evoDb, qc_a, mined_hash_a, mined_height);
        dbTx->Commit();
    }

    const CBlock block = MakeEmptyBlock();
    ExpectQuorumMerkleRoot(block, &pindex_scan, qblockman, qc_a);

    // Swap evoDb to commitment B without changing the active base-block list.
    {
        auto dbTx = evoDb.BeginTransaction();
        WriteMinedCommitment(evoDb, qc_b, mined_hash_b, mined_height);
        dbTx->Commit();
    }

    // Without invalidation, both cache layers still serve commitment A.
    {
        uint256 merkle_root;
        BlockValidationState state;
        BOOST_REQUIRE(CalcCbTxMerkleRootQuorums(block, &pindex_scan, qblockman, merkle_root, state));
        BOOST_CHECK_EQUAL(merkle_root.ToString(), CalcQuorumMerkleRootForCommitment(qc_a).ToString());
        BOOST_CHECK(merkle_root != CalcQuorumMerkleRootForCommitment(qc_b));
    }

    InvalidateCachedQcHashes();
    ExpectQuorumMerkleRoot(block, &pindex_scan, qblockman, qc_b);
}

// UndoBlock must invalidate the process-lifetime caches so a replacement
// commitment is observed after disconnect.
//
// Activate DIP0003 immediately so GetCommitmentsFromBlock accepts the payload
// at a low height without a long fake chain.
struct Dip3ActiveSetup : public RegTestingSetup {
    Dip3ActiveSetup() :
        RegTestingSetup({"-dip3params=1:1"})
    {
    }
};

BOOST_FIXTURE_TEST_CASE(qc_hash_cache_invalidated_by_undoblock, Dip3ActiveSetup)
{
    InvalidateCachedQcHashes();
    const QcHashCacheCleanupGuard cache_cleanup;

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
        auto dbTx = evoDb.BeginTransaction();
        WriteMinedCommitment(evoDb, qc_a, mined_hash_a, mined_height);
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
        BOOST_REQUIRE(qblockman.UndoBlock(block_with_qc, &pindex_mined));
        // Install the replacement while the disconnect transaction is still open.
        WriteMinedCommitment(evoDb, qc_b, mined_hash_b, mined_height);
        dbTx->Commit();
    }

    ExpectQuorumMerkleRoot(empty_block, &pindex_mined, qblockman, qc_b);
}

BOOST_AUTO_TEST_SUITE_END()
