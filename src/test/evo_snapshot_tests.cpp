// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <test/util/setup_common.h>

#include <clientversion.h>
#include <consensus/amount.h>
#include <consensus/merkle.h>
#include <evo/cbtx.h>
#include <evo/chainhelper.h>
#include <evo/creditpool.h>
#include <evo/evodb.h>
#include <evo/mnhftx.h>
#include <evo/netinfo.h>
#include <evo/snapshot.h>
#include <hash.h>
#include <llmq/blockprocessor.h>
#include <llmq/context.h>
#include <llmq/quorumsman.h>
#include <llmq/signhash.h>
#include <llmq/snapshot.h>
#include <llmq/utils.h>
#include <masternode/meta.h>
#include <node/context.h>
#include <streams.h>
#include <validation.h>
#include <versionbits.h>

#include <algorithm>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <boost/test/unit_test.hpp>

namespace {

uint256 H(uint8_t value)
{
    uint256 hash;
    hash.begin()[0] = value;
    return hash;
}

uint256 CollidingH(uint8_t suffix)
{
    uint256 hash;
    std::fill_n(hash.begin(), 8, 0xa5);
    hash.begin()[8] = suffix;
    return hash;
}

uint160 H160(uint8_t value)
{
    uint160 hash;
    hash.begin()[0] = value;
    return hash;
}

CDeterministicMNCPtr MN(uint64_t internal_id, uint8_t hash_suffix, MnType type, int version, uint8_t address_tag)
{
    auto state{std::make_shared<CDeterministicMNState>()};
    state->nVersion = version;
    state->nRegisteredHeight = 10 + internal_id;
    state->nLastPaidHeight = 20 + internal_id;
    state->nPoSePenalty = internal_id;
    state->keyIDOwner = CKeyID{H160(address_tag)};
    state->keyIDVoting = CKeyID{H160(address_tag + 20)};
    state->scriptPayout = CScript{} << OP_RETURN << std::vector<unsigned char>{address_tag, 1};
    state->scriptOperatorPayout = CScript{} << OP_RETURN << std::vector<unsigned char>{address_tag, 2};
    state->netInfo = NetInfoInterface::MakeNetInfo(version);
    BOOST_REQUIRE_EQUAL(state->netInfo->AddEntry(NetInfoPurpose::CORE_P2P,
                                                strprintf("1.1.1.%d:%d", address_tag, Params().GetDefaultPort())),
                        NetInfoStatus::Success);
    if (type == MnType::Evo) {
        state->platformNodeID = H160(address_tag + 40);
        BOOST_REQUIRE_EQUAL(state->netInfo->AddEntry(NetInfoPurpose::PLATFORM_P2P,
                                                    strprintf("2.2.2.%d:26657", address_tag)),
                            NetInfoStatus::Success);
        BOOST_REQUIRE_EQUAL(state->netInfo->AddEntry(NetInfoPurpose::PLATFORM_HTTPS,
                                                    strprintf("evo%d.example.org:443", address_tag)),
                            NetInfoStatus::Success);
    }

    auto dmn{std::make_shared<CDeterministicMN>(internal_id, type)};
    dmn->proTxHash = CollidingH(hash_suffix);
    dmn->collateralOutpoint = COutPoint(H(address_tag + 80), internal_id);
    dmn->nOperatorReward = address_tag * 10;
    state->UpdateConfirmedHash(dmn->proTxHash, H(address_tag + 100));
    dmn->pdmnState = std::move(state);
    return dmn;
}

CDeterministicMNList MNList(const uint256& block_hash, int height, bool reverse)
{
    CDeterministicMNList list{block_hash, height, 10};
    std::vector<CDeterministicMNCPtr> mns{
        MN(2, 3, MnType::Regular, ProTxVersion::LegacyBLS, 3),
        MN(5, 1, MnType::Evo, ProTxVersion::ExtAddr, 5),
        MN(7, 2, MnType::Regular, ProTxVersion::LegacyBLS, 7),
    };
    if (reverse) std::reverse(mns.begin(), mns.end());
    for (const auto& dmn : mns) list.AddMN(dmn, /*fBumpTotalCount=*/false);
    return list;
}

evo::CMinedQuorumCommitment Commitment(Consensus::LLMQType type, uint8_t quorum, uint8_t mined, bool rotated,
                                       int16_t index = 0)
{
    llmq::CFinalCommitment commitment;
    commitment.nVersion = rotated ? llmq::CFinalCommitment::BASIC_BLS_INDEXED_QUORUM_VERSION
                                  : llmq::CFinalCommitment::BASIC_BLS_NON_INDEXED_QUORUM_VERSION;
    commitment.llmqType = type;
    commitment.quorumHash = H(quorum);
    commitment.quorumIndex = index;
    const auto& params{evo::SnapshotLLMQParams(type)};
    commitment.signers.resize(params.size);
    commitment.validMembers.resize(params.size);
    return {H(quorum), H(quorum + 120), std::move(commitment), H(mined)};
}

evo::CEvoSnapshot SyntheticSnapshot(bool reverse_representation = false)
{
    evo::CEvoSnapshot snapshot;
    snapshot.base_block_hash = H(42);
    snapshot.mn_list = MNList(snapshot.base_block_hash, 500, reverse_representation);
    snapshot.credit_pool.locked = 123456;
    snapshot.credit_pool.currentLimit = 700;
    snapshot.credit_pool.latelyUnlocked = 11;
    if (reverse_representation) {
        BOOST_REQUIRE(snapshot.credit_pool.indexes.Add(15));
        BOOST_REQUIRE(snapshot.credit_pool.indexes.Add(8));
        BOOST_REQUIRE(snapshot.credit_pool.indexes.Add(7));
        BOOST_REQUIRE(snapshot.credit_pool.indexes.Add(9));
        BOOST_REQUIRE(snapshot.credit_pool.indexes.Remove(9));
        snapshot.mnhf_signals.emplace(9, 30);
        snapshot.mnhf_signals.emplace(2, 12);
    } else {
        BOOST_REQUIRE(snapshot.credit_pool.indexes.Add(7));
        BOOST_REQUIRE(snapshot.credit_pool.indexes.Add(8));
        BOOST_REQUIRE(snapshot.credit_pool.indexes.Add(15));
        snapshot.mnhf_signals.emplace(2, 12);
        snapshot.mnhf_signals.emplace(9, 30);
    }

    evo::CQuorumSnapshotData plain;
    plain.llmq_type = Consensus::LLMQType::LLMQ_TEST;
    plain.active_commitments = {Commitment(plain.llmq_type, 11, 51, false), Commitment(plain.llmq_type, 12, 52, false)};
    plain.safety_commitments = {Commitment(plain.llmq_type, 10, 50, false)};

    evo::CQuorumSnapshotData rotated;
    rotated.llmq_type = Consensus::LLMQType::LLMQ_TEST_DIP0024;
    rotated.rotation_enabled = true;
    rotated.active_commitments = {Commitment(rotated.llmq_type, 31, 71, true, 0),
                                  Commitment(rotated.llmq_type, 32, 72, true, 1)};
    rotated.safety_commitments = {Commitment(rotated.llmq_type, 21, 61, true, 0),
                                  Commitment(rotated.llmq_type, 22, 62, true, 1)};
    for (uint8_t i{1}; i <= evo::EVO_SNAPSHOT_ROTATION_CYCLES; ++i) {
        const auto mode{i == 2 ? SnapshotSkipMode::MODE_SKIPPING_ENTRIES : SnapshotSkipMode::MODE_NO_SKIPPING};
        rotated.rotation_snapshots.push_back(
            {H(40 + i), H(100 + i), llmq::CQuorumSnapshot{{true, false, true, false}, mode, i == 2 ? std::vector<int>{1} : std::vector<int>{}}});
    }

    snapshot.quorums = {std::move(plain), std::move(rotated)};
    std::set<uint256> work_hashes;
    std::set<std::pair<Consensus::LLMQType, uint256>> modifier_keys;
    for (const auto& data : snapshot.quorums) {
        for (const auto* commitments : {&data.active_commitments, &data.safety_commitments}) {
            for (const auto& entry : *commitments) {
                work_hashes.insert(entry.work_block_hash);
                modifier_keys.emplace(data.llmq_type, entry.work_block_hash);
            }
        }
        for (const auto& entry : data.rotation_snapshots) {
            work_hashes.insert(entry.work_block_hash);
            modifier_keys.emplace(data.llmq_type, entry.work_block_hash);
        }
    }
    CDeterministicMNList previous{snapshot.mn_list};
    uint256 previous_hash{snapshot.base_block_hash};
    int height{499};
    for (const auto& work_hash : work_hashes) {
        auto list{MNList(work_hash, height--, reverse_representation)};
        snapshot.historical_mn_list_diffs.push_back({previous_hash, work_hash, list.GetHeightForSnapshotCodec(),
                                                     list.GetTotalRegisteredCount(), evo::CanonicalMNListHash(list),
                                                     previous.BuildDiff(list)});
        previous_hash = work_hash;
        previous = std::move(list);
    }
    for (const auto& [type, work_hash] : modifier_keys) {
        snapshot.quorum_modifiers.push_back({type, work_hash, H(static_cast<uint8_t>(150 + snapshot.quorum_modifiers.size()))});
    }
    if (reverse_representation) {
        std::reverse(snapshot.quorums.begin(), snapshot.quorums.end());
        std::reverse(snapshot.historical_mn_list_diffs.begin(), snapshot.historical_mn_list_diffs.end());
        std::reverse(snapshot.quorum_modifiers.begin(), snapshot.quorum_modifiers.end());
        for (auto& data : snapshot.quorums) {
            std::reverse(data.active_commitments.begin(), data.active_commitments.end());
            std::reverse(data.safety_commitments.begin(), data.safety_commitments.end());
            std::reverse(data.rotation_snapshots.begin(), data.rotation_snapshots.end());
        }
    }
    return snapshot;
}

CDataStream SerializeSnapshot(const evo::CEvoSnapshot& snapshot)
{
    CDataStream stream{SER_DISK, CLIENT_VERSION};
    stream << snapshot;
    return stream;
}

void CheckInvalid(evo::CEvoSnapshot snapshot)
{
    BOOST_CHECK_THROW(snapshot.Validate(), std::ios_base::failure);
}

//! Restores consensus params mutated through const_cast when the test case
//! leaves scope, including through a failed BOOST_REQUIRE, so mutated state
//! cannot leak into cases running later in the same process.
class [[nodiscard]] ConsensusParamsRestorer
{
    Consensus::Params& m_params;
    const Consensus::Params m_saved;

public:
    explicit ConsensusParamsRestorer(const Consensus::Params& params) :
        m_params{const_cast<Consensus::Params&>(params)}, m_saved{params}
    {
    }
    ~ConsensusParamsRestorer() { m_params = m_saved; }
    Consensus::Params& Get() { return m_params; }
};

} // namespace

//! Chain fixture whose activation heights are already in force while the chain
//! is mined, so every historical coinbase is the CbTx that v20-era code paths
//! (e.g. the quorum hash modifier's chainlock probe) are entitled to assume.
//! Forcing the heights down through const_cast after mining instead would leave
//! pre-DIP3 coinbases on a chain claiming v20 was always active, which trips
//! GetTxPayload's payload-type assertion in debug builds.
struct SnapshotActivationChainSetup : public TestChainSetup {
    SnapshotActivationChainSetup() :
        TestChainSetup{102, CBaseChainParams::REGTEST,
                       {"-dip3params=2:2", "-testactivationheight=v20@2", "-testactivationheight=mn_rr@2"}}
    {
    }
};

BOOST_AUTO_TEST_SUITE(evo_snapshot_tests)

BOOST_FIXTURE_TEST_CASE(populated_roundtrip_and_representation_independence, BasicTestingSetup)
{
    const auto forward{SyntheticSnapshot()};
    const auto reverse{SyntheticSnapshot(/*reverse_representation=*/true)};
    const auto forward_bytes{SerializeSnapshot(forward)};
    const auto reverse_bytes{SerializeSnapshot(reverse)};
    BOOST_CHECK_EQUAL_COLLECTIONS(forward_bytes.begin(), forward_bytes.end(), reverse_bytes.begin(), reverse_bytes.end());
    BOOST_CHECK(evo::CanonicalMNListHash(forward.mn_list) == evo::CanonicalMNListHash(reverse.mn_list));
    BOOST_CHECK(GetEvoSnapshotHash(forward) == GetEvoSnapshotHash(reverse));

    CDataStream input{forward_bytes};
    evo::CEvoSnapshot decoded;
    input >> decoded;
    BOOST_CHECK(input.empty());
    const auto decoded_bytes{SerializeSnapshot(decoded)};
    BOOST_CHECK_EQUAL_COLLECTIONS(forward_bytes.begin(), forward_bytes.end(), decoded_bytes.begin(), decoded_bytes.end());
    BOOST_CHECK(evo::CanonicalMNListHash(decoded.mn_list) == evo::CanonicalMNListHash(forward.mn_list));
    BOOST_CHECK_EQUAL(decoded.mn_list.GetCounts().total(), 3U);
    BOOST_CHECK_EQUAL(decoded.historical_mn_list_diffs.size(), forward.historical_mn_list_diffs.size());
    BOOST_CHECK(decoded.credit_pool.indexes.Contains(7));
    BOOST_CHECK(decoded.credit_pool.indexes.Contains(8));
    BOOST_CHECK(decoded.credit_pool.indexes.Contains(15));
    BOOST_CHECK(decoded.mnhf_signals == forward.mnhf_signals);

    for (const auto internal_id : {2U, 5U, 7U}) {
        const auto original{forward.mn_list.GetMNByInternalId(internal_id)};
        BOOST_REQUIRE(original);
        const auto by_hash{decoded.mn_list.GetMN(original->proTxHash)};
        const auto by_id{decoded.mn_list.GetMNByInternalId(internal_id)};
        const auto by_collateral{decoded.mn_list.GetUniquePropertyMN(original->collateralOutpoint)};
        const auto by_owner{decoded.mn_list.GetUniquePropertyMN(original->pdmnState->keyIDOwner)};
        const auto by_service{decoded.mn_list.GetMNByService(original->pdmnState->netInfo->GetPrimary())};
        BOOST_REQUIRE(by_hash);
        BOOST_REQUIRE(by_id);
        BOOST_REQUIRE(by_collateral);
        BOOST_REQUIRE(by_owner);
        BOOST_REQUIRE(by_service);
        BOOST_CHECK(by_hash->proTxHash == original->proTxHash);
        BOOST_CHECK(by_id->proTxHash == original->proTxHash);
        BOOST_CHECK(by_collateral->proTxHash == original->proTxHash);
        BOOST_CHECK(by_owner->proTxHash == original->proTxHash);
        BOOST_CHECK(by_service->proTxHash == original->proTxHash);
    }
}

BOOST_FIXTURE_TEST_CASE(snapshot_identity_seeding_is_retrievable, TestChain100Setup)
{
    const CBlockIndex* base{WITH_LOCK(::cs_main, return m_node.chainman->ActiveTip())};
    BOOST_REQUIRE(base != nullptr);
    const auto list{MNList(base->GetBlockHash(), base->nHeight, false)};
    const CBlockIndex* historical_index{base->GetAncestor(50)};
    const auto historical_list{MNList(historical_index->GetBlockHash(), historical_index->nHeight, true)};
    const auto indexed_commitment = [&](Consensus::LLMQType type, int quorum_height, int mined_height,
                                        bool rotated, int16_t quorum_index = 0) {
        auto entry{Commitment(type, 1, 2, rotated, quorum_index)};
        entry.quorum_base_block_hash = base->GetAncestor(quorum_height)->GetBlockHash();
        entry.commitment.quorumHash = entry.quorum_base_block_hash;
        entry.mined_block_hash = base->GetAncestor(mined_height)->GetBlockHash();
        return entry;
    };
    const std::vector nonrotated{
        indexed_commitment(Consensus::LLMQType::LLMQ_TEST, 48, 58, false),
        indexed_commitment(Consensus::LLMQType::LLMQ_TEST, 72, 82, false),
    };
    const std::vector rotated{
        indexed_commitment(Consensus::LLMQType::LLMQ_TEST_DIP0024, 72, 84, true, 0),
        indexed_commitment(Consensus::LLMQType::LLMQ_TEST_DIP0024, 73, 85, true, 1),
    };
    CCreditPool pool;
    pool.locked = 123;
    pool.currentLimit = 45;
    pool.latelyUnlocked = 6;
    AbstractEHFManager::Signals signals{{2, base->nHeight}};
    llmq::CQuorumSnapshot quorum_snapshot{{true, false, true}, SnapshotSkipMode::MODE_NO_SKIPPING, {}};

    ConsensusParamsRestorer params_restorer{Params().GetConsensus()};
    const int old_dip3_height{params_restorer.Get().DIP0003Height};
    params_restorer.Get().DIP0003Height = 1;
    BOOST_CHECK_EQUAL(m_node.dmnman->GetListForBlock(base).GetCounts().total(), 0U);
    BOOST_CHECK_EQUAL(m_node.dmnman->GetListForBlock(historical_index).GetCounts().total(), 0U);

    {
        auto tx{m_node.evodb->BeginTransaction(EvoDbIdentity::SNAPSHOT)};
        BOOST_REQUIRE(m_node.dmnman->SeedListForBlock(list));
        BOOST_REQUIRE(m_node.dmnman->SeedListForBlock(historical_list));
        {
            LOCK(::cs_main);
            for (const auto& entry : nonrotated) {
                BOOST_REQUIRE(m_node.llmq_ctx->quorum_block_processor->SeedMinedCommitment(
                    entry.commitment.llmqType, entry.quorum_base_block_hash,
                    entry.commitment, entry.mined_block_hash));
            }
            for (const auto& entry : rotated) {
                BOOST_REQUIRE(m_node.llmq_ctx->quorum_block_processor->SeedMinedCommitment(
                    entry.commitment.llmqType, entry.quorum_base_block_hash,
                    entry.commitment, entry.mined_block_hash));
            }
        }
        BOOST_REQUIRE(m_node.llmq_ctx->qsnapman->SeedSnapshotForBlock(
            Consensus::LLMQType::LLMQ_TEST, base, quorum_snapshot));
        BOOST_REQUIRE(m_node.chain_helper->credit_pool_manager->SeedSnapshot(base, pool));
        BOOST_REQUIRE(m_node.chain_helper->ehf_manager->SeedSignals(base, signals));
        tx->Commit();
    }
    BOOST_REQUIRE(m_node.evodb->CommitRootTransaction(EvoDbIdentity::SNAPSHOT, /*sync=*/true));
    {
        LOCK(::cs_main);
        m_node.dmnman->InvalidateListCacheForBlock(base->GetBlockHash());
        m_node.dmnman->InvalidateListCacheForBlock(historical_index->GetBlockHash());
    }

    CDeterministicMNList stored_list;
    CDeterministicMNList stored_historical_list;
    {
        auto tx{m_node.evodb->BeginTransaction(EvoDbIdentity::SNAPSHOT)};
        stored_list = m_node.dmnman->GetListForBlock(base);
        stored_historical_list = m_node.dmnman->GetListForBlock(historical_index);
    }
    m_node.dmnman->InvalidateListCacheForBlock(base->GetBlockHash());
    m_node.dmnman->InvalidateListCacheForBlock(historical_index->GetBlockHash());
    const auto subsequent_list{m_node.dmnman->GetListForBlock(base)};
    const auto subsequent_historical_list{m_node.dmnman->GetListForBlock(historical_index)};
    params_restorer.Get().DIP0003Height = old_dip3_height;
    BOOST_CHECK(evo::CanonicalMNListHash(stored_list) == evo::CanonicalMNListHash(list));
    BOOST_CHECK(evo::CanonicalMNListHash(stored_historical_list) == evo::CanonicalMNListHash(historical_list));
    BOOST_CHECK(evo::CanonicalMNListHash(subsequent_list) == evo::CanonicalMNListHash(list));
    BOOST_CHECK(evo::CanonicalMNListHash(subsequent_historical_list) == evo::CanonicalMNListHash(historical_list));
    const auto [stored_commitment, stored_mined_hash]{
        m_node.llmq_ctx->quorum_block_processor->GetMinedCommitment(
            nonrotated.back().commitment.llmqType, nonrotated.back().quorum_base_block_hash)};
    BOOST_CHECK_EQUAL(stored_mined_hash, nonrotated.back().mined_block_hash);
    BOOST_CHECK_EQUAL(SerializeHash(stored_commitment), SerializeHash(nonrotated.back().commitment));
    {
        LOCK(::cs_main);
        const auto plain{m_node.llmq_ctx->quorum_block_processor->GetMinedCommitmentsUntilBlock(
            Consensus::LLMQType::LLMQ_TEST, base, 2)};
        BOOST_REQUIRE_EQUAL(plain.size(), 2U);
        BOOST_CHECK_EQUAL(plain[0]->nHeight, 72);
        BOOST_CHECK_EQUAL(plain[1]->nHeight, 48);
        const auto indexed{m_node.llmq_ctx->quorum_block_processor->GetLastMinedCommitmentsPerQuorumIndexUntilBlock(
            Consensus::LLMQType::LLMQ_TEST_DIP0024, base, 0)};
        BOOST_REQUIRE_EQUAL(indexed.size(), 2U);
        BOOST_CHECK_EQUAL(indexed[0]->nHeight, 72);
        BOOST_CHECK_EQUAL(indexed[1]->nHeight, 73);

        CBlock first_post_base_block;
        uint256 quorum_root;
        BlockValidationState state;
        BOOST_CHECK_MESSAGE(CalcCbTxMerkleRootQuorums(first_post_base_block, base,
                                *m_node.llmq_ctx->quorum_block_processor, quorum_root, state),
                            state.ToString());
    }
    const auto stored_snapshot{m_node.llmq_ctx->qsnapman->GetSnapshotForBlock(
        Consensus::LLMQType::LLMQ_TEST, base)};
    BOOST_REQUIRE(stored_snapshot.has_value());
    BOOST_CHECK(stored_snapshot->activeQuorumMembers == quorum_snapshot.activeQuorumMembers);

    CCreditPool stored_pool;
    AbstractEHFManager::Signals stored_signals;
    BOOST_REQUIRE(m_node.evodb->Read(std::make_pair(std::string{"cpm_S"}, base->GetBlockHash()), stored_pool));
    BOOST_REQUIRE(m_node.evodb->Read(std::make_pair(std::string{"mnhf_s2"}, base->GetBlockHash()), stored_signals));
    BOOST_CHECK_EQUAL(stored_pool.locked, pool.locked);
    BOOST_CHECK(stored_signals == signals);
}

BOOST_FIXTURE_TEST_CASE(snapshot_seed_rollback_does_not_publish_caches, TestChain100Setup)
{
    const CBlockIndex* base{WITH_LOCK(::cs_main, return m_node.chainman->ActiveTip())};
    BOOST_REQUIRE(base != nullptr);
    const auto seeded_list{MNList(base->GetBlockHash(), base->nHeight, false)};
    CCreditPool seeded_pool;
    seeded_pool.locked = 123;
    AbstractEHFManager::Signals seeded_signals{{2, base->nHeight}};
    const llmq::CQuorumSnapshot seeded_quorum{{true, false, true}, SnapshotSkipMode::MODE_NO_SKIPPING, {}};

    {
        auto tx{m_node.evodb->BeginTransaction(EvoDbIdentity::SNAPSHOT)};
        BOOST_REQUIRE(m_node.dmnman->SeedListForBlock(seeded_list));
        BOOST_REQUIRE(m_node.chain_helper->credit_pool_manager->SeedSnapshot(base, seeded_pool));
        BOOST_REQUIRE(m_node.chain_helper->ehf_manager->SeedSignals(base, seeded_signals));
        BOOST_REQUIRE(m_node.llmq_ctx->qsnapman->SeedSnapshotForBlock(
            Consensus::LLMQType::LLMQ_TEST, base, seeded_quorum));
        BOOST_CHECK(!WITH_LOCK(::cs_main, return m_node.llmq_ctx->quorum_block_processor->SeedMinedCommitment(
            Consensus::LLMQType::LLMQ_TEST, H(200),
            Commitment(Consensus::LLMQType::LLMQ_TEST, 1, 2, false).commitment, H(201))));
        // Destruction without Commit() rolls the complete scoped transaction back.
    }

    CDeterministicMNList db_list;
    CCreditPool db_pool;
    AbstractEHFManager::Signals db_signals;
    const auto quorum_hash{SerializeHash(std::make_pair(Consensus::LLMQType::LLMQ_TEST, base->GetBlockHash()))};
    llmq::CQuorumSnapshot db_quorum;
    BOOST_CHECK(!m_node.evodb->Read(std::make_pair(std::string{"dmn_S3"}, base->GetBlockHash()), db_list));
    BOOST_CHECK(!m_node.evodb->Read(std::make_pair(std::string{"cpm_S"}, base->GetBlockHash()), db_pool));
    BOOST_CHECK(!m_node.evodb->Read(std::make_pair(std::string{"mnhf_s2"}, base->GetBlockHash()), db_signals));
    BOOST_CHECK(!m_node.evodb->Read(std::make_pair(std::string_view{"llmq_S"}, quorum_hash), db_quorum));

    {
        ConsensusParamsRestorer params_restorer{Params().GetConsensus()};
        params_restorer.Get().DIP0003Height = 1;
        params_restorer.Get().V20Height = 1;
        BOOST_CHECK_EQUAL(m_node.dmnman->GetListForBlock(base).GetCounts().total(), 0U);
        BOOST_CHECK_EQUAL(m_node.chain_helper->credit_pool_manager->GetCreditPool(base).locked, 0);
        BOOST_CHECK(m_node.chain_helper->ehf_manager->GetSignalsStage(base).empty());
    }
    BOOST_CHECK(!m_node.llmq_ctx->qsnapman->GetSnapshotForBlock(
        Consensus::LLMQType::LLMQ_TEST, base).has_value());
}

BOOST_FIXTURE_TEST_CASE(quorum_members_reconstruct_from_seeded_state_only, SnapshotActivationChainSetup)
{
    const CBlockIndex* tip{WITH_LOCK(::cs_main, return m_node.chainman->ActiveTip())};
    BOOST_REQUIRE(tip != nullptr);
    ConsensusParamsRestorer global_restorer{Params().GetConsensus()};
    ConsensusParamsRestorer chain_restorer{m_node.chainman->GetConsensus()};
    auto& global_consensus{global_restorer.Get()};
    auto& consensus{chain_restorer.Get()};
    auto plain{evo::SnapshotLLMQParams(Consensus::LLMQType::LLMQ_TEST)};
    auto rotated{evo::SnapshotLLMQParams(Consensus::LLMQType::LLMQ_TEST_DIP0024)};
    plain.dkgInterval = 12;
    plain.dkgMiningWindowStart = 1;
    plain.dkgMiningWindowEnd = 3;
    rotated.dkgInterval = 12;
    consensus.llmqs = {plain, rotated};
    global_consensus.llmqs = consensus.llmqs;

    const CBlockIndex* quorum{tip->GetAncestor(96)};
    BOOST_REQUIRE(quorum != nullptr);
    std::map<const CBlockIndex*, CDeterministicMNList> lists;
    const auto make_list = [&](const CBlockIndex* work) {
        CDeterministicMNList list{work->GetBlockHash(), work->nHeight, 100};
        for (uint8_t i{0}; i < 12; ++i) {
            list.AddMN(MN(20 + i, 20 + i, MnType::Regular, ProTxVersion::LegacyBLS, 20 + i), false);
        }
        return list;
    };
    const CBlockIndex* plain_work{quorum->GetAncestor(88)};
    lists.emplace(plain_work, make_list(plain_work));
    std::vector<const CBlockIndex*> rotated_cycles;
    for (const int height : {96, 84, 72, 60}) {
        const CBlockIndex* cycle{tip->GetAncestor(height)};
        const CBlockIndex* work{tip->GetAncestor(height - llmq::WORK_DIFF_DEPTH)};
        rotated_cycles.emplace_back(cycle);
        lists.try_emplace(work, make_list(work));
    }
    for (const auto& [work, list] : lists) m_node.dmnman->SetListForBlockForTesting(list);
    BOOST_REQUIRE(m_node.chainman->IsQuorumTypeEnabled(plain.type, quorum->pprev));
    BOOST_REQUIRE(m_node.chainman->IsQuorumTypeEnabled(rotated.type, quorum->pprev));
    BOOST_REQUIRE_EQUAL(m_node.dmnman->GetListForBlock(plain_work).GetCounts().enabled(), 12U);
    const llmq::CQuorumSnapshot empty_snapshot{std::vector<bool>(12, false),
                                               SnapshotSkipMode::MODE_NO_SKIPPING, {}};
    for (size_t i{1}; i < rotated_cycles.size(); ++i) {
        m_node.llmq_ctx->qsnapman->StoreSnapshotForBlock(rotated.type, rotated_cycles[i], empty_snapshot);
    }

    // Derive the oracle through a separate manager, cache, and EvoDB. The
    // manager under test is seeded only after these expected sets exist.
    CEvoDB expected_db{util::DbWrapperParams{.path = m_args.GetDataDirBase() / "evo_snapshot_oracle",
                                             .memory = true, .wipe = true}};
    CMasternodeMetaMan expected_meta;
    CDeterministicMNManager expected_dmnman{expected_db, expected_meta};
    llmq::CQuorumSnapshotManager expected_qsnapman{expected_db};
    {
        auto tx{expected_db.BeginTransaction(EvoDbIdentity::NORMAL)};
        for (const auto& [_, list] : lists) BOOST_REQUIRE(expected_dmnman.SeedListForBlock(list));
        for (size_t i{1}; i < rotated_cycles.size(); ++i) {
            expected_qsnapman.StoreSnapshotForBlock(rotated.type, rotated_cycles[i], empty_snapshot);
        }
        tx->Commit();
    }
    const auto plain_expected{llmq::utils::GetAllQuorumMembers(
        plain.type, {expected_dmnman, expected_qsnapman, *m_node.chainman, quorum}, true)};
    const auto rotated_expected{llmq::utils::GetAllQuorumMembers(
        rotated.type, {expected_dmnman, expected_qsnapman, *m_node.chainman, quorum}, true)};
    BOOST_REQUIRE(!plain_expected.empty());
    BOOST_REQUIRE(!rotated_expected.empty());

    CBLSSecretKey quorum_key;
    quorum_key.MakeNewKey();
    llmq::CFinalCommitment seeded_commitment{plain, quorum->GetBlockHash()};
    seeded_commitment.nVersion = llmq::CFinalCommitment::BASIC_BLS_NON_INDEXED_QUORUM_VERSION;
    seeded_commitment.quorumPublicKey = quorum_key.GetPublicKey();
    seeded_commitment.quorumVvecHash = H(201);
    const CBlockIndex* mined_index{tip->GetAncestor(98)};

    {
        auto tx{m_node.evodb->BeginTransaction(EvoDbIdentity::SNAPSHOT)};
        for (const auto& [work, list] : lists) BOOST_REQUIRE(m_node.dmnman->SeedListForBlock(list));
        BOOST_REQUIRE(m_node.llmq_ctx->qsnapman->SeedQuorumModifier(
            plain.type, plain_work->GetBlockHash(),
            llmq::utils::GetQuorumHashModifier(plain, consensus, quorum)));
        for (const auto* cycle : rotated_cycles) {
            const CBlockIndex* work{cycle->GetAncestor(cycle->nHeight - llmq::WORK_DIFF_DEPTH)};
            BOOST_REQUIRE(m_node.llmq_ctx->qsnapman->SeedQuorumModifier(
                rotated.type, work->GetBlockHash(),
                llmq::utils::GetQuorumHashModifier(rotated, consensus, cycle)));
        }
        for (size_t i{1}; i < rotated_cycles.size(); ++i) {
            BOOST_REQUIRE(m_node.llmq_ctx->qsnapman->SeedSnapshotForBlock(
                rotated.type, rotated_cycles[i], empty_snapshot));
        }
        BOOST_REQUIRE(WITH_LOCK(::cs_main, return m_node.llmq_ctx->quorum_block_processor->SeedMinedCommitment(
            plain.type, quorum->GetBlockHash(), seeded_commitment, mined_index->GetBlockHash());));
        tx->Commit();
    }

    std::map<CBlockIndex*, uint32_t> saved_status;
    {
        LOCK(::cs_main);
        for (const auto& [work, _] : lists) {
            auto* mutable_work{const_cast<CBlockIndex*>(work)};
            saved_status.emplace(mutable_work, mutable_work->nStatus);
            mutable_work->nStatus &= ~BLOCK_HAVE_DATA;
            m_node.dmnman->InvalidateListCacheForBlock(work->GetBlockHash());
        }
        for (size_t i{1}; i < rotated_cycles.size(); ++i) {
            m_node.llmq_ctx->qsnapman->InvalidateSnapshotCacheForBlock(rotated.type,
                                                                       rotated_cycles[i]->GetBlockHash());
        }
    }
    std::vector<CDeterministicMNCPtr> plain_seeded;
    std::vector<CDeterministicMNCPtr> rotated_seeded;
    std::vector<llmq::CQuorumCPtr> scanned;
    llmq::VerifyRecSigStatus recovered_sig_status{llmq::VerifyRecSigStatus::NoQuorum};
    {
        auto tx{m_node.evodb->BeginTransaction(EvoDbIdentity::SNAPSHOT)};
        plain_seeded = llmq::utils::GetAllQuorumMembers(
            plain.type, {*m_node.dmnman, *m_node.llmq_ctx->qsnapman, *m_node.chainman, quorum}, true);
        rotated_seeded = llmq::utils::GetAllQuorumMembers(
            rotated.type, {*m_node.dmnman, *m_node.llmq_ctx->qsnapman, *m_node.chainman, quorum}, true);
        scanned = m_node.llmq_ctx->qman->ScanQuorums(plain.type, tip, 1);
        const uint256 id{H(202)};
        const uint256 msg_hash{H(203)};
        const llmq::SignHash sign_hash{plain.type, quorum->GetBlockHash(), id, msg_hash};
        recovered_sig_status = llmq::VerifyRecoveredSig(
            plain.type, *m_node.llmq_ctx->qman, tip, id, msg_hash,
            quorum_key.Sign(sign_hash.Get(), /*specificLegacyScheme=*/false));
    }
    const auto hashes = [](const auto& members) {
        std::vector<uint256> result;
        for (const auto& member : members) result.emplace_back(member->proTxHash);
        return result;
    };
    BOOST_CHECK(hashes(plain_seeded) == hashes(plain_expected));
    BOOST_CHECK(hashes(rotated_seeded) == hashes(rotated_expected));
    BOOST_REQUIRE_EQUAL(scanned.size(), 1U);
    BOOST_CHECK(hashes(scanned[0]->members) == hashes(plain_expected));
    BOOST_CHECK(recovered_sig_status == llmq::VerifyRecSigStatus::Valid);

    // Prove reconstruction fails closed instead of falling through to the
    // ordinary diff chain when one required seeded full list is absent.
    size_t forbidden_fallbacks{0};
    m_node.dmnman->SetListSnapshotMissHookForTesting([&](const CBlockIndex* index) {
        ++forbidden_fallbacks;
        throw std::logic_error(strprintf("forbidden NORMAL MN-list fallback at height %d", index->nHeight));
    });
    {
        auto tx{m_node.evodb->BeginTransaction(EvoDbIdentity::SNAPSHOT)};
        m_node.evodb->Erase(std::make_pair(std::string{"dmn_S3"}, plain_work->GetBlockHash()));
        m_node.dmnman->InvalidateListCacheForBlock(plain_work->GetBlockHash());
        BOOST_CHECK_THROW(llmq::utils::GetAllQuorumMembers(
            plain.type, {*m_node.dmnman, *m_node.llmq_ctx->qsnapman, *m_node.chainman, quorum}, true),
            std::logic_error);
    }
    m_node.dmnman->SetListSnapshotMissHookForTesting({});
    BOOST_CHECK_EQUAL(forbidden_fallbacks, 1U);

    {
        LOCK(::cs_main);
        for (const auto& [work, status] : saved_status) work->nStatus = status;
    }
    {
        auto tx{m_node.evodb->BeginTransaction(EvoDbIdentity::SNAPSHOT)};
        const auto modifier_key{std::make_tuple(std::string_view{"llmq_M3"}, plain.type,
                                                plain_work->GetBlockHash())};
        m_node.evodb->Erase(modifier_key);
        m_node.evodb->Write(modifier_key, H(254));
        BOOST_CHECK_THROW(llmq::utils::GetAllQuorumMembers(
            plain.type, {*m_node.dmnman, *m_node.llmq_ctx->qsnapman, *m_node.chainman, quorum}, true),
            evo::SnapshotStateMismatchError);
    }
}

BOOST_FIXTURE_TEST_CASE(chain_validation_pre_dip3_matrix, TestChain100Setup)
{
    const CBlockIndex* base{WITH_LOCK(::cs_main, return m_node.chainman->ActiveTip())};
    BOOST_REQUIRE(base != nullptr);
    evo::CEvoSnapshot snapshot;
    snapshot.base_block_hash = base->GetBlockHash();
    snapshot.mn_list = CDeterministicMNList{base->GetBlockHash(), base->nHeight, 0};
    std::string error;
    BOOST_CHECK(WITH_LOCK(::cs_main,
        return evo::ValidateEvoSnapshotAgainstChain(snapshot, *m_node.chainman, base, error)));

    auto wrong_base{snapshot};
    wrong_base.base_block_hash = H(99);
    BOOST_CHECK(!WITH_LOCK(::cs_main,
        return evo::ValidateEvoSnapshotAgainstChain(wrong_base, *m_node.chainman, base, error)));

    auto nonempty{snapshot};
    nonempty.credit_pool.locked = 1;
    BOOST_CHECK(!WITH_LOCK(::cs_main,
        return evo::ValidateEvoSnapshotAgainstChain(nonempty, *m_node.chainman, base, error)));

    ConsensusParamsRestorer params_restorer{m_node.chainman->GetConsensus()};
    auto& mutable_consensus{params_restorer.Get()};
    mutable_consensus.DIP0003Height = 1;
    mutable_consensus.V19Height = 1;
    mutable_consensus.llmqs = {evo::SnapshotLLMQParams(Consensus::LLMQType::LLMQ_TEST)};

    evo::CEvoSnapshot active{snapshot};
    evo::CQuorumSnapshotData quorum_data;
    quorum_data.llmq_type = Consensus::LLMQType::LLMQ_TEST;
    const auto& params{mutable_consensus.llmqs.front()};
    const auto make_commitment = [&](int quorum_height, int mined_height) {
        evo::CMinedQuorumCommitment entry;
        const CBlockIndex* quorum{base->GetAncestor(quorum_height)};
        entry.quorum_base_block_hash = quorum->GetBlockHash();
        entry.work_block_hash = entry.quorum_base_block_hash;
        entry.mined_block_hash = base->GetAncestor(mined_height)->GetBlockHash();
        entry.commitment.nVersion = llmq::CFinalCommitment::BASIC_BLS_NON_INDEXED_QUORUM_VERSION;
        entry.commitment.llmqType = params.type;
        entry.commitment.quorumHash = entry.quorum_base_block_hash;
        entry.commitment.signers.resize(params.size);
        entry.commitment.validMembers.resize(params.size);
        return entry;
    };
    quorum_data.active_commitments = {make_commitment(72, 82), make_commitment(48, 58)};
    quorum_data.safety_commitments = {make_commitment(24, 34)};
    std::sort(quorum_data.active_commitments.begin(), quorum_data.active_commitments.end(),
              [](const auto& a, const auto& b) {
                  return std::tie(a.quorum_base_block_hash, a.mined_block_hash) <
                         std::tie(b.quorum_base_block_hash, b.mined_block_hash);
              });
    active.quorums = {quorum_data};
    CDeterministicMNList previous{active.mn_list};
    uint256 previous_hash{active.base_block_hash};
    for (const int height : {72, 48, 24}) {
        const CBlockIndex* work{base->GetAncestor(height)};
        CDeterministicMNList list{work->GetBlockHash(), height, 0};
        active.historical_mn_list_diffs.push_back({previous_hash, work->GetBlockHash(), height, 0,
                                                   evo::CanonicalMNListHash(list), previous.BuildDiff(list)});
        active.quorum_modifiers.push_back({params.type, work->GetBlockHash(),
            llmq::utils::GetQuorumHashModifier(params, mutable_consensus, work)});
        previous_hash = work->GetBlockHash();
        previous = std::move(list);
    }
    std::sort(active.quorum_modifiers.begin(), active.quorum_modifiers.end(), [](const auto& a, const auto& b) {
        return std::tie(a.llmq_type, a.work_block_hash) < std::tie(b.llmq_type, b.work_block_hash);
    });
    const bool active_valid{WITH_LOCK(::cs_main,
        return evo::ValidateEvoSnapshotAgainstChain(active, *m_node.chainman, base, error))};
    BOOST_CHECK_MESSAGE(active_valid, error);

    auto wrong_counts{active};
    wrong_counts.quorums[0].safety_commitments.clear();
    BOOST_CHECK(!WITH_LOCK(::cs_main,
        return evo::ValidateEvoSnapshotAgainstChain(wrong_counts, *m_node.chainman, base, error)));

    auto non_ancestor{active};
    non_ancestor.quorums[0].active_commitments[0].quorum_base_block_hash = H(99);
    non_ancestor.quorums[0].active_commitments[0].commitment.quorumHash = H(99);
    BOOST_CHECK(!WITH_LOCK(::cs_main,
        return evo::ValidateEvoSnapshotAgainstChain(non_ancestor, *m_node.chainman, base, error)));

    // A young chain carries fewer commitments than the parameter horizon. A
    // coherent snapshot with a single active commitment, its historical diff,
    // and its modifier must pass both validation layers: parameter counts are
    // maxima, and completeness is established by the completion-time CbTx
    // quorum merkle root, not by per-type count equality.
    evo::CEvoSnapshot partial{snapshot};
    evo::CQuorumSnapshotData partial_data;
    partial_data.llmq_type = Consensus::LLMQType::LLMQ_TEST;
    partial_data.active_commitments = {make_commitment(72, 82)};
    partial.quorums = {partial_data};
    {
        const CBlockIndex* work{base->GetAncestor(72)};
        CDeterministicMNList list{work->GetBlockHash(), 72, 0};
        partial.historical_mn_list_diffs.push_back({partial.base_block_hash, work->GetBlockHash(), 72, 0,
                                                    evo::CanonicalMNListHash(list), partial.mn_list.BuildDiff(list)});
        partial.quorum_modifiers.push_back({params.type, work->GetBlockHash(),
            llmq::utils::GetQuorumHashModifier(params, mutable_consensus, work)});
    }
    BOOST_CHECK_NO_THROW(partial.Validate());
    const bool partial_valid{WITH_LOCK(::cs_main,
        return evo::ValidateEvoSnapshotAgainstChain(partial, *m_node.chainman, base, error))};
    BOOST_CHECK_MESSAGE(partial_valid, error);
}

BOOST_FIXTURE_TEST_CASE(builder_emits_available_history_on_young_chains, SnapshotActivationChainSetup)
{
    // No masternodes exist and no DKGs have run on this fixture chain, so every
    // enabled LLMQ type has zero mined commitments and zero rotation cycles.
    // dumptxoutset-grade building must succeed on such a chain and emit the
    // history that exists rather than failing the parameter-derived horizon.
    const CBlockIndex* base{WITH_LOCK(::cs_main, return m_node.chainman->ActiveTip())};
    BOOST_REQUIRE(base != nullptr);
    evo::CEvoSnapshot snapshot;
    std::string error;
    const bool built{WITH_LOCK(::cs_main,
        return evo::BuildEvoSnapshot(Params(), *m_node.chainman, *m_node.dmnman,
                                     *m_node.llmq_ctx->quorum_block_processor, *m_node.llmq_ctx->qsnapman,
                                     *m_node.chain_helper->credit_pool_manager, *m_node.chain_helper->ehf_manager,
                                     base, snapshot, error))};
    BOOST_REQUIRE_MESSAGE(built, error);
    for (const auto& data : snapshot.quorums) {
        BOOST_CHECK(data.active_commitments.empty());
        BOOST_CHECK(data.safety_commitments.empty());
        BOOST_CHECK(data.rotation_snapshots.empty());
    }
    BOOST_CHECK_NO_THROW(snapshot.Validate());
    const bool valid{WITH_LOCK(::cs_main,
        return evo::ValidateEvoSnapshotAgainstChain(snapshot, *m_node.chainman, base, error))};
    BOOST_CHECK_MESSAGE(valid, error);
}

BOOST_FIXTURE_TEST_CASE(rotation_bitset_matches_historical_work_list, TestChain100Setup)
{
    const CBlockIndex* base{WITH_LOCK(::cs_main, return m_node.chainman->ActiveTip())};
    BOOST_REQUIRE(base != nullptr);
    ConsensusParamsRestorer params_restorer{m_node.chainman->GetConsensus()};
    auto& consensus{params_restorer.Get()};
    auto params{evo::SnapshotLLMQParams(Consensus::LLMQType::LLMQ_TEST_DIP0024)};
    params.dkgInterval = 12;
    params.dkgMiningWindowStart = 2;
    params.dkgMiningWindowEnd = 6;
    consensus.llmqs = {params};
    consensus.DIP0003Height = 1;
    consensus.V19Height = 1;

    evo::CEvoSnapshot snapshot;
    snapshot.base_block_hash = base->GetBlockHash();
    snapshot.mn_list = CDeterministicMNList{base->GetBlockHash(), base->nHeight, 0};
    evo::CQuorumSnapshotData data;
    data.llmq_type = params.type;
    data.rotation_enabled = true;
    const auto commitment = [&](int quorum_height, int mined_height, int16_t quorum_index) {
        evo::CMinedQuorumCommitment entry;
        const CBlockIndex* quorum{base->GetAncestor(quorum_height)};
        const CBlockIndex* cycle{quorum->GetAncestor(quorum->nHeight - quorum->nHeight % params.dkgInterval)};
        entry.quorum_base_block_hash = quorum->GetBlockHash();
        entry.work_block_hash = cycle->GetAncestor(cycle->nHeight - llmq::WORK_DIFF_DEPTH)->GetBlockHash();
        entry.mined_block_hash = base->GetAncestor(mined_height)->GetBlockHash();
        entry.commitment.nVersion = llmq::CFinalCommitment::BASIC_BLS_INDEXED_QUORUM_VERSION;
        entry.commitment.llmqType = params.type;
        entry.commitment.quorumHash = entry.quorum_base_block_hash;
        entry.commitment.quorumIndex = quorum_index;
        entry.commitment.signers.resize(params.size);
        entry.commitment.validMembers.resize(params.size);
        return entry;
    };
    data.active_commitments = {commitment(84, 86, 0), commitment(85, 87, 1)};
    data.safety_commitments = {commitment(72, 74, 0), commitment(73, 75, 1)};

    std::map<uint256, const CBlockIndex*> required_work;
    for (const auto& required : evo::EvoSnapshotReconstructionHeights(base->nHeight, {params})) {
        const int cycle_height{required.quorum_height};
        const int work_height{required.work_height};
        const CBlockIndex* cycle{base->GetAncestor(cycle_height)};
        const CBlockIndex* work{base->GetAncestor(work_height)};
        BOOST_REQUIRE(cycle != nullptr);
        BOOST_REQUIRE(work != nullptr);
        const size_t population{static_cast<size_t>(params.size + 3)};
        data.rotation_snapshots.push_back({cycle->GetBlockHash(), work->GetBlockHash(),
            llmq::CQuorumSnapshot{std::vector<bool>(population, true), SnapshotSkipMode::MODE_NO_SKIPPING, {}}});
        required_work.emplace(work->GetBlockHash(), work);
    }
    for (const auto* commitments : {&data.active_commitments, &data.safety_commitments}) {
        for (const auto& entry : *commitments) {
            const CBlockIndex* work{WITH_LOCK(::cs_main,
                return m_node.chainman->m_blockman.LookupBlockIndex(entry.work_block_hash);)};
            BOOST_REQUIRE(work != nullptr);
            required_work.emplace(entry.work_block_hash, work);
        }
    }
    const auto commitment_less = [](const auto& a, const auto& b) {
        return std::tie(a.quorum_base_block_hash, a.mined_block_hash) <
               std::tie(b.quorum_base_block_hash, b.mined_block_hash);
    };
    std::sort(data.active_commitments.begin(), data.active_commitments.end(), commitment_less);
    std::sort(data.safety_commitments.begin(), data.safety_commitments.end(), commitment_less);
    std::sort(data.rotation_snapshots.begin(), data.rotation_snapshots.end(), [](const auto& a, const auto& b) {
        return std::tie(a.cycle_base_block_hash, a.work_block_hash) <
               std::tie(b.cycle_base_block_hash, b.work_block_hash);
    });
    snapshot.quorums = {std::move(data)};

    std::vector<const CBlockIndex*> ordered_work;
    for (const auto& [_, work] : required_work) ordered_work.emplace_back(work);
    std::sort(ordered_work.begin(), ordered_work.end(), [](const auto* a, const auto* b) { return a->nHeight > b->nHeight; });
    CDeterministicMNList previous{snapshot.mn_list};
    uint256 previous_hash{snapshot.base_block_hash};
    for (const auto* work : ordered_work) {
        CDeterministicMNList work_list{work->GetBlockHash(), work->nHeight, 100};
        for (uint8_t i{0}; i < params.size + 3; ++i) {
            work_list.AddMN(MN(20 + i, 20 + i, MnType::Regular, ProTxVersion::LegacyBLS, 20 + i), false);
        }
        snapshot.historical_mn_list_diffs.push_back(
            {previous_hash, work->GetBlockHash(), work->nHeight, work_list.GetTotalRegisteredCount(),
             evo::CanonicalMNListHash(work_list), previous.BuildDiff(work_list)});
        previous_hash = work->GetBlockHash();
        previous = std::move(work_list);
    }
    std::map<uint256, const CBlockIndex*> modifier_cycles;
    for (const auto* commitments : {&snapshot.quorums[0].active_commitments, &snapshot.quorums[0].safety_commitments}) {
        for (const auto& entry : *commitments) {
            const CBlockIndex* quorum_index{WITH_LOCK(::cs_main,
                return m_node.chainman->m_blockman.LookupBlockIndex(entry.quorum_base_block_hash);)};
            const CBlockIndex* cycle{quorum_index->GetAncestor(
                quorum_index->nHeight - quorum_index->nHeight % params.dkgInterval)};
            modifier_cycles.emplace(entry.work_block_hash, cycle);
        }
    }
    for (const auto& entry : snapshot.quorums[0].rotation_snapshots) {
        modifier_cycles.emplace(entry.work_block_hash, WITH_LOCK(::cs_main,
            return m_node.chainman->m_blockman.LookupBlockIndex(entry.cycle_base_block_hash);));
    }
    for (const auto& [work_hash, cycle] : modifier_cycles) {
        snapshot.quorum_modifiers.push_back({params.type, work_hash,
            llmq::utils::GetQuorumHashModifier(params, consensus, cycle)});
    }

    std::string error;
    const bool valid{WITH_LOCK(::cs_main,
        return evo::ValidateEvoSnapshotAgainstChain(snapshot, *m_node.chainman, base, error))};
    BOOST_CHECK_MESSAGE(valid, error);
    auto short_bitset{snapshot};
    short_bitset.quorums[0].rotation_snapshots[0].snapshot.activeQuorumMembers.pop_back();
    BOOST_CHECK(!WITH_LOCK(::cs_main,
        return evo::ValidateEvoSnapshotAgainstChain(short_bitset, *m_node.chainman, base, error)));
    auto bad_modifier{snapshot};
    bad_modifier.quorum_modifiers[0].modifier.begin()[0] ^= 1;
    BOOST_CHECK(!WITH_LOCK(::cs_main,
        return evo::ValidateEvoSnapshotAgainstChain(bad_modifier, *m_node.chainman, base, error)));
}

BOOST_AUTO_TEST_CASE(reconstruction_horizon_height_enumeration)
{
    const auto rotated{evo::SnapshotLLMQParams(Consensus::LLMQType::LLMQ_TEST_DIP0024)};
    const auto plain{evo::SnapshotLLMQParams(Consensus::LLMQType::LLMQ_TEST)};
    const int base_height{20 * rotated.dkgInterval + 7};
    const auto heights{evo::EvoSnapshotReconstructionHeights(base_height, {rotated, plain})};
    BOOST_REQUIRE_EQUAL(heights.size(), evo::EVO_SNAPSHOT_ROTATION_CYCLES +
                                             evo::SnapshotCommitmentCount(plain, false));
    const int rotated_h{base_height - base_height % rotated.dkgInterval};
    for (size_t i{0}; i < evo::EVO_SNAPSHOT_ROTATION_CYCLES; ++i) {
        const int expected_cycle{rotated_h - static_cast<int>(i + 1) * rotated.dkgInterval};
        BOOST_CHECK(heights[i].rotation);
        BOOST_CHECK_EQUAL(heights[i].quorum_height, expected_cycle);
        BOOST_CHECK_EQUAL(heights[i].work_height, expected_cycle - llmq::WORK_DIFF_DEPTH);
    }
    const int plain_h{base_height - base_height % plain.dkgInterval};
    for (size_t i{0}; i < evo::SnapshotCommitmentCount(plain, false); ++i) {
        const auto& height{heights[evo::EVO_SNAPSHOT_ROTATION_CYCLES + i]};
        BOOST_CHECK(!height.rotation);
        BOOST_CHECK_EQUAL(height.quorum_height, plain_h - static_cast<int>(i) * plain.dkgInterval);
        BOOST_CHECK_EQUAL(height.work_height, height.quorum_height - llmq::WORK_DIFF_DEPTH);
    }
}

BOOST_AUTO_TEST_CASE(rotation_bitset_larger_than_quorum_roundtrips)
{
    const auto& params{evo::SnapshotLLMQParams(Consensus::LLMQType::LLMQ_TEST_DIP0024)};
    evo::CQuorumSnapshotEntry entry;
    entry.cycle_base_block_hash = H(1);
    entry.work_block_hash = H(2);
    entry.snapshot.activeQuorumMembers.resize(params.size + 3);
    entry.snapshot.activeQuorumMembers[params.size + 1] = true;
    entry.snapshot.mnSkipListMode = SnapshotSkipMode::MODE_NO_SKIPPING;

    CDataStream stream{SER_DISK, CLIENT_VERSION};
    evo::WriteRotationSnapshot(stream, entry);
    const auto decoded{evo::ReadRotationSnapshot(stream, params)};
    BOOST_CHECK(stream.empty());
    BOOST_CHECK_EQUAL(decoded.snapshot.activeQuorumMembers.size(), params.size + 3U);
    BOOST_CHECK(decoded.snapshot.activeQuorumMembers[params.size + 1]);
}

BOOST_FIXTURE_TEST_CASE(populated_v3_golden_value, BasicTestingSetup)
{
    BOOST_CHECK_EQUAL(GetEvoSnapshotHash(SyntheticSnapshot()).ToString(),
                      "bb1985a651ed3110218a3c8d65d77c85facdc544d6b9203f0815d5785c1f01ff");
}

BOOST_FIXTURE_TEST_CASE(canonical_mn_reader_rejects_order_and_counter, BasicTestingSetup)
{
    BOOST_CHECK(evo::CanonicalMNListHash(CDeterministicMNList{}) ==
                evo::CanonicalMNListHash(CDeterministicMNList{}));
    const auto write_raw = [](uint32_t total, std::vector<CDeterministicMNCPtr> mns) {
        CDataStream stream{SER_DISK, CLIENT_VERSION};
        stream << H(42) << 42 << total;
        WriteCompactSize(stream, mns.size());
        for (const auto& dmn : mns) stream << *dmn;
        return stream;
    };
    auto unsorted{write_raw(10, {MN(2, 2, MnType::Regular, ProTxVersion::LegacyBLS, 2),
                                 MN(1, 1, MnType::Regular, ProTxVersion::LegacyBLS, 1)})};
    BOOST_CHECK_THROW(evo::UnserializeCanonicalMNList(unsorted), std::ios_base::failure);
    auto bad_counter{write_raw(2, {MN(2, 1, MnType::Regular, ProTxVersion::LegacyBLS, 1)})};
    BOOST_CHECK_THROW(evo::UnserializeCanonicalMNList(bad_counter), std::ios_base::failure);
}

BOOST_FIXTURE_TEST_CASE(diff_chain_roundtrip_and_canonical_determinism, BasicTestingSetup)
{
    const auto base{MNList(H(10), 100, false)};
    auto target{base};
    target.RemoveMN(base.GetMNByInternalId(2)->proTxHash);
    target.AddMN(MN(8, 8, MnType::Regular, ProTxVersion::LegacyBLS, 8));
    for (const uint64_t id : {5, 7}) {
        const auto dmn{target.GetMNByInternalId(id)};
        auto state{std::make_shared<CDeterministicMNState>(*dmn->pdmnState)};
        state->nLastPaidHeight += static_cast<int>(id);
        target.UpdateMN(*dmn, state);
    }
    const auto diff{base.BuildDiff(target)};
    auto permuted{diff};
    std::reverse(permuted.addedMNs.begin(), permuted.addedMNs.end());
    std::vector<std::pair<uint64_t, CDeterministicMNStateDiff>> updates(permuted.updatedMNs.begin(),
                                                                       permuted.updatedMNs.end());
    std::reverse(updates.begin(), updates.end());
    permuted.updatedMNs.clear();
    for (auto& update : updates) permuted.updatedMNs.emplace(std::move(update));

    CDataStream canonical{SER_DISK, CLIENT_VERSION};
    CDataStream reordered{SER_DISK, CLIENT_VERSION};
    evo::SerializeCanonicalMNListDiff(canonical, diff);
    evo::SerializeCanonicalMNListDiff(reordered, permuted);
    BOOST_CHECK_EQUAL_COLLECTIONS(canonical.begin(), canonical.end(), reordered.begin(), reordered.end());

    auto decoded{evo::UnserializeCanonicalMNListDiff(canonical)};
    auto reconstructed{base};
    reconstructed.ApplyDiffForSnapshot(H(11), 99, target.GetTotalRegisteredCount(), decoded);
    target.ApplyDiffForSnapshot(H(11), 99, target.GetTotalRegisteredCount(), CDeterministicMNListDiff{});
    BOOST_CHECK(evo::CanonicalMNListHash(reconstructed) == evo::CanonicalMNListHash(target));
    BOOST_CHECK(canonical.empty());
}

BOOST_FIXTURE_TEST_CASE(historical_diff_decode_has_cumulative_operation_budget, BasicTestingSetup)
{
    CDeterministicMNListDiff one_removal;
    one_removal.removedMns.emplace(1);
    CDataStream first{SER_DISK, CLIENT_VERSION};
    CDataStream second{SER_DISK, CLIENT_VERSION};
    evo::SerializeCanonicalMNListDiff(first, one_removal);
    evo::SerializeCanonicalMNListDiff(second, one_removal);

    size_t remaining_operations{1};
    const auto decoded{evo::UnserializeCanonicalMNListDiff(first, remaining_operations)};
    BOOST_CHECK_EQUAL(decoded.removedMns.size(), 1U);
    BOOST_CHECK_EQUAL(remaining_operations, 0U);
    BOOST_CHECK_THROW(evo::UnserializeCanonicalMNListDiff(second, remaining_operations),
                      std::ios_base::failure);
    BOOST_CHECK(evo::EvoSnapshotMaxHistoricalMNLists() < 2'048U);
}

BOOST_FIXTURE_TEST_CASE(context_free_validation_matrix, BasicTestingSetup)
{
    auto snapshot{SyntheticSnapshot()};
    snapshot.version++;
    CheckInvalid(snapshot);
    snapshot = SyntheticSnapshot();
    snapshot.base_block_hash = H(1);
    CheckInvalid(snapshot);
    snapshot = SyntheticSnapshot();
    std::reverse(snapshot.quorums.begin(), snapshot.quorums.end());
    BOOST_CHECK_THROW(snapshot.Validate(/*require_canonical_order=*/true), std::ios_base::failure);
    snapshot = SyntheticSnapshot();
    std::reverse(snapshot.historical_mn_list_diffs.begin(), snapshot.historical_mn_list_diffs.end());
    BOOST_CHECK_THROW(snapshot.Validate(/*require_canonical_order=*/true), std::ios_base::failure);
    snapshot = SyntheticSnapshot();
    std::reverse(snapshot.quorums[0].active_commitments.begin(), snapshot.quorums[0].active_commitments.end());
    BOOST_CHECK_THROW(snapshot.Validate(/*require_canonical_order=*/true), std::ios_base::failure);
    snapshot = SyntheticSnapshot();
    std::reverse(snapshot.quorums[1].rotation_snapshots.begin(), snapshot.quorums[1].rotation_snapshots.end());
    BOOST_CHECK_THROW(snapshot.Validate(/*require_canonical_order=*/true), std::ios_base::failure);
    snapshot = SyntheticSnapshot();
    snapshot.historical_mn_list_diffs[0].block_hash = H(1);
    CheckInvalid(snapshot);
    snapshot = SyntheticSnapshot();
    snapshot.quorums[1].rotation_snapshots[0].work_block_hash = H(1);
    CheckInvalid(snapshot);
    snapshot = SyntheticSnapshot();
    snapshot.quorums[0].llmq_type = Consensus::LLMQType::LLMQ_NONE;
    CheckInvalid(snapshot);
    snapshot = SyntheticSnapshot();
    snapshot.quorums[0].rotation_enabled = true;
    CheckInvalid(snapshot);
    snapshot = SyntheticSnapshot();
    snapshot.quorums[0].active_commitments.pop_back();
    CheckInvalid(snapshot);
    snapshot = SyntheticSnapshot();
    snapshot.quorums[0].safety_commitments.clear();
    CheckInvalid(snapshot);
    snapshot = SyntheticSnapshot();
    snapshot.quorums[1].rotation_snapshots.pop_back();
    CheckInvalid(snapshot);

    // Parameter-derived history counts are ceilings. Young chains and newly
    // activated quorum types legitimately carry fewer commitments and cycles;
    // chain-aware validation and the base CbTx establish completeness later.
    snapshot = SyntheticSnapshot();
    snapshot.quorums.clear();
    snapshot.historical_mn_list_diffs.clear();
    snapshot.quorum_modifiers.clear();
    evo::CQuorumSnapshotData partial;
    partial.llmq_type = Consensus::LLMQType::LLMQ_TEST;
    snapshot.quorums.emplace_back(std::move(partial));
    BOOST_CHECK_NO_THROW(snapshot.Validate());

    snapshot = SyntheticSnapshot();
    snapshot.credit_pool.locked = -1;
    CheckInvalid(snapshot);
    snapshot = SyntheticSnapshot();
    snapshot.credit_pool.currentLimit = snapshot.credit_pool.locked + 1;
    CheckInvalid(snapshot);
    snapshot = SyntheticSnapshot();
    snapshot.credit_pool.latelyUnlocked = MAX_MONEY + 1;
    CheckInvalid(snapshot);
    snapshot = SyntheticSnapshot();
    snapshot.mnhf_signals.emplace(VERSIONBITS_NUM_BITS, 10);
    CheckInvalid(snapshot);
    snapshot = SyntheticSnapshot();
    snapshot.mnhf_signals.emplace(11, -1);
    CheckInvalid(snapshot);
    snapshot = SyntheticSnapshot();
    snapshot.mnhf_signals.emplace(11, snapshot.mn_list.GetHeightForSnapshotCodec() + 1);
    CheckInvalid(snapshot);

    const auto mutate_commitment = [](auto mutation) {
        auto value{SyntheticSnapshot()};
        mutation(value.quorums[0].active_commitments[0]);
        CheckInvalid(std::move(value));
    };
    mutate_commitment([](auto& e) { e.commitment.validMembers.resize(e.commitment.signers.size() + 1); });
    mutate_commitment([](auto& e) {
        e.commitment.signers.clear();
        e.commitment.validMembers.clear();
    });
    mutate_commitment([](auto& e) { e.quorum_base_block_hash.SetNull(); });
    mutate_commitment([](auto& e) { e.mined_block_hash.SetNull(); });
    mutate_commitment([](auto& e) { e.commitment.llmqType = Consensus::LLMQType::LLMQ_TEST_PLATFORM; });
    mutate_commitment([](auto& e) { e.commitment.quorumHash = H(99); });
    mutate_commitment([](auto& e) { e.commitment.nVersion = llmq::CFinalCommitment::BASIC_BLS_INDEXED_QUORUM_VERSION; });
    mutate_commitment([](auto& e) { e.commitment.nVersion = 99; });
    snapshot = SyntheticSnapshot();
    snapshot.quorums[1].active_commitments[1].commitment.quorumIndex = 0;
    CheckInvalid(snapshot);
    snapshot = SyntheticSnapshot();
    snapshot.quorums[1].active_commitments[1].commitment.quorumIndex = 2;
    CheckInvalid(snapshot);

    const auto mutate_rotation = [](auto mutation) {
        auto value{SyntheticSnapshot()};
        mutation(value.quorums[1].rotation_snapshots[0]);
        CheckInvalid(std::move(value));
    };
    mutate_rotation([](auto& e) { e.cycle_base_block_hash.SetNull(); });
    mutate_rotation([](auto& e) { e.work_block_hash.SetNull(); });
    mutate_rotation([](auto& e) { e.snapshot.mnSkipListMode = static_cast<SnapshotSkipMode>(9); });
    mutate_rotation([](auto& e) { e.snapshot.activeQuorumMembers.resize(evo::EVO_SNAPSHOT_MAX_MNS + 1); });
    mutate_rotation([](auto& e) { e.snapshot.mnSkipList = {-1}; });

    // A cycle's skip list accumulates across every quorum index, so lengths
    // beyond a single quorum's size and negative wraparound deltas after the
    // first (absolute) entry are legitimate.
    auto aggregate_skips{SyntheticSnapshot()};
    auto& rotation_entry{aggregate_skips.quorums[1].rotation_snapshots[0]};
    const auto& rotation_params{evo::SnapshotLLMQParams(aggregate_skips.quorums[1].llmq_type)};
    rotation_entry.snapshot.mnSkipListMode = SnapshotSkipMode::MODE_SKIPPING_ENTRIES;
    rotation_entry.snapshot.mnSkipList.assign(static_cast<size_t>(rotation_params.size) + 2, 1);
    rotation_entry.snapshot.mnSkipList.front() = 3;
    rotation_entry.snapshot.mnSkipList.back() = -2;
    BOOST_CHECK_NO_THROW(aggregate_skips.Validate());
}

BOOST_FIXTURE_TEST_CASE(bounded_readers_reject_claimed_sizes_first, BasicTestingSetup)
{
    CDataStream mn_stream{SER_DISK, CLIENT_VERSION};
    mn_stream << H(1) << 1 << uint32_t{0};
    WriteCompactSize(mn_stream, evo::EVO_SNAPSHOT_MAX_MNS + 1);
    BOOST_CHECK_THROW(evo::UnserializeCanonicalMNList(mn_stream), std::ios_base::failure);
    BOOST_CHECK(mn_stream.empty());

    const auto decode_quorum = [](CDataStream stream) {
        evo::CQuorumSnapshotData data;
        stream >> data;
    };
    const auto& plain_params{evo::SnapshotLLMQParams(Consensus::LLMQType::LLMQ_TEST)};
    const size_t commitment_limit{evo::SnapshotCommitmentCount(plain_params, false)};
    CDataStream active{SER_DISK, CLIENT_VERSION};
    active << Consensus::LLMQType::LLMQ_TEST << false;
    WriteCompactSize(active, plain_params.signingActiveQuorumCount + 1);
    BOOST_CHECK_THROW(decode_quorum(active), std::ios_base::failure);
    CDataStream safety{SER_DISK, CLIENT_VERSION};
    safety << Consensus::LLMQType::LLMQ_TEST << false;
    WriteCompactSize(safety, 0);
    WriteCompactSize(safety, commitment_limit - plain_params.signingActiveQuorumCount + 1);
    BOOST_CHECK_THROW(decode_quorum(safety), std::ios_base::failure);
    CDataStream rotations{SER_DISK, CLIENT_VERSION};
    rotations << Consensus::LLMQType::LLMQ_TEST_DIP0024 << true;
    WriteCompactSize(rotations, 0);
    WriteCompactSize(rotations, 0);
    WriteCompactSize(rotations, evo::EVO_SNAPSHOT_ROTATION_CYCLES + 1);
    BOOST_CHECK_THROW(decode_quorum(rotations), std::ios_base::failure);

    const auto& rotated_params{evo::SnapshotLLMQParams(Consensus::LLMQType::LLMQ_TEST_DIP0024)};
    CDataStream bitset{SER_DISK, CLIENT_VERSION};
    bitset << H(1) << H(2) << SnapshotSkipMode::MODE_NO_SKIPPING;
    WriteCompactSize(bitset, evo::EVO_SNAPSHOT_MAX_MNS + 1);
    BOOST_CHECK_THROW(evo::ReadRotationSnapshot(bitset, rotated_params), std::ios_base::failure);
    CDataStream skip_list{SER_DISK, CLIENT_VERSION};
    skip_list << H(1) << H(2) << SnapshotSkipMode::MODE_NO_SKIPPING;
    WriteCompactSize(skip_list, 0);
    WriteCompactSize(skip_list, rotated_params.size + 1);
    BOOST_CHECK_THROW(evo::ReadRotationSnapshot(skip_list, rotated_params), std::ios_base::failure);

    CDataStream commitment_bits{SER_DISK, CLIENT_VERSION};
    auto oversized_commitment{Commitment(Consensus::LLMQType::LLMQ_TEST, 1, 2, false)};
    oversized_commitment.commitment.signers.resize(plain_params.size + 1);
    commitment_bits << Consensus::LLMQType::LLMQ_TEST << false;
    WriteCompactSize(commitment_bits, 1);
    commitment_bits << oversized_commitment;
    evo::CQuorumSnapshotData oversized_data;
    BOOST_CHECK_EXCEPTION(commitment_bits >> oversized_data, std::ios_base::failure, [](const auto& e) {
        return std::string{e.what()}.find("inconsistent evo snapshot commitment bitset sizes") != std::string::npos;
    });
    // The valid-members bitset payload, BLS material, and mined-block hash
    // remain unread: rejection occurs at the mismatched claimed size.
    BOOST_CHECK_GT(commitment_bits.size(), uint256::size());

    const auto commitment_prefix = [](CDataStream& s) {
        s << Consensus::LLMQType::LLMQ_TEST << false;
        WriteCompactSize(s, 1);
        s << H(1) << H(2) << uint16_t{llmq::CFinalCommitment::BASIC_BLS_NON_INDEXED_QUORUM_VERSION}
          << Consensus::LLMQType::LLMQ_TEST << H(1);
    };
    CDataStream over_ceiling{SER_DISK, CLIENT_VERSION};
    commitment_prefix(over_ceiling);
    WriteCompactSize(over_ceiling, evo::EVO_SNAPSHOT_MAX_QUORUM_SIZE + 1);
    BOOST_CHECK_THROW(decode_quorum(over_ceiling), std::ios_base::failure);
    CDataStream empty_bits{SER_DISK, CLIENT_VERSION};
    commitment_prefix(empty_bits);
    WriteCompactSize(empty_bits, 0);
    BOOST_CHECK_THROW(decode_quorum(empty_bits), std::ios_base::failure);

    auto oversized_payout_mn{std::const_pointer_cast<CDeterministicMN>(
        MN(8, 8, MnType::Regular, ProTxVersion::ExtAddr, 8))};
    auto payout_state{std::make_shared<CDeterministicMNState>(*oversized_payout_mn->pdmnState)};
    payout_state->payouts.resize(evo::EVO_SNAPSHOT_MAX_PAYOUT_SHARES + 1);
    oversized_payout_mn->pdmnState = std::move(payout_state);
    CDataStream payouts{SER_DISK, CLIENT_VERSION};
    payouts << H(42) << 42 << uint32_t{10};
    WriteCompactSize(payouts, 1);
    payouts << *oversized_payout_mn;
    BOOST_CHECK_THROW(evo::UnserializeCanonicalMNList(payouts), std::ios_base::failure);

    CDataStream wrapped_string{SER_DISK, CLIENT_VERSION};
    WriteCompactSize(wrapped_string, evo::EVO_SNAPSHOT_MAX_MN_COMPACT_ITEMS + 1);
    wrapped_string << uint8_t{0x42};
    evo::SnapshotBoundedInput bounded_string{wrapped_string, evo::EVO_SNAPSHOT_MAX_MN_COMPACT_ITEMS};
    OverrideStream bounded_override{&bounded_string, SER_DISK, CLIENT_VERSION};
    std::string decoded_string;
    BOOST_CHECK_EXCEPTION(bounded_override >> decoded_string, std::ios_base::failure,
                          [](const auto& e) { return std::string{e.what()}.find("CompactSize budget exceeded") != std::string::npos; });
    BOOST_CHECK(decoded_string.empty());
    BOOST_REQUIRE_EQUAL(wrapped_string.size(), 1U);
    BOOST_CHECK_EQUAL(std::to_integer<uint8_t>(wrapped_string.data()[0]), 0x42);

    const auto snapshot_prefix = [](CDataStream& stream) {
        stream << evo::EVO_SNAPSHOT_VERSION << H(42);
        evo::SerializeCanonicalMNList(stream, CDeterministicMNList{H(42), 1, 0});
    };
    CDataStream quorum_types{SER_DISK, CLIENT_VERSION};
    snapshot_prefix(quorum_types);
    WriteCompactSize(quorum_types, Consensus::available_llmqs.size() + 1);
    evo::CEvoSnapshot decoded;
    BOOST_CHECK_THROW(quorum_types >> decoded, std::ios_base::failure);

    CDataStream history{SER_DISK, CLIENT_VERSION};
    snapshot_prefix(history);
    WriteCompactSize(history, 0);
    WriteCompactSize(history, evo::EvoSnapshotMaxHistoricalMNLists() + 1);
    BOOST_CHECK_THROW(history >> decoded, std::ios_base::failure);

    CDataStream signals{SER_DISK, CLIENT_VERSION};
    snapshot_prefix(signals);
    WriteCompactSize(signals, 0);
    WriteCompactSize(signals, 0);
    WriteCompactSize(signals, 0);
    signals << CCreditPool{};
    WriteCompactSize(signals, Consensus::MAX_VERSION_BITS_DEPLOYMENTS + 1);
    BOOST_CHECK_THROW(signals >> decoded, std::ios_base::failure);

    CDataStream ranges{SER_DISK, CLIENT_VERSION};
    snapshot_prefix(ranges);
    WriteCompactSize(ranges, 0);
    WriteCompactSize(ranges, 0);
    WriteCompactSize(ranges, 0);
    ranges << CAmount{0} << CAmount{0} << CAmount{0};
    WriteCompactSize(ranges, evo::EVO_SNAPSHOT_MAX_RANGES + 1);
    BOOST_CHECK_THROW(ranges >> decoded, std::ios_base::failure);
    BOOST_CHECK(ranges.empty());

    const auto snapshot_bytes{SerializeSnapshot(SyntheticSnapshot())};
    DomainPort domain;
    BOOST_REQUIRE_EQUAL(domain.Set("evo5.example.org", 443), DomainPort::Status::Success);
    CDataStream encoded_domain{SER_DISK, CLIENT_VERSION};
    encoded_domain << domain;
    const auto domain_pos{std::search(snapshot_bytes.begin(), snapshot_bytes.end(),
                                      encoded_domain.begin(), encoded_domain.end())};
    BOOST_REQUIRE(domain_pos != snapshot_bytes.end());

    CDataStream oversized_domain{SER_DISK, CLIENT_VERSION};
    const size_t domain_offset{static_cast<size_t>(std::distance(snapshot_bytes.begin(), domain_pos))};
    oversized_domain.write(Span{snapshot_bytes}.first(domain_offset));
    constexpr size_t MAX_DOMAIN_LENGTH{253};
    WriteCompactSize(oversized_domain, MAX_DOMAIN_LENGTH + 1);
    const std::string oversized_addr(MAX_DOMAIN_LENGTH + 1, 'a');
    oversized_domain.write(MakeByteSpan(oversized_addr));
    const size_t serialized_addr_size{encoded_domain.size() - sizeof(uint16_t)};
    oversized_domain.write(Span{snapshot_bytes}.subspan(domain_offset + serialized_addr_size));
    BOOST_CHECK_EXCEPTION(oversized_domain >> decoded, std::ios_base::failure,
                          [](const auto& e) { return std::string{e.what()}.find("String length limit exceeded") != std::string::npos; });
}

BOOST_FIXTURE_TEST_CASE(commitment_sizes_are_format_bounded_not_param_exact, BasicTestingSetup)
{
    // -llmqtestparams and -llmqdevnetparams change the effective quorum size at
    // runtime, so commitments whose bitsets differ from the static default must
    // pass this layer; exact sizing is established by chain-aware validation.
    auto snapshot{SyntheticSnapshot()};
    const auto& params{evo::SnapshotLLMQParams(snapshot.quorums[0].llmq_type)};
    for (auto& entry : snapshot.quorums[0].active_commitments) {
        entry.commitment.signers.assign(params.size + 5, false);
        entry.commitment.validMembers.assign(params.size + 5, true);
    }
    BOOST_CHECK_NO_THROW(snapshot.Validate(/*require_canonical_order=*/true));
    const auto bytes{SerializeSnapshot(snapshot)};
    CDataStream input{bytes};
    evo::CEvoSnapshot decoded;
    BOOST_CHECK_NO_THROW(input >> decoded);
    BOOST_CHECK(input.empty());
    const auto reencoded{SerializeSnapshot(decoded)};
    BOOST_CHECK_EQUAL_COLLECTIONS(bytes.begin(), bytes.end(), reencoded.begin(), reencoded.end());
}

BOOST_FIXTURE_TEST_CASE(mnhf_signal_wire_order_is_canonical, BasicTestingSetup)
{
    const auto bytes{SerializeSnapshot(SyntheticSnapshot())};
    // The MNHF signal section is the encoding's tail: a count followed by
    // (bit, height) pairs. SyntheticSnapshot carries (2, 12) and (9, 30).
    const size_t tail_size{1 + 2 * (sizeof(uint8_t) + sizeof(int32_t))};
    const auto with_signals = [&](const std::vector<std::pair<uint8_t, int>>& signals) {
        CDataStream stream{SER_DISK, CLIENT_VERSION};
        stream.write(Span{bytes}.first(bytes.size() - tail_size));
        WriteCompactSize(stream, signals.size());
        for (const auto& signal : signals) stream << signal;
        return stream;
    };
    const auto expect_noncanonical = [](CDataStream stream) {
        evo::CEvoSnapshot decoded;
        BOOST_CHECK_EXCEPTION(stream >> decoded, std::ios_base::failure, [](const auto& e) {
            return std::string{e.what()}.find("noncanonical MNHF signal order") != std::string::npos;
        });
    };
    auto canonical{with_signals({{2, 12}, {9, 30}})};
    evo::CEvoSnapshot decoded;
    BOOST_CHECK_NO_THROW(canonical >> decoded);
    BOOST_CHECK(canonical.empty());
    BOOST_CHECK_EQUAL(decoded.mnhf_signals.size(), 2U);
    expect_noncanonical(with_signals({{9, 30}, {2, 12}}));
    expect_noncanonical(with_signals({{2, 12}, {2, 30}}));
}

BOOST_FIXTURE_TEST_CASE(unserialize_replaces_previous_contents, BasicTestingSetup)
{
    const auto populated_bytes{SerializeSnapshot(SyntheticSnapshot())};
    evo::CEvoSnapshot minimal;
    minimal.base_block_hash = H(42);
    minimal.mn_list = CDeterministicMNList{H(42), 500, 0};
    const auto minimal_bytes{SerializeSnapshot(minimal)};

    evo::CEvoSnapshot decoded;
    CDataStream populated{populated_bytes};
    populated >> decoded;
    BOOST_REQUIRE(!decoded.mnhf_signals.empty());
    CDataStream empty{minimal_bytes};
    empty >> decoded;
    BOOST_CHECK(decoded.quorums.empty());
    BOOST_CHECK(decoded.historical_mn_list_diffs.empty());
    BOOST_CHECK(decoded.quorum_modifiers.empty());
    BOOST_CHECK(decoded.mnhf_signals.empty());
    const auto reencoded{SerializeSnapshot(decoded)};
    BOOST_CHECK_EQUAL_COLLECTIONS(minimal_bytes.begin(), minimal_bytes.end(), reencoded.begin(), reencoded.end());
}

BOOST_FIXTURE_TEST_CASE(hash_prefix_collision_runs_are_bounded, BasicTestingSetup)
{
    // Every MN() proTxHash shares CollidingH's 64-bit prefix, so run length
    // equals list size here.
    const auto write_list = [](size_t count) {
        CDataStream stream{SER_DISK, CLIENT_VERSION};
        stream << H(42) << 42 << uint32_t{200};
        WriteCompactSize(stream, count);
        for (size_t i{0}; i < count; ++i) {
            stream << *MN(i + 1, static_cast<uint8_t>(i + 1), MnType::Regular, ProTxVersion::LegacyBLS,
                          static_cast<uint8_t>(i + 1));
        }
        return stream;
    };
    auto at_bound{write_list(evo::EVO_SNAPSHOT_MAX_HASH_PREFIX_RUN)};
    BOOST_CHECK_NO_THROW(evo::UnserializeCanonicalMNList(at_bound));
    auto over_bound{write_list(evo::EVO_SNAPSHOT_MAX_HASH_PREFIX_RUN + 1)};
    BOOST_CHECK_EXCEPTION(evo::UnserializeCanonicalMNList(over_bound), std::ios_base::failure, [](const auto& e) {
        return std::string{e.what()}.find("collision bound") != std::string::npos;
    });

    // A diff addition that would grow an at-bound collision group is rejected
    // before the HAMT performs the inserts.
    evo::CEvoSnapshot snapshot;
    snapshot.base_block_hash = H(42);
    snapshot.mn_list = CDeterministicMNList{H(42), 500, 200};
    for (size_t i{0}; i < evo::EVO_SNAPSHOT_MAX_HASH_PREFIX_RUN; ++i) {
        snapshot.mn_list.AddMN(MN(i + 1, static_cast<uint8_t>(i + 1), MnType::Regular, ProTxVersion::LegacyBLS,
                                  static_cast<uint8_t>(i + 1)),
                               /*fBumpTotalCount=*/false);
    }
    CDeterministicMNListDiff diff;
    diff.addedMNs.push_back(MN(30, 30, MnType::Regular, ProTxVersion::LegacyBLS, 30));
    snapshot.historical_mn_list_diffs.push_back({H(42), H(43), 499, 200, H(1), std::move(diff)});
    std::map<uint256, CDeterministicMNList> lists;
    std::string reconstruction_error;
    BOOST_CHECK(!evo::ReconstructHistoricalMNLists(snapshot, lists, reconstruction_error));
    BOOST_CHECK(reconstruction_error.find("collision bound") != std::string::npos);
}

BOOST_FIXTURE_TEST_CASE(quorum_data_unserialize_replaces_previous_contents, BasicTestingSetup)
{
    evo::CQuorumSnapshotData data;
    data.llmq_type = Consensus::LLMQType::LLMQ_TEST;
    data.active_commitments = {Commitment(data.llmq_type, 11, 51, false)};
    CDataStream once{SER_DISK, CLIENT_VERSION};
    once << data;
    CDataStream twice{SER_DISK, CLIENT_VERSION};
    twice << data;

    evo::CQuorumSnapshotData reused;
    once >> reused;
    twice >> reused;
    BOOST_CHECK_EQUAL(reused.active_commitments.size(), 1U);
    BOOST_CHECK_EQUAL(reused.safety_commitments.size(), 0U);
    BOOST_CHECK_EQUAL(reused.rotation_snapshots.size(), 0U);
}

BOOST_FIXTURE_TEST_CASE(reconstruction_record_budget_is_cumulative, BasicTestingSetup)
{
    const auto snapshot{SyntheticSnapshot()};
    std::map<uint256, CDeterministicMNList> lists;
    std::string error;
    BOOST_REQUIRE(evo::ReconstructHistoricalMNLists(snapshot, lists, error));
    // Every historical entry here carries the same 3-MN list with no
    // additions, so the cumulative charge is exactly 3 records per entry.
    const size_t total_records{3 * snapshot.historical_mn_list_diffs.size()};
    BOOST_CHECK(evo::ReconstructHistoricalMNLists(snapshot, lists, error, total_records));
    BOOST_CHECK(!evo::ReconstructHistoricalMNLists(snapshot, lists, error, total_records - 1));
    BOOST_CHECK(error.find("record budget") != std::string::npos);
}

BOOST_FIXTURE_TEST_CASE(cbtx_cross_checks, BasicTestingSetup)
{
    const auto snapshot{SyntheticSnapshot()};
    CCbTx cbtx;
    cbtx.nVersion = CCbTx::Version::CLSIG_AND_BALANCE;
    cbtx.nHeight = snapshot.mn_list.GetHeightForSnapshotCodec();
    cbtx.merkleRootMNList = snapshot.mn_list.to_sml()->CalcMerkleRoot();
    std::vector<uint256> quorum_hashes;
    for (const auto& data : snapshot.quorums) {
        for (const auto& entry : data.active_commitments) quorum_hashes.emplace_back(SerializeHash(entry.commitment));
    }
    std::sort(quorum_hashes.begin(), quorum_hashes.end());
    cbtx.merkleRootQuorums = ComputeMerkleRoot(quorum_hashes);
    cbtx.creditPoolBalance = snapshot.credit_pool.locked;

    std::string error;
    BOOST_CHECK(evo::VerifyEvoSnapshotCbTx(snapshot, cbtx, error));
    cbtx.merkleRootMNList = H(1);
    BOOST_CHECK(!evo::VerifyEvoSnapshotCbTx(snapshot, cbtx, error));
    cbtx.merkleRootMNList = snapshot.mn_list.to_sml()->CalcMerkleRoot();
    cbtx.merkleRootQuorums = H(2);
    BOOST_CHECK(!evo::VerifyEvoSnapshotCbTx(snapshot, cbtx, error));
    cbtx.merkleRootQuorums = ComputeMerkleRoot(quorum_hashes);
    cbtx.creditPoolBalance++;
    BOOST_CHECK(!evo::VerifyEvoSnapshotCbTx(snapshot, cbtx, error));
    cbtx.creditPoolBalance = snapshot.credit_pool.locked;
    cbtx.nHeight++;
    BOOST_CHECK(!evo::VerifyEvoSnapshotCbTx(snapshot, cbtx, error));
    BOOST_CHECK(error.find("coinbase height") != std::string::npos);
}

BOOST_FIXTURE_TEST_CASE(rejects_unknown_wire_version, BasicTestingSetup)
{
    auto bytes{SerializeSnapshot(SyntheticSnapshot())};
    bytes.data()[0] = std::byte{4};
    evo::CEvoSnapshot decoded;
    BOOST_CHECK_THROW(bytes >> decoded, std::ios_base::failure);
}

BOOST_AUTO_TEST_SUITE_END()
