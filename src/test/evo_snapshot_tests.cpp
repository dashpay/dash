// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <test/util/setup_common.h>

#include <chainparams.h>
#include <clientversion.h>
#include <consensus/amount.h>
#include <consensus/merkle.h>
#include <crypto/sha256.h>
#include <evo/assetlocktx.h>
#include <evo/cbtx.h>
#include <evo/creditpool.h>
#include <evo/evodb.h>
#include <evo/mnhftx.h>
#include <evo/netinfo.h>
#include <evo/smldiff.h>
#include <evo/snapshot.h>
#include <evo/specialtx.h>
#include <evo/specialtxman.h>
#include <hash.h>
#include <llmq/blockprocessor.h>
#include <llmq/context.h>
#include <llmq/options.h>
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
#include <cstddef>
#include <limits>
#include <map>
#include <set>
#include <string>
#include <type_traits>
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

evo::MinedQuorumCommitment Commitment(Consensus::LLMQType type, uint8_t quorum, uint8_t mined, bool rotated,
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

CTransactionRef SyntheticCoinbase(int height)
{
    CMutableTransaction tx;
    tx.nVersion = 3;
    tx.nType = TRANSACTION_COINBASE;
    tx.vin.resize(1);
    tx.vin[0].prevout.SetNull();
    tx.vin[0].scriptSig = CScript{} << height << OP_0;
    tx.vout.emplace_back(50 * COIN, CScript{} << OP_TRUE);
    CCbTx cbtx;
    cbtx.nVersion = CCbTx::Version::CLSIG_AND_BALANCE;
    cbtx.nHeight = height;
    SetTxPayload(tx, cbtx);
    return MakeTransactionRef(std::move(tx));
}

evo::EvoSnapshot SyntheticSnapshot(bool reverse_representation = false, bool with_bootstrap = true)
{
    evo::EvoSnapshot snapshot;
    snapshot.base_block_hash = H(42);
    snapshot.mn_list = MNList(snapshot.base_block_hash, 500, reverse_representation);
    snapshot.credit_pool.locked = 123456;
    snapshot.credit_pool.currentLimit = 700;
    snapshot.credit_pool.latelyUnlocked = 11;
    snapshot.credit_history = {{100'000, 0}, {123'000, 5}, {123'456, 6}};
    snapshot.base_coinbase = SyntheticCoinbase(500);
    snapshot.base_coinbase_branch = {H(201), H(202)};
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

    evo::QuorumSnapshotData plain;
    plain.llmq_type = Consensus::LLMQType::LLMQ_TEST;
    plain.active_commitments = {Commitment(plain.llmq_type, 11, 51, false), Commitment(plain.llmq_type, 12, 52, false)};
    plain.safety_commitments = {Commitment(plain.llmq_type, 10, 50, false)};

    evo::QuorumSnapshotData rotated;
    rotated.llmq_type = Consensus::LLMQType::LLMQ_TEST_DIP0024;
    rotated.rotation_enabled = true;
    rotated.active_commitments = {Commitment(rotated.llmq_type, 31, 71, true, 0),
                                  Commitment(rotated.llmq_type, 32, 72, true, 1)};
    rotated.safety_commitments = {Commitment(rotated.llmq_type, 21, 61, true, 0),
                                  Commitment(rotated.llmq_type, 22, 62, true, 1)};
    for (uint8_t i{1}; i <= 4; ++i) {
        const auto mode{i == 2 ? SnapshotSkipMode::MODE_SKIPPING_ENTRIES : SnapshotSkipMode::MODE_NO_SKIPPING};
        rotated.rotation_snapshots.push_back(
            {H(40 + i), H(100 + i), llmq::CQuorumSnapshot{{true, false, true, false}, mode, i == 2 ? std::vector<int>{1} : std::vector<int>{}}});
    }
    if (with_bootstrap) rotated.bootstrap_work = {{48, H(141)}, {72, H(142)}};

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
        for (const auto& entry : data.bootstrap_work) {
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
            std::reverse(data.bootstrap_work.begin(), data.bootstrap_work.end());
        }
    }
    return snapshot;
}

std::vector<unsigned char> SerializeTransactionForTest(const CTransaction& tx)
{
    CDataStream stream{SER_NETWORK, PROTOCOL_VERSION};
    stream << tx;
    return {UCharCast(stream.data()), UCharCast(stream.data() + stream.size())};
}

//! The wire bytes of a canonically ordered snapshot, assembled field by field
//! in the order of the format table rather than through EvoSnapshot's own
//! serializer, so a pinned hash of them is independent of the codec under
//! test. Version 3 omits every field v4 added.
std::vector<unsigned char> AssembleWireBytes(const evo::EvoSnapshot& snapshot, uint16_t version)
{
    CDataStream s{SER_DISK, CLIENT_VERSION};
    s << version << snapshot.base_block_hash;
    evo::SerializeCanonicalMNList(s, snapshot.mn_list);
    if (version >= 4) {
        if (snapshot.base_coinbase) {
            const auto coinbase{SerializeTransactionForTest(*snapshot.base_coinbase)};
            WriteCompactSize(s, coinbase.size());
            s.write(MakeByteSpan(coinbase));
        } else {
            WriteCompactSize(s, 0);
        }
        WriteCompactSize(s, snapshot.base_coinbase_branch.size());
        for (const auto& node : snapshot.base_coinbase_branch)
            s << node;
    }
    WriteCompactSize(s, snapshot.quorums.size());
    for (const auto& data : snapshot.quorums) {
        s << static_cast<uint8_t>(data.llmq_type) << uint8_t{data.rotation_enabled};
        for (const auto* commitments : {&data.active_commitments, &data.safety_commitments}) {
            WriteCompactSize(s, commitments->size());
            for (const auto& entry : *commitments) {
                s << entry.quorum_base_block_hash << entry.work_block_hash << entry.commitment << entry.mined_block_hash;
            }
        }
        WriteCompactSize(s, data.rotation_snapshots.size());
        for (const auto& entry : data.rotation_snapshots)
            evo::WriteRotationSnapshot(s, entry);
        if (version >= 4) {
            WriteCompactSize(s, data.bootstrap_work.size());
            for (const auto& entry : data.bootstrap_work)
                s << int32_t{entry.cycle_base_height} << entry.work_block_hash;
        }
    }
    WriteCompactSize(s, snapshot.historical_mn_list_diffs.size());
    for (const auto& entry : snapshot.historical_mn_list_diffs) {
        s << entry.previous_block_hash << entry.block_hash << int32_t{entry.height} << entry.total_registered_count
          << entry.canonical_list_hash;
        evo::SerializeCanonicalMNListDiff(s, entry.diff);
    }
    WriteCompactSize(s, snapshot.quorum_modifiers.size());
    for (const auto& entry : snapshot.quorum_modifiers) {
        s << static_cast<uint8_t>(entry.llmq_type) << entry.work_block_hash << entry.modifier;
    }
    s << snapshot.credit_pool.locked << snapshot.credit_pool.currentLimit << snapshot.credit_pool.latelyUnlocked
      << snapshot.credit_pool.indexes;
    if (version >= 4) {
        WriteCompactSize(s, snapshot.credit_history.size());
        for (const auto& entry : snapshot.credit_history)
            s << entry.credit_pool << entry.unlocked;
    }
    WriteCompactSize(s, snapshot.mnhf_signals.size());
    for (const auto& [bit, height] : snapshot.mnhf_signals)
        s << bit << int32_t{height};
    return {UCharCast(s.data()), UCharCast(s.data() + s.size())};
}

std::string Sha256Hex(const std::vector<unsigned char>& bytes)
{
    uint256 hash;
    CSHA256().Write(bytes.data(), bytes.size()).Finalize(hash.begin());
    return hash.ToString();
}

CDataStream SerializeSnapshot(const evo::EvoSnapshot& snapshot)
{
    CDataStream stream{SER_DISK, CLIENT_VERSION};
    stream << snapshot;
    return stream;
}

void CheckInvalid(evo::EvoSnapshot snapshot) { BOOST_CHECK_THROW(snapshot.Validate(), std::ios_base::failure); }

// The snapshot decoder inserts attacker-chosen update IDs straight into this
// map, so its hash must not be the identity an adversary can drive into one
// bucket.
static_assert(std::is_same_v<decltype(CDeterministicMNListDiff::updatedMNs)::hasher, StaticSaltedHasher>);

//! Restores consensus params mutated through const_cast when the test case
//! leaves scope, including through a failed BOOST_REQUIRE, so mutated state
//! cannot leak into cases running later in the same process.
class [[nodiscard]] ConsensusParamsRestorer
{
    Consensus::Params& m_params;
    const Consensus::Params m_saved;

public:
    explicit ConsensusParamsRestorer(const Consensus::Params& params) :
        m_params{const_cast<Consensus::Params&>(params)},
        m_saved{params}
    {
    }
    ~ConsensusParamsRestorer() { m_params = m_saved; }
    Consensus::Params& Get() { return m_params; }
};

//! A masternode whose hashes have uniform 64-bit prefixes, so lists of any
//! size stay inside the snapshot's collision-run bound.
CDeterministicMNCPtr ChainMN(uint64_t id)
{
    const auto tagged = [id](const char* tag) { return SerializeHash(std::make_pair(std::string{tag}, id)); };
    auto state{std::make_shared<CDeterministicMNState>()};
    state->nVersion = ProTxVersion::LegacyBLS;
    state->nRegisteredHeight = 1;
    state->keyIDOwner = CKeyID{uint160{Span{tagged("owner").begin(), 20}}};
    state->keyIDVoting = CKeyID{uint160{Span{tagged("voting").begin(), 20}}};
    state->scriptPayout = CScript{} << OP_RETURN << ToByteVector(tagged("payout"));
    state->netInfo = NetInfoInterface::MakeNetInfo(state->nVersion);
    BOOST_REQUIRE_EQUAL(state->netInfo->AddEntry(NetInfoPurpose::CORE_P2P, strprintf("1.2.%d.%d:%d", id / 250, 1 + id % 250,
                                                                                     Params().GetDefaultPort())),
                        NetInfoStatus::Success);
    auto dmn{std::make_shared<CDeterministicMN>(id, MnType::Regular)};
    dmn->proTxHash = tagged("protx");
    dmn->collateralOutpoint = COutPoint(tagged("collateral"), 0);
    state->UpdateConfirmedHash(dmn->proTxHash, tagged("confirmed"));
    dmn->pdmnState = std::move(state);
    return dmn;
}

//! The synthetic population the chain fixture records at `index`: it grows
//! over time, so consecutive work blocks have different lists.
CDeterministicMNList ChainList(const CBlockIndex* index, bool empty = false)
{
    const uint32_t count{static_cast<uint32_t>(6 + index->nHeight / 8)};
    CDeterministicMNList list{index->GetBlockHash(), index->nHeight, count};
    if (empty) return list;
    for (uint64_t id{0}; id < count; ++id)
        list.AddMN(ChainMN(id), /*fBumpTotalCount=*/false);
    return list;
}

std::vector<uint256> MemberHashes(const std::vector<CDeterministicMNCPtr>& members)
{
    std::vector<uint256> hashes;
    hashes.reserve(members.size());
    for (const auto& member : members)
        hashes.push_back(member->proTxHash);
    return hashes;
}

//! Build the Evo section at `base` from the synced NORMAL view.
bool TryBuildEvoSnapshot(node::NodeContext& node, const CBlockIndex* base, evo::EvoSnapshot& snapshot, std::string& error)
{
    LOCK(::cs_main);
    const EvoViewScope normal{node.evodb->ViewFor(EvoDbIdentity::NORMAL)};
    return evo::BuildEvoSnapshot(Params(), *node.chainman, *node.dmnman, *node.llmq_ctx->quorum_block_processor,
                                 *node.llmq_ctx->qsnapman, *node.special_tx->m_cpoolman, *node.special_tx->m_mnhfman,
                                 base, snapshot, error);
}

evo::EvoSnapshot BuildEvoSnapshotAt(node::NodeContext& node, const CBlockIndex* base)
{
    evo::EvoSnapshot snapshot;
    std::string error;
    BOOST_REQUIRE_MESSAGE(TryBuildEvoSnapshot(node, base, snapshot, error), error);
    return snapshot;
}

bool ValidAgainstChain(const node::NodeContext& node, const evo::EvoSnapshot& snapshot, const CBlockIndex* base,
                       std::string& error)
{
    return WITH_LOCK(::cs_main, return evo::ValidateEvoSnapshotAgainstChain(snapshot, *node.chainman, base, error));
}

//! Seed `snapshot` into a fresh unpublished generation and return its view.
EvoViewToken SeedGeneration(node::NodeContext& node, const evo::EvoSnapshot& snapshot, const CBlockIndex* base)
{
    LOCK(::cs_main);
    if (node.evodb->ViewFor(EvoDbIdentity::SNAPSHOT).generation != 0) {
        BOOST_REQUIRE(node.evodb->AbandonSnapshotGeneration());
    }
    BOOST_REQUIRE(node.evodb->CreateSnapshotGeneration(base->GetBlockHash(), base->nHeight));
    const EvoViewToken view{node.evodb->ViewFor(EvoDbIdentity::SNAPSHOT)};
    {
        auto tx{node.evodb->BeginTransaction(EvoDbIdentity::SNAPSHOT)};
        std::string error;
        BOOST_REQUIRE_MESSAGE(evo::SeedEvoSnapshotState(view, snapshot, *node.special_tx, node.chainman->m_blockman,
                                                        base, error),
                              error);
        tx->Commit();
    }
    BOOST_REQUIRE(node.evodb->CommitRootTransaction(EvoDbIdentity::SNAPSHOT));
    return view;
}

} // namespace

//! A chain whose activation heights are in force while it is mined, so every
//! coinbase is the CbTx that v20-era code paths are entitled to assume.
struct SnapshotActivationChainSetup : public TestChainSetup {
    SnapshotActivationChainSetup() :
        TestChainSetup{102,
                       CBaseChainParams::REGTEST,
                       {"-dip3params=2:2", "-testactivationheight=v20@2", "-testactivationheight=mn_rr@2"}}
    {
    }
};

/**
 * A 200-block regtest chain with DIP3 and v20 from height 2, a non-rotating
 * (LLMQ_TEST) and a rotating (LLMQ_TEST_DIP0024) type, both with 12-block
 * cycles. NORMAL is the fully synced oracle: it holds a list for every block,
 * the quarter of every cycle, built in order, and the commitments a scenario
 * mines. The snapshot view of a generation holds only what is seeded into it.
 */
struct EvoSnapshotChainSetup : public TestChainSetup {
    ConsensusParamsRestorer m_restorer{Params().GetConsensus()};
    Consensus::LLMQParams m_plain{evo::SnapshotLLMQParams(Consensus::LLMQType::LLMQ_TEST)};
    Consensus::LLMQParams m_rotated{evo::SnapshotLLMQParams(Consensus::LLMQType::LLMQ_TEST_DIP0024)};
    static constexpr int CYCLE_LENGTH{12};
    //! The secret key of each mined quorum, so tests can sign as it.
    std::map<std::pair<Consensus::LLMQType, int>, CBLSSecretKey> m_quorum_keys;

    EvoSnapshotChainSetup() :
        TestChainSetup{102,
                       CBaseChainParams::REGTEST,
                       {"-dip3params=2:2", "-testactivationheight=v20@2", "-testactivationheight=mn_rr@2"}}
    {
        mineBlocks(98);
        BOOST_REQUIRE(&Params().GetConsensus() == &m_node.chainman->GetConsensus());
        for (auto* params : {&m_plain, &m_rotated}) {
            params->dkgInterval = CYCLE_LENGTH;
            params->dkgMiningWindowStart = 2;
            params->dkgMiningWindowEnd = 6;
        }
        // ChainLocks and Platform run on a type that forms no quorum here:
        // these chains have no ChainLocks, so a base certifies none and its
        // ChainLock scans would reach back to genesis, and Platform quorums
        // select only evonodes. The scan-coverage tests choose their own.
        auto& consensus{m_restorer.Get()};
        consensus.llmqs = {m_plain, m_rotated, evo::SnapshotLLMQParams(Consensus::LLMQType::LLMQ_TEST_INSTANTSEND)};
        consensus.llmqTypeChainLocks = Consensus::LLMQType::LLMQ_TEST_INSTANTSEND;
        consensus.llmqTypePlatform = Consensus::LLMQType::LLMQ_TEST_INSTANTSEND;
    }

    const CBlockIndex* At(int height) { return WITH_LOCK(::cs_main, return m_node.chainman->ActiveChain()[height]); }
    EvoViewToken Normal() const { return m_node.evodb->ViewFor(EvoDbIdentity::NORMAL); }
    llmq::UtilParameters Util(const CBlockIndex* index)
    {
        return {*m_node.dmnman, *m_node.llmq_ctx->qsnapman, *m_node.chainman, index};
    }

    //! Record the synthetic lists in NORMAL, then build every cycle's quarter
    //! in order, as a node that validated each cycle would have.
    void SyncNormal(const std::set<int>& empty_heights = {})
    {
        LOCK(::cs_main);
        const CBlockIndex* tip{m_node.chainman->ActiveTip()};
        {
            auto tx{m_node.evodb->BeginTransaction(EvoDbIdentity::NORMAL)};
            for (int height{2}; height <= tip->nHeight; ++height) {
                const CBlockIndex* index{tip->GetAncestor(height)};
                m_node.evodb->Erase(std::make_pair(std::string{"dmn_S3"}, index->GetBlockHash()));
                BOOST_REQUIRE(m_node.dmnman->SeedListForBlock(Normal(), ChainList(index, empty_heights.contains(height))));
            }
            tx->Commit();
        }
        BOOST_REQUIRE(m_node.evodb->CommitRootTransaction(EvoDbIdentity::NORMAL));
        const EvoViewScope normal{Normal()};
        for (int cycle{CYCLE_LENGTH}; cycle <= tip->nHeight; cycle += CYCLE_LENGTH) {
            (void)llmq::utils::GetAllQuorumMembers(m_rotated.type, Util(tip->GetAncestor(cycle)), /*reset_cache=*/true);
        }
    }

    //! Record a commitment for the quorum at `quorum_height`, mined two blocks
    //! after it, as block connection would.
    void Mine(const Consensus::LLMQParams& params, int quorum_height)
    {
        LOCK(::cs_main);
        const CBlockIndex* quorum{At(quorum_height)};
        const bool rotation{params.useRotation};
        llmq::CFinalCommitment commitment{params, quorum->GetBlockHash()};
        commitment.nVersion = llmq::CFinalCommitment::GetVersion(
            rotation, DeploymentActiveAfter(quorum, m_node.chainman->GetConsensus(), Consensus::DEPLOYMENT_V19));
        commitment.quorumIndex = rotation ? quorum_height % params.dkgInterval : 0;
        commitment.quorumVvecHash = SerializeHash(std::make_pair(params.type, quorum_height));
        auto& key{m_quorum_keys[{params.type, quorum_height}]};
        key.MakeNewKey();
        commitment.quorumPublicKey = key.GetPublicKey();
        std::fill(commitment.signers.begin(), commitment.signers.end(), true);
        std::fill(commitment.validMembers.begin(), commitment.validMembers.end(), true);
        auto tx{m_node.evodb->BeginTransaction(EvoDbIdentity::NORMAL)};
        BOOST_REQUIRE(m_node.llmq_ctx->quorum_block_processor->SeedMinedCommitment(Normal(), params.type,
                                                                                   quorum->GetBlockHash(), commitment,
                                                                                   At(quorum_height + 2)->GetBlockHash()));
        tx->Commit();
        BOOST_REQUIRE(m_node.evodb->CommitRootTransaction(EvoDbIdentity::NORMAL));
    }

    //! Mine every quorum of each cycle in [first, last]: the non-rotating one
    //! and rotating index 0, plus index 1 unless its DKG keeps failing.
    void MineCycles(int first, int last, bool rotated_index_one = true)
    {
        for (int cycle{first}; cycle <= last; cycle += CYCLE_LENGTH) {
            Mine(m_plain, cycle);
            Mine(m_rotated, cycle);
            if (rotated_index_one) Mine(m_rotated, cycle + 1);
        }
    }

    bool TryBuild(const CBlockIndex* base, evo::EvoSnapshot& snapshot, std::string& error)
    {
        return TryBuildEvoSnapshot(m_node, base, snapshot, error);
    }
    evo::EvoSnapshot Build(const CBlockIndex* base) { return BuildEvoSnapshotAt(m_node, base); }
    bool Valid(const evo::EvoSnapshot& snapshot, const CBlockIndex* base, std::string& error)
    {
        return ValidAgainstChain(m_node, snapshot, base, error);
    }
    EvoViewToken Seed(const evo::EvoSnapshot& snapshot, const CBlockIndex* base)
    {
        return SeedGeneration(m_node, snapshot, base);
    }

    std::vector<uint256> Members(const EvoViewToken& view, const Consensus::LLMQParams& params, int quorum_height)
    {
        const EvoViewScope scope{view};
        return MemberHashes(llmq::utils::GetAllQuorumMembers(params.type, Util(At(quorum_height)), /*reset_cache=*/true));
    }

    //! Members of a quorum under `view` match the synced oracle's, and the
    //! quorum is not trivially empty.
    void CheckMembers(const EvoViewToken& view, const Consensus::LLMQParams& params, int quorum_height)
    {
        const auto expected{Members(Normal(), params, quorum_height)};
        BOOST_CHECK_MESSAGE(!expected.empty(), strprintf("empty oracle quorum at %d", quorum_height));
        BOOST_CHECK_MESSAGE(Members(view, params, quorum_height) == expected,
                            strprintf("type %d quorum at %d", static_cast<int>(params.type), quorum_height));
    }

    std::optional<std::vector<uint256>> Predicted(const EvoViewToken& view, const Consensus::LLMQParams& params,
                                                  int quorum_height)
    {
        const EvoViewScope scope{view};
        const CBlockIndex* work{At(quorum_height - quorum_height % params.dkgInterval - llmq::WORK_DIFF_DEPTH)};
        const auto members{llmq::utils::ComputeQuorumMembersFromWorkBlock(params.type, Util(work), work, quorum_height)};
        if (!members) return std::nullopt;
        return MemberHashes(*members);
    }

    static const evo::QuorumSnapshotData& Type(const evo::EvoSnapshot& snapshot, Consensus::LLMQType type)
    {
        const auto it{std::ranges::find_if(snapshot.quorums, [type](const auto& data) { return data.llmq_type == type; })};
        BOOST_REQUIRE(it != snapshot.quorums.end());
        return *it;
    }

    std::set<int> Heights(const std::vector<uint256>& hashes)
    {
        std::set<int> heights;
        LOCK(::cs_main);
        for (const auto& hash : hashes)
            heights.insert(m_node.chainman->m_blockman.LookupBlockIndex(hash)->nHeight);
        return heights;
    }

    std::set<int> RotationHeights(const evo::EvoSnapshot& snapshot)
    {
        std::vector<uint256> hashes;
        for (const auto& entry : Type(snapshot, m_rotated.type).rotation_snapshots) {
            hashes.push_back(entry.cycle_base_block_hash);
        }
        return Heights(hashes);
    }

    std::set<int> ModifierHeights(const evo::EvoSnapshot& snapshot, Consensus::LLMQType type)
    {
        std::vector<uint256> hashes;
        for (const auto& entry : snapshot.quorum_modifiers) {
            if (entry.llmq_type == type) hashes.push_back(entry.work_block_hash);
        }
        return Heights(hashes);
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
    evo::EvoSnapshot decoded;
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

BOOST_AUTO_TEST_CASE(rotation_bitset_larger_than_quorum_roundtrips)
{
    const auto& params{evo::SnapshotLLMQParams(Consensus::LLMQType::LLMQ_TEST_DIP0024)};
    evo::QuorumSnapshotEntry entry;
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

BOOST_FIXTURE_TEST_CASE(populated_v4_golden_value, BasicTestingSetup)
{
    // The assembler reproduces the v3 bytes whose hash this test pinned before
    // the format changed, so it encodes the shared fields independently of the
    // codec. The v4 pin is its v4 output.
    const auto v3_content{SyntheticSnapshot(/*reverse_representation=*/false, /*with_bootstrap=*/false)};
    BOOST_CHECK_EQUAL(Sha256Hex(AssembleWireBytes(v3_content, 3)),
                      "5b5d496a66d93775a6a6d1badf2ea2d5bbb25b58031fe0dd2c3c670674c6908d");

    const auto snapshot{SyntheticSnapshot()};
    BOOST_REQUIRE_NO_THROW(snapshot.Validate(/*require_canonical_order=*/true));
    const auto assembled{AssembleWireBytes(snapshot, 4)};
    const auto encoded{SerializeSnapshot(snapshot)};
    BOOST_CHECK_EQUAL_COLLECTIONS(assembled.begin(), assembled.end(), UCharCast(encoded.data()),
                                  UCharCast(encoded.data() + encoded.size()));
    BOOST_CHECK_EQUAL(Sha256Hex(assembled), "31be7fa4afc67c3a0ca288e1ed991d488f8d64d755f9d65c35c0ff5a1326e652");
    BOOST_CHECK_EQUAL(GetEvoSnapshotHash(snapshot).ToString(), Sha256Hex(assembled));
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

BOOST_FIXTURE_TEST_CASE(inactive_mn_roundtrip, BasicTestingSetup)
{
    for (const int version : {ProTxVersion::LegacyBLS, ProTxVersion::ExtAddr}) {
        auto inactive{std::make_shared<CDeterministicMN>(*MN(2, 3, MnType::Regular, version, 3))};
        auto state{std::make_shared<CDeterministicMNState>(*inactive->pdmnState)};
        state->netInfo = NetInfoInterface::MakeNetInfo(version);
        state->BanIfNotBanned(100);
        inactive->pdmnState = state;
        BOOST_REQUIRE(state->netInfo->IsEmpty());

        evo::EvoSnapshot snapshot;
        snapshot.base_block_hash = H(42);
        snapshot.mn_list = CDeterministicMNList{snapshot.base_block_hash, 500, 10};
        snapshot.base_coinbase = SyntheticCoinbase(500);
        snapshot.mn_list.AddMN(inactive, /*fBumpTotalCount=*/false);
        BOOST_CHECK_NO_THROW(snapshot.Validate());
        auto bytes{SerializeSnapshot(snapshot)};
        evo::EvoSnapshot decoded;
        BOOST_REQUIRE_NO_THROW(bytes >> decoded);
        BOOST_CHECK(bytes.empty());
        BOOST_CHECK(GetEvoSnapshotHash(snapshot) == GetEvoSnapshotHash(decoded));

        // Cover both historical additions and updates to empty network info.
        for (const bool addition : {false, true}) {
            CDeterministicMNList base{H(41), 501, 10};
            if (!addition) base.AddMN(MN(2, 3, MnType::Regular, version, 3), /*fBumpTotalCount=*/false);
            const auto diff{base.BuildDiff(snapshot.mn_list)};
            CDataStream encoded{SER_DISK, CLIENT_VERSION};
            evo::SerializeCanonicalMNListDiff(encoded, diff);
            const auto decoded_diff{evo::UnserializeCanonicalMNListDiff(encoded)};
            evo::EvoSnapshot historical;
            historical.base_block_hash = base.GetBlockHash();
            historical.mn_list = base;
            historical.historical_mn_list_diffs = {{base.GetBlockHash(), snapshot.base_block_hash, 500, 10,
                                                    evo::CanonicalMNListHash(snapshot.mn_list), decoded_diff}};
            std::map<uint256, CDeterministicMNList> lists;
            std::string error;
            BOOST_REQUIRE_MESSAGE(evo::ReconstructHistoricalMNLists(historical, lists, error), error);
            BOOST_REQUIRE_EQUAL(lists.size(), 1U);
            BOOST_CHECK(evo::CanonicalMNListHash(lists.begin()->second) == evo::CanonicalMNListHash(snapshot.mn_list));
        }
    }
}

BOOST_FIXTURE_TEST_CASE(historical_diff_applies_exchanged_unique_properties, BasicTestingSetup)
{
    // Between two work blocks a surviving MN can take an address another
    // survivor still holds at the first endpoint, and a new registration can
    // take an address an updated MN released. Both endpoint lists are valid,
    // so the diff between them must apply regardless of update order.
    const auto older{MNList(H(10), 100, false)};
    const auto with_net_info = [](const CDeterministicMN& dmn, std::shared_ptr<NetInfoInterface> net_info) {
        auto state{std::make_shared<CDeterministicMNState>(*dmn.pdmnState)};
        state->netInfo = std::move(net_info);
        auto copy{std::make_shared<CDeterministicMN>(dmn)};
        copy->pdmnState = std::move(state);
        return copy;
    };
    const auto a{older.GetMNByInternalId(2)};
    const auto b{older.GetMNByInternalId(7)};
    auto fresh{NetInfoInterface::MakeNetInfo(b->pdmnState->nVersion)};
    BOOST_REQUIRE_EQUAL(fresh->AddEntry(NetInfoPurpose::CORE_P2P, strprintf("1.1.1.9:%d", Params().GetDefaultPort())),
                        NetInfoStatus::Success);
    CDeterministicMNList newer{H(11), 101, 10};
    newer.AddMN(with_net_info(*a, b->pdmnState->netInfo), /*fBumpTotalCount=*/false);
    newer.AddMN(with_net_info(*b, fresh), /*fBumpTotalCount=*/false);
    newer.AddMN(older.GetMNByInternalId(5), /*fBumpTotalCount=*/false);
    newer.AddMN(with_net_info(*MN(8, 8, MnType::Regular, ProTxVersion::LegacyBLS, 8), a->pdmnState->netInfo),
                /*fBumpTotalCount=*/false);

    const CDeterministicMNList* endpoints[]{&older, &newer};
    for (const auto* from : endpoints) {
        const auto& to{from == &older ? newer : older};
        CDataStream encoded{SER_DISK, CLIENT_VERSION};
        evo::SerializeCanonicalMNListDiff(encoded, from->BuildDiff(to));
        const auto diff{evo::UnserializeCanonicalMNListDiff(encoded)};
        BOOST_REQUIRE_EQUAL(diff.updatedMNs.size(), 2U);
        BOOST_REQUIRE_EQUAL(diff.addedMNs.size() + diff.removedMns.size(), 1U);
        auto applied{*from};
        applied.ApplyDiffForSnapshot(to.GetBlockHash(), to.GetHeightForSnapshotCodec(), to.GetTotalRegisteredCount(), diff);
        BOOST_CHECK(evo::CanonicalMNListHash(applied) == evo::CanonicalMNListHash(to));

        // A diff whose result would hold one address twice is still rejected.
        CDeterministicMNListDiff conflicting;
        conflicting.updatedMNs.emplace(2, diff.updatedMNs.at(2));
        auto rejected{*from};
        BOOST_CHECK_THROW(rejected.ApplyDiffForSnapshot(to.GetBlockHash(), to.GetHeightForSnapshotCodec(),
                                                        to.GetTotalRegisteredCount(), conflicting),
                          std::runtime_error);
    }

    // The full reconstruction path, which re-checks every encoding around the
    // apply, accepts the exchange as well.
    evo::EvoSnapshot snapshot;
    snapshot.base_block_hash = newer.GetBlockHash();
    snapshot.mn_list = newer;
    snapshot.historical_mn_list_diffs = {{newer.GetBlockHash(), older.GetBlockHash(), older.GetHeightForSnapshotCodec(),
                                          older.GetTotalRegisteredCount(), evo::CanonicalMNListHash(older),
                                          newer.BuildDiff(older)}};
    std::map<uint256, CDeterministicMNList> lists;
    std::string error;
    BOOST_REQUIRE_MESSAGE(evo::ReconstructHistoricalMNLists(snapshot, lists, error), error);
    BOOST_REQUIRE_EQUAL(lists.size(), 1U);
    BOOST_CHECK(evo::CanonicalMNListHash(lists.begin()->second) == evo::CanonicalMNListHash(older));
}

BOOST_FIXTURE_TEST_CASE(historical_diff_additions_require_new_identities, BasicTestingSetup)
{
    const auto original{MNList(H(11), 101, false)};
    const auto original_hash{evo::CanonicalMNListHash(original)};
    const auto existing{original.GetMNByInternalId(2)};
    auto same_hash{std::make_shared<CDeterministicMN>(*MN(8, 8, MnType::Regular, ProTxVersion::LegacyBLS, 8))};
    same_hash->proTxHash = existing->proTxHash;
    const std::vector<CDeterministicMNCPtr> additions{
        existing,
        MN(2, 8, MnType::Regular, ProTxVersion::LegacyBLS, 8),
        same_hash,
    };
    for (const bool remove_existing : {false, true}) {
        for (const auto& addition : additions) {
            auto list{original};
            CDeterministicMNListDiff diff;
            if (remove_existing) diff.removedMns.emplace(existing->GetInternalId());
            diff.addedMNs.push_back(addition);
            BOOST_CHECK_EXCEPTION(list.ApplyDiffForSnapshot(H(10), 100, original.GetTotalRegisteredCount(), diff),
                                  std::runtime_error, [](const auto& e) {
                                      return std::string{e.what()} ==
                                             "historical MN-diff addition reuses an existing identity";
                                  });
            BOOST_CHECK(evo::CanonicalMNListHash(list) == original_hash);
        }
    }
}

BOOST_FIXTURE_TEST_CASE(malformed_lazy_operator_keys_are_rejected, BasicTestingSetup)
{
    const auto noncanonical = [](const auto& e) {
        return std::string{e.what()}.find("noncanonical MN object encoding") != std::string::npos;
    };
    const std::vector<std::byte> undecodable(CBLSPublicKey::SerSize, std::byte{0xff});
    for (const int version : {ProTxVersion::LegacyBLS, ProTxVersion::BasicBLS}) {
        const bool legacy{version == ProTxVersion::LegacyBLS};
        auto mn{std::make_shared<CDeterministicMN>(*MN(2, 3, MnType::Regular, version, 3))};
        auto state{std::make_shared<CDeterministicMNState>(*mn->pdmnState)};
        CDataStream key_bytes{SER_DISK, CLIENT_VERSION};
        key_bytes.write(undecodable);
        key_bytes >> CBLSLazyPublicKeyVersionWrapper(state->pubKeyOperator, legacy);
        mn->pdmnState = state;

        // The lazy wrapper re-emits undecodable bytes verbatim until the key is
        // first read, after which it emits the empty key: two encodings of one
        // object, so the decoder must reject the bytes outright.
        CDataStream lazy{SER_DISK, CLIENT_VERSION};
        lazy << *mn;
        BOOST_CHECK(!mn->pdmnState->pubKeyOperator.Get().IsValid());
        CDataStream materialized{SER_DISK, CLIENT_VERSION};
        materialized << *mn;
        BOOST_REQUIRE(lazy.str() != materialized.str());

        CDataStream list{SER_DISK, CLIENT_VERSION};
        list << H(42) << 500 << uint32_t{10};
        WriteCompactSize(list, 1);
        list.write(Span{lazy});
        BOOST_CHECK_EXCEPTION(evo::UnserializeCanonicalMNList(list), std::ios_base::failure, noncanonical);

        CDataStream addition{SER_DISK, CLIENT_VERSION};
        WriteCompactSize(addition, 1);
        addition.write(Span{lazy});
        WriteCompactSize(addition, 0);
        WriteCompactSize(addition, 0);
        BOOST_CHECK_EXCEPTION(evo::UnserializeCanonicalMNListDiff(addition), std::ios_base::failure, noncanonical);

        CDeterministicMNStateDiff state_diff;
        state_diff.fields = CDeterministicMNStateDiff::Field_nVersion | CDeterministicMNStateDiff::Field_pubKeyOperator;
        state_diff.state.nVersion = version;
        key_bytes.write(undecodable);
        key_bytes >> CBLSLazyPublicKeyVersionWrapper(state_diff.state.pubKeyOperator, legacy);
        CDataStream update{SER_DISK, CLIENT_VERSION};
        WriteCompactSize(update, 0);
        WriteCompactSize(update, 1);
        WriteVarInt<CDataStream, VarIntMode::DEFAULT, uint64_t>(update, 2);
        update << state_diff;
        WriteCompactSize(update, 0);
        BOOST_CHECK_EXCEPTION(evo::UnserializeCanonicalMNListDiff(update), std::ios_base::failure, noncanonical);

        // An in-memory snapshot holding the unread bytes must not hash either.
        lazy.clear();
        lazy << *MN(2, 3, MnType::Regular, version, 3);
        auto unread{std::make_shared<CDeterministicMN>(deserialize, lazy)};
        BOOST_REQUIRE(lazy.empty());
        auto unread_state{std::make_shared<CDeterministicMNState>(*unread->pdmnState)};
        key_bytes.write(undecodable);
        key_bytes >> CBLSLazyPublicKeyVersionWrapper(unread_state->pubKeyOperator, legacy);
        unread->pdmnState = unread_state;
        evo::EvoSnapshot snapshot;
        snapshot.base_block_hash = H(42);
        snapshot.mn_list = CDeterministicMNList{snapshot.base_block_hash, 500, 10};
        snapshot.base_coinbase = SyntheticCoinbase(500);
        snapshot.mn_list.AddMN(unread, /*fBumpTotalCount=*/false);
        BOOST_CHECK_EXCEPTION(snapshot.Validate(), std::ios_base::failure, noncanonical);
        BOOST_CHECK_THROW(GetEvoSnapshotHash(snapshot), std::ios_base::failure);

        // A decodable key keeps one encoding whether or not it has been read.
        CBLSSecretKey secret;
        BOOST_REQUIRE(secret.SetHexStr("000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f", legacy));
        auto keyed_state{std::make_shared<CDeterministicMNState>(*unread_state)};
        keyed_state->pubKeyOperator.Set(secret.GetPublicKey(), legacy);
        unread->pdmnState = keyed_state;
        BOOST_CHECK_NO_THROW(snapshot.Validate());
        const auto before_read{GetEvoSnapshotHash(snapshot)};
        auto bytes{SerializeSnapshot(snapshot)};
        evo::EvoSnapshot decoded;
        BOOST_REQUIRE_NO_THROW(bytes >> decoded);
        BOOST_CHECK(decoded.mn_list.GetMNByInternalId(2)->pdmnState->pubKeyOperator.Get().IsValid());
        BOOST_CHECK(GetEvoSnapshotHash(decoded) == before_read);
    }
}

BOOST_FIXTURE_TEST_CASE(unset_mn_id_is_rejected, BasicTestingSetup)
{
    const auto mn{MN(2, 3, MnType::Regular, ProTxVersion::LegacyBLS, 3)};
    CDataStream malformed{SER_DISK, CLIENT_VERSION};
    malformed << mn->proTxHash;
    WriteVarInt<CDataStream, VarIntMode::DEFAULT, uint64_t>(malformed, std::numeric_limits<uint64_t>::max());
    malformed << mn->collateralOutpoint << mn->nOperatorReward << mn->pdmnState << mn->nType;
    const auto invalid_id = [](const auto& e) {
        return std::string{e.what()}.find("invalid canonical MN internalId") != std::string::npos;
    };
    CDataStream base{SER_DISK, CLIENT_VERSION};
    base << H(42) << 500 << uint32_t{10};
    WriteCompactSize(base, 1);
    base.write(Span{malformed});
    BOOST_CHECK_EXCEPTION(evo::UnserializeCanonicalMNList(base), std::ios_base::failure, invalid_id);

    CDataStream addition{SER_DISK, CLIENT_VERSION};
    WriteCompactSize(addition, 1);
    addition.write(Span{malformed});
    WriteCompactSize(addition, 0);
    WriteCompactSize(addition, 0);
    BOOST_CHECK_EXCEPTION(evo::UnserializeCanonicalMNListDiff(addition), std::ios_base::failure, invalid_id);
}

BOOST_FIXTURE_TEST_CASE(commitment_bls_wire_encoding_is_canonical, BasicTestingSetup)
{
    CBLSSecretKey key;
    BOOST_REQUIRE(key.SetHexStr("000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f", false));
    for (const bool legacy : {false, true}) {
        for (const bool indexed : {false, true}) {
            auto entry{Commitment(Consensus::LLMQType::LLMQ_TEST, 11, 51, indexed)};
            if (legacy) {
                entry.commitment.nVersion = indexed ? llmq::CFinalCommitment::LEGACY_BLS_INDEXED_QUORUM_VERSION
                                                    : llmq::CFinalCommitment::LEGACY_BLS_NON_INDEXED_QUORUM_VERSION;
            }
            entry.commitment.quorumPublicKey = key.GetPublicKey();
            entry.commitment.quorumSig = key.Sign(H(1), legacy);
            entry.commitment.membersSig = key.Sign(H(2), legacy);
            const auto encode = [&](int opposite_field) {
                auto& qc{entry.commitment};
                CDataStream bytes{SER_DISK, CLIENT_VERSION};
                bytes << entry.quorum_base_block_hash << entry.work_block_hash << qc.nVersion << qc.llmqType
                      << qc.quorumHash;
                if (indexed) bytes << qc.quorumIndex;
                bytes << DYNBITSET(qc.signers) << DYNBITSET(qc.validMembers)
                      << CBLSPublicKeyVersionWrapper(qc.quorumPublicKey, opposite_field == 0 ? !legacy : legacy)
                      << qc.quorumVvecHash
                      << CBLSSignatureVersionWrapper(qc.quorumSig, opposite_field == 1 ? !legacy : legacy)
                      << CBLSSignatureVersionWrapper(qc.membersSig, opposite_field == 2 ? !legacy : legacy)
                      << entry.mined_block_hash;
                return bytes;
            };
            auto canonical{encode(-1)};
            BOOST_CHECK_NO_THROW(evo::ReadMinedQuorumCommitment(canonical));
            BOOST_CHECK(canonical.empty());
            if (legacy) continue;
            for (int field{0}; field < 3; ++field) {
                auto bytes{encode(field)};
                auto ordinary_input{bytes};
                evo::MinedQuorumCommitment ordinary;
                BOOST_REQUIRE_NO_THROW(ordinary_input >> ordinary);
                CDataStream normalized{SER_DISK, CLIENT_VERSION};
                normalized << ordinary;
                BOOST_REQUIRE(bytes.str() != normalized.str());
                BOOST_CHECK_EXCEPTION(evo::ReadMinedQuorumCommitment(bytes), std::ios_base::failure, [](const auto& e) {
                    return std::string{e.what()}.find("noncanonical evo snapshot commitment encoding") != std::string::npos;
                });
            }
        }
    }
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
    evo::QuorumSnapshotData partial;
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
        evo::QuorumSnapshotData data;
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
    WriteCompactSize(rotations,
                     evo::EvoSnapshotMaxRotationSnapshots(evo::SnapshotLLMQParams(Consensus::LLMQType::LLMQ_TEST_DIP0024)) +
                         1);
    BOOST_CHECK_THROW(decode_quorum(rotations), std::ios_base::failure);
    // A type without rotation carries no quarters at all.
    CDataStream plain_rotations{SER_DISK, CLIENT_VERSION};
    plain_rotations << Consensus::LLMQType::LLMQ_TEST_DIP0024 << false;
    WriteCompactSize(plain_rotations, 0);
    WriteCompactSize(plain_rotations, 0);
    WriteCompactSize(plain_rotations, 1);
    BOOST_CHECK_THROW(decode_quorum(plain_rotations), std::ios_base::failure);
    CDataStream bootstrap{SER_DISK, CLIENT_VERSION};
    bootstrap << Consensus::LLMQType::LLMQ_TEST_DIP0024 << true;
    WriteCompactSize(bootstrap, 0);
    WriteCompactSize(bootstrap, 0);
    WriteCompactSize(bootstrap, 0);
    WriteCompactSize(bootstrap, evo::EVO_SNAPSHOT_MAX_BOOTSTRAP_WORK + 1);
    BOOST_CHECK_THROW(decode_quorum(bootstrap), std::ios_base::failure);
    CDataStream bootstrap_order{SER_DISK, CLIENT_VERSION};
    bootstrap_order << Consensus::LLMQType::LLMQ_TEST_DIP0024 << true;
    WriteCompactSize(bootstrap_order, 0);
    WriteCompactSize(bootstrap_order, 0);
    WriteCompactSize(bootstrap_order, 0);
    WriteCompactSize(bootstrap_order, 2);
    bootstrap_order << evo::BootstrapWork{48, H(1)} << evo::BootstrapWork{48, H(2)};
    BOOST_CHECK_EXCEPTION(decode_quorum(bootstrap_order), std::ios_base::failure, [](const auto& e) {
        return std::string{e.what()}.find("bootstrap work order") != std::string::npos;
    });

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
    evo::QuorumSnapshotData oversized_data;
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
    std::string decoded_string;
    BOOST_CHECK_EXCEPTION(bounded_string >> decoded_string, std::ios_base::failure,
                          [](const auto& e) { return std::string{e.what()}.find("CompactSize budget exceeded") != std::string::npos; });
    BOOST_CHECK(decoded_string.empty());
    BOOST_REQUIRE_EQUAL(wrapped_string.size(), 1U);
    BOOST_CHECK_EQUAL(std::to_integer<uint8_t>(wrapped_string.data()[0]), 0x42);

    const auto snapshot_prefix = [](CDataStream& stream) {
        stream << evo::EVO_SNAPSHOT_VERSION << H(42);
        evo::SerializeCanonicalMNList(stream, CDeterministicMNList{H(42), 1, 0});
        WriteCompactSize(stream, 0); // no base coinbase
        WriteCompactSize(stream, 0); // and no branch
    };
    CDataStream quorum_types{SER_DISK, CLIENT_VERSION};
    snapshot_prefix(quorum_types);
    WriteCompactSize(quorum_types, Consensus::available_llmqs.size() + 1);
    evo::EvoSnapshot decoded;
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
    WriteCompactSize(signals, 0);
    WriteCompactSize(signals, VERSIONBITS_NUM_BITS + 1);
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
    evo::EvoSnapshot decoded;
    BOOST_CHECK_NO_THROW(input >> decoded);
    BOOST_CHECK(input.empty());
    const auto reencoded{SerializeSnapshot(decoded)};
    BOOST_CHECK_EQUAL_COLLECTIONS(bytes.begin(), bytes.end(), reencoded.begin(), reencoded.end());
}

BOOST_FIXTURE_TEST_CASE(canonical_sort_preserves_input, BasicTestingSetup)
{
    const auto snapshot{SyntheticSnapshot(/*reverse_representation=*/true)};
    const auto original{SerializeSnapshot(snapshot)};
    const auto first_type{snapshot.quorums.front().llmq_type};
    const auto sorted{evo::CanonicallySortedCopy(snapshot.quorums)};
    BOOST_CHECK(evo::IsCanonicallySorted(sorted));
    BOOST_CHECK(!evo::IsCanonicallySorted(snapshot.quorums));
    BOOST_CHECK(snapshot.quorums.front().llmq_type == first_type);
    const auto after{SerializeSnapshot(snapshot)};
    BOOST_CHECK_EQUAL_COLLECTIONS(original.begin(), original.end(), after.begin(), after.end());
}

BOOST_FIXTURE_TEST_CASE(rotation_flag_wire_encoding_is_canonical, BasicTestingSetup)
{
    for (const uint8_t flag : {0, 1, 2, 255}) {
        CDataStream input{SER_DISK, CLIENT_VERSION};
        input << Consensus::LLMQType::LLMQ_TEST_DIP0024 << flag;
        for (int i{0}; i < 4; ++i)
            WriteCompactSize(input, 0);
        evo::QuorumSnapshotData decoded;
        if (flag <= 1) {
            BOOST_CHECK_NO_THROW(input >> decoded);
            BOOST_CHECK_EQUAL(decoded.rotation_enabled, flag != 0);
        } else {
            BOOST_CHECK_EXCEPTION(input >> decoded, std::ios_base::failure, [](const auto& e) {
                return std::string{e.what()}.find("noncanonical evo quorum rotation flag") != std::string::npos;
            });
        }
    }
}

BOOST_FIXTURE_TEST_CASE(nested_mn_map_wire_order_is_canonical, BasicTestingSetup)
{
    const auto mn{MN(5, 1, MnType::Evo, ProTxVersion::ExtAddr, 5)};
    auto net_info{mn->pdmnState->netInfo};
    CDataStream canonical_info{SER_DISK, CLIENT_VERSION};
    canonical_info << NetInfoSerWrapper(net_info, /*is_extended=*/true);
    auto info_input{canonical_info};
    uint8_t version;
    info_input >> version;
    const auto count{ReadCompactSize(info_input)};
    std::vector<std::pair<NetInfoPurpose, NetInfoList>> entries(count);
    for (auto& entry : entries) info_input >> entry;
    BOOST_REQUIRE(entries.size() > 1);
    BOOST_REQUIRE(info_input.empty());

    const auto is_noncanonical = [](const auto& e) {
        return std::string{e.what()}.find("ExtNetInfo:") != std::string::npos;
    };
    for (const bool duplicate : {false, true}) {
        auto changed_entries{entries};
        if (duplicate) {
            changed_entries.insert(changed_entries.begin(), entries.front());
        } else {
            std::reverse(changed_entries.begin(), changed_entries.end());
        }
        CDataStream changed_info{SER_DISK, CLIENT_VERSION};
        changed_info << version << changed_entries;
        const auto replace_info = [&](const auto& obj) {
            CDataStream encoded{SER_DISK, CLIENT_VERSION};
            encoded << obj;
            const auto pos{std::search(encoded.begin(), encoded.end(), canonical_info.begin(), canonical_info.end())};
            BOOST_REQUIRE(pos != encoded.end());
            const size_t offset{static_cast<size_t>(pos - encoded.begin())};
            CDataStream changed{SER_DISK, CLIENT_VERSION};
            changed.write(Span{encoded}.first(offset));
            changed.write(Span{changed_info});
            changed.write(Span{encoded}.subspan(offset + canonical_info.size()));
            return changed;
        };
        const auto changed_mn{replace_info(*mn)};
        auto ordinary_input{changed_mn};
        BOOST_CHECK_THROW(CDeterministicMN(deserialize, ordinary_input), std::ios_base::failure);

        CDataStream base{SER_DISK, CLIENT_VERSION};
        base << H(42) << 42 << uint32_t{10};
        WriteCompactSize(base, 1);
        base.write(Span{changed_mn});
        BOOST_CHECK_EXCEPTION(evo::UnserializeCanonicalMNList(base), std::ios_base::failure, is_noncanonical);

        CDataStream addition{SER_DISK, CLIENT_VERSION};
        WriteCompactSize(addition, 1);
        addition.write(Span{changed_mn});
        WriteCompactSize(addition, 0);
        WriteCompactSize(addition, 0);
        BOOST_CHECK_EXCEPTION(evo::UnserializeCanonicalMNListDiff(addition), std::ios_base::failure, is_noncanonical);

        const auto old_mn{MN(5, 1, MnType::Evo, ProTxVersion::ExtAddr, 6)};
        const CDeterministicMNStateDiff state_diff{*old_mn->pdmnState, *mn->pdmnState};
        const auto changed_diff{replace_info(state_diff)};
        CDataStream update{SER_DISK, CLIENT_VERSION};
        WriteCompactSize(update, 0);
        WriteCompactSize(update, 1);
        WriteVarInt<CDataStream, VarIntMode::DEFAULT, uint64_t>(update, 5);
        update.write(Span{changed_diff});
        WriteCompactSize(update, 0);
        BOOST_CHECK_EXCEPTION(evo::UnserializeCanonicalMNListDiff(update), std::ios_base::failure, is_noncanonical);
    }
}

BOOST_FIXTURE_TEST_CASE(validation_matches_height_and_range_decode_bounds, BasicTestingSetup)
{
    evo::EvoSnapshot snapshot;
    snapshot.base_block_hash = H(42);
    snapshot.mn_list.SetBlockHash(snapshot.base_block_hash);
    BOOST_CHECK_EXCEPTION(snapshot.Validate(), std::ios_base::failure, [](const auto& e) {
        return std::string{e.what()}.find("negative canonical MN-list height") != std::string::npos;
    });
    BOOST_CHECK_THROW(GetEvoSnapshotHash(snapshot), std::ios_base::failure);
    snapshot.mn_list = CDeterministicMNList{snapshot.base_block_hash, 0, 0};
    for (size_t i{0}; i < evo::EVO_SNAPSHOT_MAX_RANGES; ++i) {
        BOOST_REQUIRE(snapshot.credit_pool.indexes.Add(2 * i));
    }
    BOOST_CHECK_EQUAL(snapshot.credit_pool.indexes.RangeCount(), evo::EVO_SNAPSHOT_MAX_RANGES);
    BOOST_CHECK_NO_THROW(snapshot.Validate());
    auto bytes{SerializeSnapshot(snapshot)};
    evo::EvoSnapshot decoded;
    BOOST_CHECK_NO_THROW(bytes >> decoded);
    BOOST_REQUIRE(snapshot.credit_pool.indexes.Add(2 * evo::EVO_SNAPSHOT_MAX_RANGES));
    BOOST_CHECK_EXCEPTION(snapshot.Validate(), std::ios_base::failure, [](const auto& e) {
        return std::string{e.what()}.find("oversized CRangesSet range count") != std::string::npos;
    });
    BOOST_CHECK_THROW(GetEvoSnapshotHash(snapshot), std::ios_base::failure);

    CDataStream continuous{SER_DISK, CLIENT_VERSION};
    WriteCompactSize(continuous, 1);
    continuous << uint64_t{0} << uint64_t{evo::EVO_SNAPSHOT_MAX_RANGES + 1};
    continuous >> snapshot.credit_pool.indexes;
    BOOST_CHECK_EQUAL(snapshot.credit_pool.indexes.RangeCount(), 1U);
    BOOST_CHECK_GT(snapshot.credit_pool.indexes.Size(), evo::EVO_SNAPSHOT_MAX_RANGES);
    BOOST_CHECK_NO_THROW(snapshot.Validate());
}

BOOST_FIXTURE_TEST_CASE(validation_enforces_nested_mn_decode_budget, BasicTestingSetup)
{
    const auto is_budget_error = [](const auto& e) {
        return std::string{e.what()}.find("CompactSize budget exceeded") != std::string::npos;
    };
    auto mn{std::const_pointer_cast<CDeterministicMN>(MN(2, 3, MnType::Regular, ProTxVersion::LegacyBLS, 3))};
    auto state{std::make_shared<CDeterministicMNState>(*mn->pdmnState)};
    // Legacy net info has no CompactSizes; the two scripts share the budget.
    state->scriptPayout.assign(evo::EVO_SNAPSHOT_MAX_MN_COMPACT_ITEMS / 2, OP_TRUE);
    state->scriptOperatorPayout = state->scriptPayout;
    mn->pdmnState = state;
    evo::EvoSnapshot snapshot;
    snapshot.base_block_hash = H(42);
    snapshot.mn_list = CDeterministicMNList{snapshot.base_block_hash, 42, 10};
    snapshot.base_coinbase = SyntheticCoinbase(42);
    snapshot.mn_list.AddMN(mn, /*fBumpTotalCount=*/false);
    BOOST_CHECK_NO_THROW(snapshot.Validate());
    auto bytes{SerializeSnapshot(snapshot)};
    evo::EvoSnapshot decoded;
    BOOST_CHECK_NO_THROW(bytes >> decoded);

    state = std::make_shared<CDeterministicMNState>(*state);
    state->scriptOperatorPayout.push_back(OP_TRUE);
    snapshot.mn_list.UpdateMN(mn->proTxHash, state);
    BOOST_CHECK_EXCEPTION(snapshot.Validate(), std::ios_base::failure, is_budget_error);
    BOOST_CHECK_THROW(GetEvoSnapshotHash(snapshot), std::ios_base::failure);
    bytes = SerializeSnapshot(snapshot);
    BOOST_CHECK_EXCEPTION(bytes >> decoded, std::ios_base::failure, is_budget_error);

    snapshot = SyntheticSnapshot();
    mn->pdmnState = state;
    snapshot.historical_mn_list_diffs.front().diff.addedMNs.push_back(mn);
    BOOST_CHECK_EXCEPTION(snapshot.Validate(), std::ios_base::failure, is_budget_error);

    // A diff can carry a legacy payout field even when the resulting extended
    // MN's full encoding omits that field. Check the diff's own budget as well.
    snapshot = SyntheticSnapshot();
    const auto extended{MN(5, 1, MnType::Evo, ProTxVersion::ExtAddr, 5)};
    CDeterministicMNState changed{*extended->pdmnState};
    changed.scriptPayout.assign(evo::EVO_SNAPSHOT_MAX_MN_COMPACT_ITEMS + 1, OP_TRUE);
    snapshot.historical_mn_list_diffs.front().diff.updatedMNs.emplace(
        5, CDeterministicMNStateDiff{*extended->pdmnState, changed});
    BOOST_CHECK_EXCEPTION(snapshot.Validate(), std::ios_base::failure, is_budget_error);

    // Both the base MN and diff fit separately, but the update combines two
    // scripts whose total exceeds the full-MN budget.
    snapshot = SyntheticSnapshot();
    const auto base_mn{snapshot.mn_list.GetMNByInternalId(2)};
    auto base_state{std::make_shared<CDeterministicMNState>(*base_mn->pdmnState)};
    base_state->scriptPayout.assign(evo::EVO_SNAPSHOT_MAX_MN_COMPACT_ITEMS / 2, OP_TRUE);
    snapshot.mn_list.UpdateMN(base_mn->proTxHash, base_state);
    CDeterministicMNState combined{*base_state};
    combined.scriptOperatorPayout.assign(evo::EVO_SNAPSHOT_MAX_MN_COMPACT_ITEMS / 2 + 1, OP_TRUE);
    snapshot.historical_mn_list_diffs.front().diff.updatedMNs.emplace(
        2, CDeterministicMNStateDiff{*base_state, combined});
    BOOST_CHECK_EXCEPTION(snapshot.Validate(), std::ios_base::failure, is_budget_error);
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
        evo::EvoSnapshot decoded;
        BOOST_CHECK_EXCEPTION(stream >> decoded, std::ios_base::failure, [](const auto& e) {
            return std::string{e.what()}.find("noncanonical MNHF signal order") != std::string::npos;
        });
    };
    auto canonical{with_signals({{2, 12}, {9, 30}})};
    evo::EvoSnapshot decoded;
    BOOST_CHECK_NO_THROW(canonical >> decoded);
    BOOST_CHECK(canonical.empty());
    BOOST_CHECK_EQUAL(decoded.mnhf_signals.size(), 2U);
    expect_noncanonical(with_signals({{9, 30}, {2, 12}}));
    expect_noncanonical(with_signals({{2, 12}, {2, 30}}));
}

BOOST_FIXTURE_TEST_CASE(mnhf_signals_outnumbering_deployments_are_valid, BasicTestingSetup)
{
    // CMNHFManager drops a signal only when a current deployment reuses its
    // bit, so signals of buried EHF forks (bits 10, 11 and 12) stay in the
    // map alongside V24's bit 13.
    auto snapshot{SyntheticSnapshot()};
    snapshot.mnhf_signals = {{10, 100}, {11, 200}, {12, 300}, {13, 400}};
    BOOST_REQUIRE_GT(snapshot.mnhf_signals.size(), size_t{Consensus::MAX_VERSION_BITS_DEPLOYMENTS});

    BOOST_CHECK_NO_THROW(snapshot.Validate());
    BOOST_CHECK_NO_THROW(GetEvoSnapshotHash(snapshot));

    CDataStream stream{SerializeSnapshot(snapshot)};
    evo::EvoSnapshot decoded;
    BOOST_CHECK_NO_THROW(stream >> decoded);
    BOOST_CHECK(decoded.mnhf_signals == snapshot.mnhf_signals);
}

BOOST_FIXTURE_TEST_CASE(unserialize_replaces_previous_contents, BasicTestingSetup)
{
    const auto populated_bytes{SerializeSnapshot(SyntheticSnapshot())};
    evo::EvoSnapshot minimal;
    minimal.base_block_hash = H(42);
    minimal.mn_list = CDeterministicMNList{H(42), 500, 0};
    const auto minimal_bytes{SerializeSnapshot(minimal)};

    evo::EvoSnapshot decoded;
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
    evo::EvoSnapshot snapshot;
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
    evo::QuorumSnapshotData data;
    data.llmq_type = Consensus::LLMQType::LLMQ_TEST;
    data.active_commitments = {Commitment(data.llmq_type, 11, 51, false)};
    CDataStream encoded{SER_DISK, CLIENT_VERSION};
    encoded << data;

    // Every vector the decoder clears starts non-empty and the payload leaves
    // two of them empty, so dropping any single clear() leaves stale entries.
    evo::QuorumSnapshotData reused;
    reused.llmq_type = Consensus::LLMQType::LLMQ_TEST;
    reused.active_commitments = {Commitment(reused.llmq_type, 21, 61, false)};
    reused.safety_commitments = {Commitment(reused.llmq_type, 22, 62, false)};
    reused.rotation_snapshots = {
        {H(23), H(63), llmq::CQuorumSnapshot{{true, false}, SnapshotSkipMode::MODE_NO_SKIPPING, {}}}};

    encoded >> reused;
    BOOST_CHECK_EQUAL(reused.active_commitments.size(), 1U);
    BOOST_CHECK(reused.active_commitments.at(0).quorum_base_block_hash ==
                data.active_commitments.at(0).quorum_base_block_hash);
    BOOST_CHECK_EQUAL(reused.safety_commitments.size(), 0U);
    BOOST_CHECK_EQUAL(reused.rotation_snapshots.size(), 0U);
}

BOOST_FIXTURE_TEST_CASE(validation_enforces_the_decode_operation_budget, BasicTestingSetup)
{
    auto snapshot{SyntheticSnapshot()};
    const size_t budget{evo::EvoSnapshotMaxHistoricalMNOperations()};
    size_t operations{evo::EvoSnapshotHistoricalMNOperations(snapshot.historical_mn_list_diffs)};
    BOOST_REQUIRE(operations < budget);
    BOOST_CHECK_NO_THROW(snapshot.Validate());

    // Removals are the cheapest chargeable operation and are bounded per diff
    // by EVO_SNAPSHOT_MAX_MNS, so spread them over entries until the
    // cumulative ceiling is passed by exactly one. Ids start above anything
    // BuildDiff() produced so every insertion counts.
    // The budget check precedes the chain checks, so placeholder entries can
    // carry the overflow once the real ones are full.
    uint64_t next_id{1'000'000};
    for (size_t i{0}; operations <= budget; ++i) {
        if (i == snapshot.historical_mn_list_diffs.size()) snapshot.historical_mn_list_diffs.emplace_back();
        auto& entry{snapshot.historical_mn_list_diffs[i]};
        while (operations <= budget && entry.diff.removedMns.size() < evo::EVO_SNAPSHOT_MAX_MNS) {
            entry.diff.removedMns.emplace(next_id++);
            ++operations;
        }
    }
    BOOST_REQUIRE(snapshot.historical_mn_list_diffs.size() <= evo::EvoSnapshotMaxHistoricalMNLists());
    BOOST_REQUIRE_EQUAL(operations, budget + 1);
    BOOST_CHECK_EXCEPTION(snapshot.Validate(), std::ios_base::failure, [](const auto& e) {
        return std::string{e.what()}.find("operation budget") != std::string::npos;
    });
}

BOOST_FIXTURE_TEST_CASE(historical_chain_rejects_revisited_blocks_and_counter_growth, BasicTestingSetup)
{
    const auto broken_chain = [](const evo::EvoSnapshot& snapshot) {
        std::map<uint256, CDeterministicMNList> lists;
        std::string error;
        BOOST_CHECK(!evo::ReconstructHistoricalMNLists(snapshot, lists, error));
        BOOST_CHECK_MESSAGE(error.find("broken historical MN-list diff chain") != std::string::npos, error);
    };

    // A transition must leave its predecessor: neither the first entry nor a
    // later one may target the base block, and no target may repeat.
    auto snapshot{SyntheticSnapshot()};
    auto history{evo::CanonicallySortedCopy(snapshot.historical_mn_list_diffs)};
    BOOST_REQUIRE(history.size() >= 3);
    snapshot.historical_mn_list_diffs = history;
    snapshot.historical_mn_list_diffs[0].block_hash = snapshot.base_block_hash;
    broken_chain(snapshot);
    snapshot.historical_mn_list_diffs = history;
    snapshot.historical_mn_list_diffs[1].block_hash = snapshot.base_block_hash;
    snapshot.historical_mn_list_diffs[2].previous_block_hash = snapshot.base_block_hash;
    broken_chain(snapshot);
    snapshot.historical_mn_list_diffs = history;
    snapshot.historical_mn_list_diffs[2].block_hash = history[0].block_hash;
    broken_chain(snapshot);

    // Registrations only ever raise the counter, so replaying backwards it can
    // never grow. A forged entry that also recomputes its canonical hash would
    // otherwise pass every remaining check.
    snapshot.historical_mn_list_diffs = history;
    auto& entry{snapshot.historical_mn_list_diffs[0]};
    std::map<uint256, CDeterministicMNList> lists;
    std::string error;
    BOOST_REQUIRE_MESSAGE(evo::ReconstructHistoricalMNLists(snapshot, lists, error), error);
    const auto& reconstructed{lists.at(entry.block_hash)};
    ++entry.total_registered_count;
    CDeterministicMNList inflated{entry.block_hash, entry.height, entry.total_registered_count};
    reconstructed.ForEachMNShared(/*onlyValid=*/false,
                                  [&](const auto& dmn) { inflated.AddMN(dmn, /*fBumpTotalCount=*/false); });
    entry.canonical_list_hash = evo::CanonicalMNListHash(inflated);
    broken_chain(snapshot);
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
    BOOST_REQUIRE_EQUAL(std::to_integer<uint8_t>(bytes.data()[0]), 4);
    // v3 is rejected outright: no network ever pinned a v3 Evo section.
    bytes.data()[0] = std::byte{3};
    evo::EvoSnapshot decoded;
    BOOST_CHECK_THROW(bytes >> decoded, std::ios_base::failure);
}

//! Recompute the historical lists and modifiers a context-free valid snapshot
//! needs from what its commitments, quarters and bootstrap entries reference,
//! so that a tampered set reaches the chain validator's planner comparison.
void RewireDependencies(EvoSnapshotChainSetup& setup, evo::EvoSnapshot& snapshot)
{
    std::map<std::pair<Consensus::LLMQType, uint256>, uint256> old_modifiers;
    for (const auto& entry : snapshot.quorum_modifiers) {
        old_modifiers.emplace(std::make_pair(entry.llmq_type, entry.work_block_hash), entry.modifier);
    }
    std::set<std::pair<Consensus::LLMQType, uint256>> keys;
    std::set<uint256> work_hashes;
    for (const auto& data : snapshot.quorums) {
        const auto add = [&](const uint256& hash) {
            keys.emplace(data.llmq_type, hash);
            if (hash != snapshot.base_block_hash) work_hashes.insert(hash);
        };
        for (const auto* commitments : {&data.active_commitments, &data.safety_commitments}) {
            for (const auto& entry : *commitments)
                add(entry.work_block_hash);
        }
        for (const auto& entry : data.rotation_snapshots)
            add(entry.work_block_hash);
        for (const auto& entry : data.bootstrap_work)
            add(entry.work_block_hash);
    }
    snapshot.quorum_modifiers.clear();
    for (const auto& key : keys) {
        const auto old{old_modifiers.find(key)};
        snapshot.quorum_modifiers.push_back(
            {key.first, key.second, old != old_modifiers.end() ? old->second : SerializeHash(key)});
    }
    std::vector<const CBlockIndex*> work;
    {
        LOCK(::cs_main);
        for (const auto& hash : work_hashes)
            work.push_back(setup.m_node.chainman->m_blockman.LookupBlockIndex(hash));
    }
    std::sort(work.begin(), work.end(), [](const auto* a, const auto* b) { return a->nHeight > b->nHeight; });
    snapshot.historical_mn_list_diffs.clear();
    CDeterministicMNList previous{snapshot.mn_list};
    uint256 previous_hash{snapshot.base_block_hash};
    const EvoViewScope normal{setup.Normal()};
    for (const CBlockIndex* index : work) {
        const auto list{setup.m_node.dmnman->GetListForBlock(index)};
        snapshot.historical_mn_list_diffs.push_back({previous_hash, index->GetBlockHash(), index->nHeight,
                                                     list.GetTotalRegisteredCount(), evo::CanonicalMNListHash(list),
                                                     previous.BuildDiff(list)});
        previous_hash = index->GetBlockHash();
        previous = list;
    }
    snapshot.quorums = evo::CanonicallySortedCopy(std::move(snapshot.quorums));
    for (auto& data : snapshot.quorums) {
        data.rotation_snapshots = evo::CanonicallySortedCopy(std::move(data.rotation_snapshots));
        data.bootstrap_work = evo::CanonicallySortedCopy(std::move(data.bootstrap_work));
    }
    snapshot.quorum_modifiers = evo::CanonicallySortedCopy(std::move(snapshot.quorum_modifiers));
    BOOST_REQUIRE_NO_THROW(snapshot.Validate(/*require_canonical_order=*/true));
}

BOOST_FIXTURE_TEST_CASE(planner_carries_every_quarter_a_carried_safety_quorum_reads, EvoSnapshotChainSetup)
{
    // h = 156 and B = h + 1, before h's mining window: the active cycle is
    // h - C and the safety cycle h - 2C, whose members read h - 3C .. h - 5C.
    SyncNormal();
    MineCycles(12, 180);
    const CBlockIndex* base{At(157)};
    const auto snapshot{Build(base)};
    std::string error;
    BOOST_CHECK_MESSAGE(Valid(snapshot, base, error), error);
    BOOST_CHECK(RotationHeights(snapshot) == (std::set<int>{96, 108, 120, 132, 144, 156}));
    // A_T = {132, 144, 156, 168}: each quarter's work block, and each cycle's.
    // N's work block 160 is above the base.
    BOOST_CHECK(ModifierHeights(snapshot, m_rotated.type) == (std::set<int>{88, 100, 112, 124, 136, 148}));
    BOOST_CHECK(Type(snapshot, m_rotated.type).bootstrap_work.empty());
    // Non-rotating: the three retained quorums 144, 132, 120 plus h; N's work
    // block is above the base.
    BOOST_CHECK(ModifierHeights(snapshot, m_plain.type) == (std::set<int>{112, 124, 136, 148}));
    BOOST_REQUIRE_EQUAL(Type(snapshot, m_plain.type).bootstrap_work.size(), 1U);
    BOOST_CHECK_EQUAL(Type(snapshot, m_plain.type).bootstrap_work[0].cycle_base_height, 156);

    const EvoViewToken seeded{Seed(snapshot, base)};
    for (const int quorum : {132, 133, 144, 145, 156, 157}) {
        CheckMembers(seeded, m_rotated, quorum);
    }
    for (const int quorum : {120, 132, 144, 156}) {
        CheckMembers(seeded, m_plain, quorum);
    }

    // Lists outside the carried set are unavailable in the snapshot view, not
    // reconstructed from (absent) diffs or guessed empty.
    size_t misses{0};
    m_node.dmnman->SetListSnapshotMissHookForTesting([&](const CBlockIndex*) { ++misses; });
    {
        const EvoViewScope scope{seeded};
        BOOST_CHECK_THROW(m_node.dmnman->GetListForBlock(At(150)), BlockDataUnavailableError);
    }
    m_node.dmnman->SetListSnapshotMissHookForTesting(nullptr);
    BOOST_CHECK_EQUAL(misses, 1U);
}

BOOST_FIXTURE_TEST_CASE(planner_carries_the_quarter_of_the_base_cycle, EvoSnapshotChainSetup)
{
    // B = h + 7, after h = 156 is fully mined: the next cycle N = 168 reads
    // the quarter of h itself.
    SyncNormal();
    MineCycles(12, 180);
    const CBlockIndex* base{At(163)};
    const auto snapshot{Build(base)};
    std::string error;
    BOOST_CHECK_MESSAGE(Valid(snapshot, base, error), error);
    BOOST_CHECK(RotationHeights(snapshot).contains(156));

    const EvoViewToken seeded{Seed(snapshot, base)};
    {
        const EvoViewScope scope{seeded};
        const auto carried{m_node.llmq_ctx->qsnapman->GetSnapshotForBlock(m_rotated.type, At(156))};
        BOOST_REQUIRE(carried.has_value());
        const EvoViewScope normal{Normal()};
        const auto synced{m_node.llmq_ctx->qsnapman->GetSnapshotForBlock(m_rotated.type, At(156))};
        BOOST_REQUIRE(synced.has_value());
        BOOST_CHECK(SerializeHash(*carried) == SerializeHash(*synced));
    }
    for (const int quorum : {168, 169}) {
        CheckMembers(seeded, m_rotated, quorum);
    }
}

BOOST_FIXTURE_TEST_CASE(planner_follows_an_isolated_failed_dkg_index, EvoSnapshotChainSetup)
{
    // Index 1's DKG has failed since cycle 60, so at B = 190 (h = 180) its
    // carried commitments sit at h - 10C and h - 11C. Their members read the
    // quarters of h - 11C .. h - 13C, not the quarter of h - 10C.
    SyncNormal();
    MineCycles(12, 60);
    MineCycles(72, 180, /*rotated_index_one=*/false);
    const CBlockIndex* base{At(190)};
    auto snapshot{Build(base)};
    std::string error;
    BOOST_CHECK_MESSAGE(Valid(snapshot, base, error), error);
    const auto quarters{RotationHeights(snapshot)};
    BOOST_CHECK(quarters == (std::set<int>{12, 24, 36, 48, 132, 144, 156, 168, 180}));
    BOOST_CHECK(!quarters.contains(60));

    const EvoViewToken seeded{Seed(snapshot, base)};
    for (const int quorum : {49, 61, 168, 180}) {
        CheckMembers(seeded, m_rotated, quorum);
    }
    {
        // The quarter of h - 10C was a reconstruction output: neither stored
        // nor reported unavailable.
        const EvoViewScope scope{seeded};
        BOOST_CHECK(!m_node.llmq_ctx->qsnapman->GetSnapshotForBlock(m_rotated.type, At(60)).has_value());
    }

    auto& rotated{const_cast<evo::QuorumSnapshotData&>(Type(snapshot, m_rotated.type))};
    std::erase_if(rotated.rotation_snapshots,
                  [&](const auto& entry) { return entry.cycle_base_block_hash == At(24)->GetBlockHash(); });
    const EvoViewToken incomplete{Seed(snapshot, base)};
    {
        const EvoViewScope scope{incomplete};
        BOOST_CHECK_THROW(llmq::utils::GetAllQuorumMembers(m_rotated.type, Util(At(61)), /*reset_cache=*/true),
                          BlockDataUnavailableError);
    }
}

BOOST_FIXTURE_TEST_CASE(planner_bootstraps_the_next_cycle_from_the_base_cycle, EvoSnapshotChainSetup)
{
    // B = N - 1 = 179 (h = 168): N's work block 172 is at or below the base,
    // and nothing carried references it.
    SyncNormal();
    MineCycles(12, 180);
    const CBlockIndex* base{At(179)};
    const auto snapshot{Build(base)};
    std::string error;
    BOOST_CHECK_MESSAGE(Valid(snapshot, base, error), error);
    for (const auto* params : {&m_rotated, &m_plain}) {
        const auto& bootstrap{Type(snapshot, params->type).bootstrap_work};
        BOOST_REQUIRE_EQUAL(bootstrap.size(), 1U);
        BOOST_CHECK_EQUAL(bootstrap[0].cycle_base_height, 180);
        BOOST_CHECK(bootstrap[0].work_block_hash == At(172)->GetBlockHash());
    }

    const EvoViewToken seeded{Seed(snapshot, base)};
    for (const int quorum : {168, 169, 180, 181}) {
        CheckMembers(seeded, m_rotated, quorum);
        const auto predicted{Predicted(seeded, m_rotated, quorum)};
        BOOST_REQUIRE(predicted.has_value());
        BOOST_CHECK(*predicted == Members(Normal(), m_rotated, quorum));
    }
    for (const int quorum : {168, 180}) {
        CheckMembers(seeded, m_plain, quorum);
        const auto predicted{Predicted(seeded, m_plain, quorum)};
        BOOST_REQUIRE(predicted.has_value());
        BOOST_CHECK(*predicted == Members(Normal(), m_plain, quorum));
    }
}

BOOST_FIXTURE_TEST_CASE(planner_defers_a_next_cycle_whose_work_block_is_above_the_base, EvoSnapshotChainSetup)
{
    // With 12-block cycles N's work block is h + 4, so B = h + 3 = 171 places
    // it above the base (the 24-block regtest equivalent is B = h + 5).
    SyncNormal();
    MineCycles(12, 180);
    const CBlockIndex* base{At(171)};
    const auto snapshot{Build(base)};
    std::string error;
    BOOST_CHECK_MESSAGE(Valid(snapshot, base, error), error);
    const auto work{ModifierHeights(snapshot, m_rotated.type)};
    BOOST_CHECK(work.contains(160));
    BOOST_CHECK(!work.contains(172));
    const auto quarters{RotationHeights(snapshot)};
    for (const int quarter : {168, 156, 144})
        BOOST_CHECK(quarters.contains(quarter));
    BOOST_CHECK(Type(snapshot, m_rotated.type).bootstrap_work.empty());
    BOOST_CHECK(!ModifierHeights(snapshot, m_plain.type).contains(172));
}

BOOST_FIXTURE_TEST_CASE(chain_validation_requires_the_exact_planned_sets, EvoSnapshotChainSetup)
{
    SyncNormal();
    MineCycles(12, 180);
    const CBlockIndex* base{At(179)};
    const auto snapshot{Build(base)};
    std::string error;
    BOOST_REQUIRE_MESSAGE(Valid(snapshot, base, error), error);

    const auto rotated_of = [&](evo::EvoSnapshot& s) -> evo::QuorumSnapshotData& {
        return const_cast<evo::QuorumSnapshotData&>(Type(s, m_rotated.type));
    };
    const auto rejected = [&](evo::EvoSnapshot tampered, const std::string& reason) {
        RewireDependencies(*this, tampered);
        BOOST_CHECK(!Valid(tampered, base, error));
        BOOST_CHECK_MESSAGE(error.find(reason) != std::string::npos, error);
    };

    auto dropped_quarter{snapshot};
    rotated_of(dropped_quarter).rotation_snapshots.pop_back();
    rejected(dropped_quarter, "rotation cycles differ");

    auto added_quarter{snapshot};
    {
        const auto extra{
            WITH_LOCK(::cs_main, return m_node.llmq_ctx->qsnapman->GetSnapshotForBlock(m_rotated.type, At(84)))};
        BOOST_REQUIRE(extra.has_value());
        BOOST_REQUIRE(!RotationHeights(snapshot).contains(84));
        rotated_of(added_quarter).rotation_snapshots.push_back({At(84)->GetBlockHash(), At(76)->GetBlockHash(), *extra});
    }
    rejected(added_quarter, "rotation cycles differ");

    auto dropped_bootstrap{snapshot};
    rotated_of(dropped_bootstrap).bootstrap_work.clear();
    rejected(dropped_bootstrap, "bootstrap work differs");

    auto added_bootstrap{snapshot};
    rotated_of(added_bootstrap).bootstrap_work.push_back({168, At(160)->GetBlockHash()});
    rejected(added_bootstrap, "bootstrap work differs");

    auto moved_bootstrap{snapshot};
    rotated_of(moved_bootstrap).bootstrap_work[0].work_block_hash = At(171)->GetBlockHash();
    rejected(moved_bootstrap, "bootstrap work differs");

    // Modifiers and historical lists are pinned by the context-free layer to
    // what the sets reference, and by the planner to the planned work blocks.
    auto dropped_modifier{snapshot};
    dropped_modifier.quorum_modifiers.pop_back();
    BOOST_CHECK(!Valid(dropped_modifier, base, error));
    auto extra_modifier{snapshot};
    extra_modifier.quorum_modifiers.push_back({m_plain.type, At(150)->GetBlockHash(), H(9)});
    extra_modifier.quorum_modifiers = evo::CanonicallySortedCopy(std::move(extra_modifier.quorum_modifiers));
    BOOST_CHECK(!Valid(extra_modifier, base, error));
    auto wrong_modifier{snapshot};
    wrong_modifier.quorum_modifiers[0].modifier = H(9);
    BOOST_CHECK(!Valid(wrong_modifier, base, error));
    BOOST_CHECK_MESSAGE(error.find("modifier mismatch") != std::string::npos, error);
}

BOOST_FIXTURE_TEST_CASE(a_cycle_without_a_quarter_makes_the_base_unsupported, EvoSnapshotChainSetup)
{
    // The work block of cycle 108 has no masternodes, so the ordinary
    // computation stores no quarter for it. B = 157 needs that quarter.
    SyncNormal(/*empty_heights=*/{100});
    MineCycles(12, 180);
    const CBlockIndex* base{At(157)};
    evo::EvoSnapshot snapshot;
    std::string error;
    BOOST_CHECK(!TryBuild(base, snapshot, error));
    BOOST_CHECK_MESSAGE(error.find("rotation quarter for applicable cycle 108") != std::string::npos, error);

    // A candidate cannot help: without the quarter the planned set is
    // incomplete, and with an invented one the cycle cannot have one.
    {
        LOCK(::cs_main);
        const EvoViewScope normal{Normal()};
        m_node.llmq_ctx->qsnapman->StoreSnapshotForBlock(m_rotated.type, At(108),
                                                         llmq::CQuorumSnapshot{{}, SnapshotSkipMode::MODE_NO_SKIPPING, {}});
    }
    const auto invented{Build(base)};
    BOOST_CHECK(!Valid(invented, base, error));
    BOOST_CHECK_MESSAGE(error.find("leaves without one") != std::string::npos, error);
    auto missing{invented};
    auto& rotated{const_cast<evo::QuorumSnapshotData&>(Type(missing, m_rotated.type))};
    std::erase_if(rotated.rotation_snapshots,
                  [&](const auto& entry) { return entry.cycle_base_block_hash == At(108)->GetBlockHash(); });
    RewireDependencies(*this, missing);
    BOOST_CHECK(!Valid(missing, base, error));
    BOOST_CHECK_MESSAGE(error.find("rotation cycles differ") != std::string::npos, error);
}

BOOST_FIXTURE_TEST_CASE(a_type_enabled_before_the_next_cycle_makes_the_base_unsupported, EvoSnapshotChainSetup)
{
    // DIP0024 enables the rotating type at N = 180. At B = 175 that type is
    // disabled, so nothing of it is carried, yet N's members read its work
    // block 172 (at or below B) and its exact modifier there.
    SyncNormal();
    for (int cycle{12}; cycle <= 168; cycle += CYCLE_LENGTH)
        Mine(m_plain, cycle);
    const CBlockIndex* base{At(175)};
    m_restorer.Get().DIP0024Height = 1000;
    const auto snapshot{Build(base)};
    std::string error;
    BOOST_REQUIRE_MESSAGE(Valid(snapshot, base, error), error);

    m_restorer.Get().DIP0024Height = 180;
    BOOST_CHECK(!Valid(snapshot, base, error));
    BOOST_CHECK_MESSAGE(error.find("undecidable activation context") != std::string::npos, error);
    evo::EvoSnapshot refused;
    BOOST_CHECK(!TryBuild(base, refused, error));
    BOOST_CHECK_MESSAGE(error.find("undecidable activation context") != std::string::npos, error);

    // With N's work block above the base, or N - 1 the base itself, N's
    // context is decidable and the base is supported.
    for (const int height : {171, 179}) {
        const CBlockIndex* supported{At(height)};
        const auto supported_snapshot{Build(supported)};
        BOOST_CHECK_MESSAGE(Valid(supported_snapshot, supported, error), error);
    }
}

BOOST_FIXTURE_TEST_CASE(builder_never_exports_a_fallback_modifier, EvoSnapshotChainSetup)
{
    // A v20 modifier commits to the work block's coinbase ChainLock. Without
    // the work block's data the builder exports a seeded exact modifier or
    // fails, never the ordinary block-hash fallback. Work block 40 serves the
    // carried quarter of cycle 48 and lies below the credit-pool window.
    SyncNormal();
    MineCycles(12, 60);
    MineCycles(72, 180, /*rotated_index_one=*/false);
    const CBlockIndex* base{At(190)};
    BOOST_REQUIRE(RotationHeights(Build(base)).contains(48));
    auto* work{const_cast<CBlockIndex*>(At(40))};
    const uint32_t saved_status{WITH_LOCK(::cs_main, return work->nStatus)};
    WITH_LOCK(::cs_main, work->nStatus &= ~BLOCK_HAVE_DATA);
    evo::EvoSnapshot snapshot;
    std::string error;
    BOOST_CHECK(!TryBuild(base, snapshot, error));
    BOOST_CHECK_MESSAGE(error.find("exact score modifier") != std::string::npos, error);

    const uint256 seeded_modifier{H(77)};
    {
        LOCK(::cs_main);
        auto tx{m_node.evodb->BeginTransaction(EvoDbIdentity::NORMAL)};
        for (const auto* params : {&m_plain, &m_rotated}) {
            BOOST_REQUIRE(m_node.llmq_ctx->qsnapman->SeedQuorumModifier(Normal(), params->type, work->GetBlockHash(),
                                                                        seeded_modifier));
        }
        tx->Commit();
        BOOST_REQUIRE(m_node.evodb->CommitRootTransaction(EvoDbIdentity::NORMAL));
    }
    const bool built{TryBuild(base, snapshot, error)};
    WITH_LOCK(::cs_main, work->nStatus = saved_status);
    BOOST_REQUIRE_MESSAGE(built, error);
    for (const auto& entry : snapshot.quorum_modifiers) {
        if (entry.work_block_hash == work->GetBlockHash()) BOOST_CHECK(entry.modifier == seeded_modifier);
    }
    // With the block readable again, the chain validator recomputes it.
    BOOST_CHECK(!Valid(snapshot, base, error));
}

BOOST_FIXTURE_TEST_CASE(young_chains_without_masternodes, SnapshotActivationChainSetup)
{
    // No masternodes exist and no DKGs have run. A rotating type then has
    // applicable cycles without quarters, so the base is unsupported; without
    // rotating types only the work blocks of h and N are carried.
    const CBlockIndex* base{WITH_LOCK(::cs_main, return m_node.chainman->ActiveTip())};
    BOOST_REQUIRE(base != nullptr);
    const auto build = [&](evo::EvoSnapshot& snapshot, std::string& error) {
        return WITH_LOCK(::cs_main, return evo::BuildEvoSnapshot(Params(), *m_node.chainman, *m_node.dmnman,
                                                                 *m_node.llmq_ctx->quorum_block_processor,
                                                                 *m_node.llmq_ctx->qsnapman, *m_node.special_tx->m_cpoolman,
                                                                 *m_node.special_tx->m_mnhfman, base, snapshot, error));
    };
    evo::EvoSnapshot snapshot;
    std::string error;
    BOOST_CHECK(!build(snapshot, error));
    BOOST_CHECK_MESSAGE(error.find("rotation quarter for applicable cycle") != std::string::npos, error);

    ConsensusParamsRestorer restorer{Params().GetConsensus()};
    std::erase_if(restorer.Get().llmqs, [](const auto& params) { return params.useRotation; });
    BOOST_REQUIRE_MESSAGE(build(snapshot, error), error);
    for (const auto& data : snapshot.quorums) {
        BOOST_CHECK(data.active_commitments.empty());
        BOOST_CHECK(data.safety_commitments.empty());
        BOOST_CHECK(!data.bootstrap_work.empty());
    }
    const bool valid{
        WITH_LOCK(::cs_main, return evo::ValidateEvoSnapshotAgainstChain(snapshot, *m_node.chainman, base, error))};
    BOOST_CHECK_MESSAGE(valid, error);
}

BOOST_FIXTURE_TEST_CASE(chain_validation_pre_dip3_matrix, TestChain100Setup)
{
    const CBlockIndex* base{WITH_LOCK(::cs_main, return m_node.chainman->ActiveTip())};
    BOOST_REQUIRE(base != nullptr);
    evo::EvoSnapshot snapshot;
    snapshot.base_block_hash = base->GetBlockHash();
    snapshot.mn_list = CDeterministicMNList{base->GetBlockHash(), base->nHeight, 0};
    std::string error;
    BOOST_CHECK(WITH_LOCK(::cs_main, return evo::ValidateEvoSnapshotAgainstChain(snapshot, *m_node.chainman, base, error)));

    auto wrong_base{snapshot};
    wrong_base.base_block_hash = H(99);
    BOOST_CHECK(
        !WITH_LOCK(::cs_main, return evo::ValidateEvoSnapshotAgainstChain(wrong_base, *m_node.chainman, base, error)));
    auto nonempty{snapshot};
    nonempty.credit_pool.locked = 1;
    BOOST_CHECK(
        !WITH_LOCK(::cs_main, return evo::ValidateEvoSnapshotAgainstChain(nonempty, *m_node.chainman, base, error)));
    auto counted{snapshot};
    counted.mn_list = CDeterministicMNList{base->GetBlockHash(), base->nHeight, 1};
    BOOST_CHECK(!WITH_LOCK(::cs_main, return evo::ValidateEvoSnapshotAgainstChain(counted, *m_node.chainman, base, error)));
    BOOST_CHECK_EQUAL(error, "nonempty pre-DIP3 evo snapshot");
}


/**
 * 530 blocks with v20 and MN_RR from height 2, so every block's CbTx records
 * a different pool balance (the platform share). Regtest's credit window is
 * P = 100 blocks; the chain is long enough to push any window out of the
 * parsed-body cache (4P entries before it truncates). Only the non-rotating
 * LLMQ_TEST type is configured, so the base needs no quorum history.
 */
struct CreditWindowSetup : public SnapshotActivationChainSetup {
    ConsensusParamsRestorer m_restorer{Params().GetConsensus()};
    static constexpr int WINDOW{100};

    CreditWindowSetup()
    {
        BOOST_REQUIRE_EQUAL(Params().CreditPoolPeriodBlocks(), WINDOW);
        mineBlocks(428);
        m_restorer.Get().llmqs = {evo::SnapshotLLMQParams(Consensus::LLMQType::LLMQ_TEST)};
    }
};

//! The same chain shape with v20 and MN_RR only from height 150.
struct CreditWindowLateV20Setup : public TestChainSetup {
    ConsensusParamsRestorer m_restorer{Params().GetConsensus()};

    CreditWindowLateV20Setup() :
        TestChainSetup{104,
                       CBaseChainParams::REGTEST,
                       {"-dip3params=2:2", "-testactivationheight=v20@150", "-testactivationheight=mn_rr@150"}}
    {
        mineBlocks(226);
        m_restorer.Get().llmqs = {evo::SnapshotLLMQParams(Consensus::LLMQType::LLMQ_TEST)};
    }
};

const CBlockIndex* Ancestor(node::NodeContext& node, int height)
{
    return WITH_LOCK(::cs_main, return node.chainman->ActiveChain()[height]);
}

//! Make the bodies of the blocks in [first, last] unreadable, as on a node
//! that never downloaded them, until destruction. Also evicts the cache of
//! entries parsed from real bodies, so nothing serves them from memory.
class [[nodiscard]] AbsentBodies
{
    std::map<CBlockIndex*, uint32_t> m_saved;

public:
    AbsentBodies(const node::NodeContext& node, int first, int last)
    {
        LOCK(::cs_main);
        for (int height{first}; height <= last; ++height) {
            auto* index{const_cast<CBlockIndex*>(node.chainman->ActiveChain()[height])};
            m_saved.emplace(index, index->nStatus);
            index->nStatus &= ~BLOCK_HAVE_DATA;
        }
        // Push the bodies out of the parsed-body cache by reading every other
        // block's.
        const CBlockIndex* tip{node.chainman->ActiveTip()};
        for (int height{tip->nHeight}; height >= 0; --height) {
            if (height < first || height > last) {
                (void)CCreditPoolManager::GetBalanceAt(tip->GetAncestor(height), Params().GetConsensus());
            }
        }
    }
    ~AbsentBodies()
    {
        LOCK(::cs_main);
        for (const auto& [index, status] : m_saved)
            index->nStatus = status;
    }
};

std::vector<CCreditPool> PoolsAfter(node::NodeContext& node, const EvoViewToken& view, int base, int count)
{
    // Block connection constructs pools under cs_main, which the versionbits
    // and MNHF lookups they make take in that order.
    LOCK(::cs_main);
    const EvoViewScope scope{view};
    std::vector<CCreditPool> pools;
    for (int k{1}; k <= count; ++k)
        pools.push_back(node.special_tx->m_cpoolman->GetCreditPool(Ancestor(node, base + k)));
    return pools;
}

void CheckSamePools(const std::vector<CCreditPool>& actual, const std::vector<CCreditPool>& expected)
{
    BOOST_REQUIRE_EQUAL(actual.size(), expected.size());
    for (size_t i{0}; i < actual.size(); ++i) {
        BOOST_CHECK_MESSAGE(SerializeHash(actual[i]) == SerializeHash(expected[i]),
                            strprintf("pool at base + %d: %s vs %s", i + 1, actual[i].ToString(), expected[i].ToString()));
    }
}

BOOST_FIXTURE_TEST_CASE(credit_window_is_carried_and_consumed, CreditWindowSetup)
{
    // B = 110: the window is [11, 110], and pools up to B + P + 2 read the
    // carried entries until B + P + 1 reaches the first block above the base.
    const int base_height{110};
    const CBlockIndex* base{Ancestor(m_node, base_height)};
    const auto snapshot{BuildEvoSnapshotAt(m_node, base)};
    BOOST_REQUIRE_EQUAL(snapshot.credit_history.size(), static_cast<size_t>(WINDOW));
    BOOST_CHECK(snapshot.credit_history.back().credit_pool == snapshot.credit_pool.locked);
    BOOST_CHECK(snapshot.credit_history.front().credit_pool < snapshot.credit_history.back().credit_pool);
    std::string error;
    BOOST_CHECK_MESSAGE(ValidAgainstChain(m_node, snapshot, base, error), error);

    const auto oracle{PoolsAfter(m_node, m_node.evodb->ViewFor(EvoDbIdentity::NORMAL), base_height, WINDOW + 2)};
    const EvoViewToken seeded{SeedGeneration(m_node, snapshot, base)};
    {
        const AbsentBodies absent{m_node, base_height - WINDOW + 1, base_height};
        CheckSamePools(PoolsAfter(m_node, seeded, base_height, WINDOW + 2), oracle);
    }

    // A carried entry for a block this node also has must match its body.
    auto tampered{snapshot};
    tampered.credit_history[5].credit_pool += 1;
    const EvoViewToken mismatched{SeedGeneration(m_node, tampered, base)};
    BOOST_CHECK_THROW(PoolsAfter(m_node, mismatched, base_height, WINDOW), evo::SnapshotStateMismatchError);

    // A missing entry is unavailable, never a guessed zero.
    auto short_history{snapshot};
    short_history.credit_history.erase(short_history.credit_history.begin());
    const EvoViewToken incomplete{SeedGeneration(m_node, short_history, base)};
    {
        const AbsentBodies absent{m_node, base_height - WINDOW + 1, base_height};
        BOOST_CHECK_THROW(PoolsAfter(m_node, incomplete, base_height, 1), BlockDataUnavailableError);
    }
}

BOOST_FIXTURE_TEST_CASE(credit_window_expires_carried_unlocks_at_both_edges, CreditWindowSetup)
{
    // No unit-test chain can mine an asset unlock (it needs a quorum
    // signature), so carry unlocked amounts at the window's first and last
    // block and check the sliding sum they leave as they expire.
    const int base_height{110};
    const CBlockIndex* base{Ancestor(m_node, base_height)};
    auto snapshot{BuildEvoSnapshotAt(m_node, base)};
    snapshot.credit_history.front().unlocked = 7 * COIN;
    snapshot.credit_history.back().unlocked = 9 * COIN;
    snapshot.credit_pool.latelyUnlocked = 16 * COIN;
    BOOST_REQUIRE_NO_THROW(snapshot.Validate());
    const EvoViewToken seeded{SeedGeneration(m_node, snapshot, base)};
    const AbsentBodies absent{m_node, base_height - WINDOW + 1, base_height};
    const auto pools{PoolsAfter(m_node, seeded, base_height, WINDOW + 1)};
    BOOST_CHECK_EQUAL(pools[0].latelyUnlocked, 9 * COIN);          // B + 1 expires the first entry
    BOOST_CHECK_EQUAL(pools[WINDOW - 2].latelyUnlocked, 9 * COIN); // B + P - 1 still holds the last
    BOOST_CHECK_EQUAL(pools[WINDOW - 1].latelyUnlocked, 0);        // B + P expires it
    BOOST_CHECK_EQUAL(pools[WINDOW].latelyUnlocked, 0);
}

BOOST_FIXTURE_TEST_CASE(credit_window_spans_v20_activation, CreditWindowLateV20Setup)
{
    // B = 110 precedes v20 (150), which activates inside (B, B + P].
    const int base_height{110};
    const CBlockIndex* base{Ancestor(m_node, base_height)};
    const auto snapshot{BuildEvoSnapshotAt(m_node, base)};
    BOOST_REQUIRE_EQUAL(snapshot.credit_history.size(), 100U);
    for (const auto& entry : snapshot.credit_history)
        BOOST_CHECK(entry == CreditHistoryEntry{});
    BOOST_CHECK_EQUAL(snapshot.credit_pool.locked, 0);
    std::string error;
    BOOST_CHECK_MESSAGE(ValidAgainstChain(m_node, snapshot, base, error), error);

    const auto oracle{PoolsAfter(m_node, m_node.evodb->ViewFor(EvoDbIdentity::NORMAL), base_height, 102)};
    BOOST_REQUIRE(oracle.back().locked > 0);
    const EvoViewToken seeded{SeedGeneration(m_node, snapshot, base)};
    const AbsentBodies absent{m_node, base_height - 99, base_height};
    CheckSamePools(PoolsAfter(m_node, seeded, base_height, 102), oracle);

    // Before v20 there is no pool, so neither the window nor the base pool
    // may claim one.
    auto early_entry{snapshot};
    early_entry.credit_history[3].credit_pool = 1;
    early_entry.credit_history.back().credit_pool = 1;
    early_entry.credit_pool.locked = 1;
    BOOST_CHECK(!ValidAgainstChain(m_node, early_entry, base, error));
    BOOST_CHECK_MESSAGE(error.find("before v20") != std::string::npos, error);
}

BOOST_FIXTURE_TEST_CASE(credit_window_validation, CreditWindowSetup)
{
    for (const auto& chain :
         {CBaseChainParams::MAIN, CBaseChainParams::TESTNET, CBaseChainParams::DEVNET, CBaseChainParams::REGTEST}) {
        const auto params{CreateChainParams(*m_node.args, chain)};
        BOOST_CHECK_LE(static_cast<size_t>(params->CreditPoolPeriodBlocks()), evo::EVO_SNAPSHOT_MAX_CREDIT_HISTORY);
    }

    const CBlockIndex* base{Ancestor(m_node, 110)};
    const auto snapshot{BuildEvoSnapshotAt(m_node, base)};
    std::string error;
    BOOST_REQUIRE_MESSAGE(ValidAgainstChain(m_node, snapshot, base, error), error);

    auto short_count{snapshot};
    short_count.credit_history.erase(short_count.credit_history.begin());
    BOOST_CHECK(!ValidAgainstChain(m_node, short_count, base, error));
    BOOST_CHECK_MESSAGE(error.find("credit-pool window") != std::string::npos, error);
    auto long_count{snapshot};
    long_count.credit_history.insert(long_count.credit_history.begin(), CreditHistoryEntry{});
    BOOST_CHECK(!ValidAgainstChain(m_node, long_count, base, error));

    auto wrong_sum{snapshot};
    wrong_sum.credit_history[10].unlocked = 1;
    BOOST_CHECK_THROW(wrong_sum.Validate(), std::ios_base::failure);
    auto wrong_end{snapshot};
    wrong_end.credit_history.back().credit_pool -= 1;
    BOOST_CHECK_THROW(wrong_end.Validate(), std::ios_base::failure);
    auto out_of_range{snapshot};
    out_of_range.credit_history[0].credit_pool = MAX_MONEY + 1;
    BOOST_CHECK_THROW(out_of_range.Validate(), std::ios_base::failure);
    auto oversized{snapshot};
    oversized.credit_history.resize(evo::EVO_SNAPSHOT_MAX_CREDIT_HISTORY + 1);
    oversized.credit_history.back().credit_pool = oversized.credit_pool.locked;
    BOOST_CHECK_THROW(oversized.Validate(), std::ios_base::failure);

    // Young chain: the window is the whole chain, genesis included.
    const CBlockIndex* young{Ancestor(m_node, 40)};
    const auto young_snapshot{BuildEvoSnapshotAt(m_node, young)};
    BOOST_CHECK_EQUAL(young_snapshot.credit_history.size(), 41U);
    BOOST_CHECK(young_snapshot.credit_history.front() == CreditHistoryEntry{});
    BOOST_CHECK_MESSAGE(ValidAgainstChain(m_node, young_snapshot, young, error), error);
}


BOOST_FIXTURE_TEST_CASE(base_coinbase_codec_bounds, BasicTestingSetup)
{
    const auto snapshot{SyntheticSnapshot()};
    BOOST_REQUIRE_NO_THROW(snapshot.Validate());

    auto wrong_height{snapshot};
    wrong_height.base_coinbase = SyntheticCoinbase(499);
    BOOST_CHECK_EXCEPTION(wrong_height.Validate(), std::ios_base::failure, [](const auto& e) {
        return std::string{e.what()}.find("coinbase height") != std::string::npos;
    });
    auto not_coinbase{snapshot};
    {
        CMutableTransaction tx{*snapshot.base_coinbase};
        tx.vin[0].prevout = COutPoint{H(1), 0};
        not_coinbase.base_coinbase = MakeTransactionRef(std::move(tx));
    }
    CheckInvalid(not_coinbase);
    auto wrong_type{snapshot};
    {
        CMutableTransaction tx{*snapshot.base_coinbase};
        tx.nType = TRANSACTION_NORMAL;
        wrong_type.base_coinbase = MakeTransactionRef(std::move(tx));
    }
    CheckInvalid(wrong_type);
    // A 64-byte transaction could pass for an inner merkle node.
    auto sixty_four{snapshot};
    {
        CMutableTransaction tx;
        tx.nVersion = 3;
        tx.nType = TRANSACTION_COINBASE;
        tx.vin.resize(1);
        tx.vin[0].prevout.SetNull();
        tx.vin[0].scriptSig = CScript{} << OP_1 << OP_1;
        tx.vout.emplace_back(0, CScript{});
        tx.vExtraPayload = {0x01};
        BOOST_REQUIRE_EQUAL(::GetSerializeSize(CTransaction{tx}, PROTOCOL_VERSION), 64U);
        sixty_four.base_coinbase = MakeTransactionRef(std::move(tx));
    }
    CheckInvalid(sixty_four);
    auto long_branch{snapshot};
    long_branch.base_coinbase_branch.resize(evo::EVO_SNAPSHOT_MAX_COINBASE_BRANCH + 1);
    CheckInvalid(long_branch);
    // Validate (and so hashing and emission) applies the decoder's length cap.
    auto oversized_coinbase{snapshot};
    {
        CMutableTransaction tx{*snapshot.base_coinbase};
        tx.vout[0].scriptPubKey.assign(evo::EVO_SNAPSHOT_MAX_COINBASE_BYTES, OP_TRUE);
        BOOST_REQUIRE_GT(::GetSerializeSize(CTransaction{tx}, PROTOCOL_VERSION), evo::EVO_SNAPSHOT_MAX_COINBASE_BYTES);
        oversized_coinbase.base_coinbase = MakeTransactionRef(std::move(tx));
    }
    CheckInvalid(oversized_coinbase);
    BOOST_CHECK_THROW(GetEvoSnapshotHash(oversized_coinbase), std::ios_base::failure);
    // Without a CbTx at the base there can be no MN or quorum state.
    auto missing{snapshot};
    missing.base_coinbase.reset();
    missing.base_coinbase_branch.clear();
    CheckInvalid(missing);

    // Decoding bounds the length before allocating and wants exactly one
    // canonical transaction.
    const auto prefix = [](CDataStream& stream) {
        stream << evo::EVO_SNAPSHOT_VERSION << H(42);
        evo::SerializeCanonicalMNList(stream, CDeterministicMNList{H(42), 500, 0});
    };
    evo::EvoSnapshot decoded;
    CDataStream oversized{SER_DISK, CLIENT_VERSION};
    prefix(oversized);
    WriteCompactSize(oversized, evo::EVO_SNAPSHOT_MAX_COINBASE_BYTES + 1);
    BOOST_CHECK_EXCEPTION(oversized >> decoded, std::ios_base::failure, [](const auto& e) {
        return std::string{e.what()}.find("oversized evo snapshot base coinbase") != std::string::npos;
    });
    const auto coinbase_bytes{SerializeTransactionForTest(*snapshot.base_coinbase)};
    CDataStream trailing{SER_DISK, CLIENT_VERSION};
    prefix(trailing);
    WriteCompactSize(trailing, coinbase_bytes.size() + 1);
    trailing.write(MakeByteSpan(coinbase_bytes));
    trailing << uint8_t{0};
    BOOST_CHECK_EXCEPTION(trailing >> decoded, std::ios_base::failure, [](const auto& e) {
        return std::string{e.what()}.find("trailing bytes") != std::string::npos;
    });
    CDataStream truncated{SER_DISK, CLIENT_VERSION};
    prefix(truncated);
    WriteCompactSize(truncated, coinbase_bytes.size() - 1);
    truncated.write(MakeByteSpan(coinbase_bytes).first(coinbase_bytes.size() - 1));
    BOOST_CHECK_THROW(truncated >> decoded, std::ios_base::failure);
}

BOOST_FIXTURE_TEST_CASE(base_coinbase_is_proven_against_the_base_header, CreditWindowSetup)
{
    const CBlockIndex* base{Ancestor(m_node, 110)};
    const auto snapshot{BuildEvoSnapshotAt(m_node, base)};
    BOOST_REQUIRE(snapshot.base_coinbase);
    std::string error;
    BOOST_CHECK_MESSAGE(evo::VerifyEvoSnapshotBaseBlock(snapshot, *base, Params().GetConsensus(), error), error);
    BOOST_CHECK_MESSAGE(ValidAgainstChain(m_node, snapshot, base, error), error);

    // Another header: its merkle root does not commit to this coinbase.
    BOOST_CHECK(!evo::VerifyEvoSnapshotBaseBlock(snapshot, *base->pprev, Params().GetConsensus(), error));
    auto tampered_branch{snapshot};
    if (tampered_branch.base_coinbase_branch.empty()) {
        tampered_branch.base_coinbase_branch.push_back(H(1));
    } else {
        tampered_branch.base_coinbase_branch[0] = H(1);
    }
    BOOST_CHECK(!evo::VerifyEvoSnapshotBaseBlock(tampered_branch, *base, Params().GetConsensus(), error));
    BOOST_CHECK(!ValidAgainstChain(m_node, tampered_branch, base, error));
    BOOST_CHECK_MESSAGE(error.find("not proven by the base header") != std::string::npos, error);
    auto tampered_coinbase{snapshot};
    {
        CMutableTransaction tx{*snapshot.base_coinbase};
        tx.vout[0].nValue -= 1;
        tampered_coinbase.base_coinbase = MakeTransactionRef(std::move(tx));
    }
    BOOST_CHECK(!evo::VerifyEvoSnapshotBaseBlock(tampered_coinbase, *base, Params().GetConsensus(), error));
    BOOST_CHECK(!ValidAgainstChain(m_node, tampered_coinbase, base, error));
    // A proven coinbase whose CbTx commits to other state is rejected too.
    auto other_pool{snapshot};
    other_pool.credit_pool.locked += 1;
    other_pool.credit_history.back().credit_pool += 1;
    BOOST_CHECK(!evo::VerifyEvoSnapshotBaseBlock(other_pool, *base, Params().GetConsensus(), error));
    BOOST_CHECK_MESSAGE(error.find("credit pool balance") != std::string::npos, error);
    // DIP3 is active at the base, so the coinbase is required.
    auto missing{snapshot};
    missing.base_coinbase.reset();
    missing.base_coinbase_branch.clear();
    BOOST_CHECK(!ValidAgainstChain(m_node, missing, base, error));

    // The branch reproduces the root for every position of larger trees.
    CBlock block;
    for (int i{0}; i < 7; ++i) {
        CMutableTransaction tx;
        tx.vin.resize(1);
        tx.vin[0].prevout = COutPoint{H(static_cast<uint8_t>(i)), 0};
        block.vtx.push_back(MakeTransactionRef(std::move(tx)));
    }
    for (uint32_t i{0}; i < block.vtx.size(); ++i) {
        BOOST_CHECK(evo::ComputeMerkleRootFromBranch(block.vtx[i]->GetHash(), evo::BlockMerkleBranch(block, i), i) ==
                    BlockMerkleRoot(block));
    }
}

BOOST_FIXTURE_TEST_CASE(served_quorum_chainlocks_are_never_guessed_null, EvoSnapshotChainSetup)
{
    // A mnlistdiff serving the carried quorum 168 reports the ChainLock its
    // work block's coinbase (160) certifies. Without that body the snapshot
    // view cannot tell, so it must report unavailable, never a null signature.
    SyncNormal();
    MineCycles(12, 180);
    const CBlockIndex* base{At(179)};
    const EvoViewToken seeded{Seed(Build(base), base)};
    LOCK(::cs_main);
    const auto commitment{
        m_node.llmq_ctx->quorum_block_processor->GetMinedCommitment(m_plain.type, At(168)->GetBlockHash()).first};
    const auto serve = [&](const EvoViewToken& view, std::string& error) {
        const EvoViewScope scope{view};
        CSimplifiedMNListDiff diff;
        diff.newQuorums = {commitment};
        return diff.BuildQuorumChainlockInfo(*m_node.llmq_ctx->qman, base, error);
    };
    std::string error;
    BOOST_REQUIRE_MESSAGE(serve(seeded, error), error);
    {
        // An indexed body that does not read back is no answer either.
        auto* work{const_cast<CBlockIndex*>(At(160))};
        const auto saved{std::make_pair(work->nFile, work->nDataPos)};
        std::tie(work->nFile, work->nDataPos) = std::make_pair(At(159)->nFile, At(159)->nDataPos);
        const bool served{serve(seeded, error)};
        std::tie(work->nFile, work->nDataPos) = saved;
        BOOST_CHECK(!served);
        BOOST_CHECK_MESSAGE(IsBlockDataUnavailableError(error), error);
    }
    const AbsentBodies absent{m_node, 160, 160};
    BOOST_CHECK(!serve(seeded, error));
    BOOST_CHECK_MESSAGE(IsBlockDataUnavailableError(error), error);
    // NORMAL keeps answering from what it has.
    BOOST_CHECK_MESSAGE(serve(Normal(), error), error);
}

BOOST_FIXTURE_TEST_CASE(base_coinbase_serves_the_base_chainlock, CreditWindowSetup)
{
    // Unit-test chains have no ChainLocks, so carry a coinbase whose CbTx
    // certifies one; seeding does not prove it, only the load gates do.
    const CBlockIndex* base{Ancestor(m_node, 110)};
    auto snapshot{BuildEvoSnapshotAt(m_node, base)};
    CBLSSecretKey key;
    key.MakeNewKey();
    const CBLSSignature signature{key.Sign(H(7), /*specificLegacyScheme=*/false)};
    {
        auto cbtx{*Assert(DecodeCbTxPayload(*snapshot.base_coinbase, /*legacy_bls_scheme=*/false))};
        cbtx.bestCLSignature = signature;
        cbtx.bestCLHeightDiff = 3;
        CMutableTransaction tx{*snapshot.base_coinbase};
        SetTxPayload(tx, cbtx);
        snapshot.base_coinbase = MakeTransactionRef(std::move(tx));
    }
    const EvoViewToken seeded{SeedGeneration(m_node, snapshot, base)};
    const AbsentBodies absent{m_node, base->nHeight - 1, base->nHeight};
    // Without the block, the ordinary reader cannot tell a ChainLock from none.
    BOOST_CHECK(!GetNonNullCoinbaseChainlock(base).has_value());

    LOCK(::cs_main);
    {
        const EvoViewScope scope{seeded};
        const auto carried{m_node.special_tx->GetCoinbaseChainlock(base)};
        BOOST_REQUIRE(carried.status == CoinbaseChainlock::Status::CERTIFIED);
        BOOST_CHECK(carried.signature == signature);
        BOOST_CHECK_EQUAL(carried.height_diff, 3U);
        // Below the base nothing is carried: unavailable, never "no ChainLock".
        BOOST_CHECK(m_node.special_tx->GetCoinbaseChainlock(base->pprev).status == CoinbaseChainlock::Status::UNAVAILABLE);
    }
    const EvoViewScope normal{m_node.evodb->ViewFor(EvoDbIdentity::NORMAL)};
    BOOST_CHECK(m_node.special_tx->GetCoinbaseChainlock(base->pprev).status == CoinbaseChainlock::Status::NONE);
}


/**
 * The 102-block activation chain with only LLMQ_TEST configured, and the
 * testdummy bit reconfigured as an EHF deployment whose window has not
 * started: a signal for it is the out-of-window premise of
 * evo_mnhf_tests/mnhf_bit_reuse_reindex_tests, which GetSignalsStage() drops.
 */
struct MnhfSnapshotSetup : public SnapshotActivationChainSetup {
    ConsensusParamsRestorer m_restorer{Params().GetConsensus()};
    int m_bit{-1};

    MnhfSnapshotSetup()
    {
        auto& consensus{m_restorer.Get()};
        consensus.llmqs = {evo::SnapshotLLMQParams(Consensus::LLMQType::LLMQ_TEST)};
        auto& deployment{consensus.vDeployments[Consensus::DEPLOYMENT_TESTDUMMY]};
        deployment.nStartTime = 2'000'000'000;
        deployment.useEHF = true;
        m_bit = deployment.bit;
    }

    //! Rewrite the NORMAL signal record of `index`, as block connection
    //! would have written it, and drop every cached copy.
    void WriteSignals(const CBlockIndex* index, const std::optional<CMNHFManager::Signals>& signals)
    {
        {
            LOCK(::cs_main);
            auto tx{m_node.evodb->BeginTransaction(EvoDbIdentity::NORMAL)};
            const auto key{std::make_pair(std::string{"mnhf_s2"}, index->GetBlockHash())};
            m_node.evodb->Erase(key);
            if (signals) m_node.evodb->Write(key, *signals);
            tx->Commit();
            BOOST_REQUIRE(m_node.evodb->CommitRootTransaction(EvoDbIdentity::NORMAL));
        }
        // Opening an EvoDB starts a new view-cache epoch.
        CEvoDB epoch{util::DbWrapperParams{.path = m_args.GetDataDirBase() / "mnhf_epoch", .memory = true, .wipe = true}};
    }
};

BOOST_FIXTURE_TEST_CASE(mnhf_raw_signal_record_is_exported, MnhfSnapshotSetup)
{
    const CBlockIndex* base{Ancestor(m_node, 100)};
    const CMNHFManager::Signals raw{{static_cast<uint8_t>(m_bit), base->nHeight}};
    WriteSignals(base, raw);
    auto& mnhfman{*m_node.special_tx->m_mnhfman};
    {
        LOCK(::cs_main);
        BOOST_REQUIRE(mnhfman.GetForBlock(base) == raw);
        BOOST_REQUIRE(mnhfman.GetSignalsStage(base).empty());
    }

    const auto snapshot{BuildEvoSnapshotAt(m_node, base)};
    BOOST_CHECK(snapshot.mnhf_signals == raw);
    std::string error;
    BOOST_CHECK_MESSAGE(ValidAgainstChain(m_node, snapshot, base, error), error);

    const EvoViewToken seeded{SeedGeneration(m_node, snapshot, base)};
    LOCK(::cs_main);
    const EvoViewScope scope{seeded};
    CMNHFManager::Signals stored;
    BOOST_REQUIRE(m_node.evodb->Read(std::make_pair(std::string{"mnhf_s2"}, base->GetBlockHash()), stored));
    BOOST_CHECK(stored == raw);
    BOOST_CHECK(mnhfman.GetForBlock(base) == raw);
    // B + 1 sees the record filtered exactly as NORMAL does.
    BOOST_CHECK(mnhfman.GetSignalsStage(base).empty());
    // Below the base only bodies can answer: without them the record is
    // unavailable, never a guess; with them it is rebuilt as NORMAL has it.
    {
        const AbsentBodies absent{m_node, base->nHeight - 10, base->nHeight - 1};
        BOOST_CHECK_THROW(mnhfman.GetSignalsStage(base->pprev), BlockDataUnavailableError);
    }
    const auto rebuilt{mnhfman.GetSignalsStage(base->pprev)};
    const EvoViewScope normal{m_node.evodb->ViewFor(EvoDbIdentity::NORMAL)};
    BOOST_CHECK(rebuilt == mnhfman.GetSignalsStage(base->pprev));
}

BOOST_FIXTURE_TEST_CASE(mnhf_export_rebuilds_a_record_held_only_in_memory, MnhfSnapshotSetup)
{
    // The exporter runs outside any block transaction, where a rebuilt signal
    // record is only cached, never written. It still exports that record. The
    // deployment's window is open, so the rebuild carries the signal forward.
    m_restorer.Get().vDeployments[Consensus::DEPLOYMENT_TESTDUMMY].nStartTime = 0;
    const CBlockIndex* base{Ancestor(m_node, 90)};
    const CMNHFManager::Signals raw{{static_cast<uint8_t>(m_bit), 84}};
    WriteSignals(base->GetAncestor(84), raw);
    for (int height{85}; height <= base->nHeight; ++height)
        WriteSignals(base->GetAncestor(height), std::nullopt);
    BOOST_REQUIRE(!WITH_LOCK(::cs_main, return m_node.evodb->HasActiveTransaction()));

    const auto snapshot{BuildEvoSnapshotAt(m_node, base)};
    BOOST_CHECK(snapshot.mnhf_signals == raw);
    LOCK(::cs_main);
    CMNHFManager::Signals stored;
    BOOST_CHECK(!m_node.evodb->Read(std::make_pair(std::string{"mnhf_s2"}, base->GetBlockHash()), stored));
    BOOST_CHECK(WITH_LOCK(m_node.evodb->cs, return m_node.evodb->GetCurTransaction().IsClean()));
}


BOOST_FIXTURE_TEST_CASE(work_block_modifiers_are_exact_in_the_snapshot_view, EvoSnapshotChainSetup)
{
    SyncNormal();
    MineCycles(12, 180);
    const CBlockIndex* base{At(179)};
    auto snapshot{Build(base)};
    // Unit-test chains have no ChainLocks, so carry the modifier that a
    // ChainLock in the coinbase of N's work block (172) would give. The key is
    // fixed: whether a modifier selects members other than NORMAL's is chance,
    // so the scenario pins one that does.
    std::array<uint8_t, CBLSSecretKey::SerSize> secret{};
    secret.back() = 9;
    const CBLSSecretKey key{secret};
    BOOST_REQUIRE(key.IsValid());
    const CBLSSignature signature{key.Sign(H(9), /*specificLegacyScheme=*/false)};
    const uint256 certified{SerializeHash(std::make_tuple(m_rotated.type, 172, signature))};
    for (auto& entry : snapshot.quorum_modifiers) {
        if (entry.llmq_type == m_rotated.type && entry.work_block_hash == At(172)->GetBlockHash()) {
            entry.modifier = certified;
        }
    }
    const EvoViewToken seeded{Seed(snapshot, base)};
    const auto modifier_at = [&](const EvoViewToken& view, int height) {
        const EvoViewScope scope{view};
        return llmq::utils::GetQuorumModifierForWorkBlock(m_rotated, m_node.chainman->GetConsensus(), At(height),
                                                          *m_node.llmq_ctx->qsnapman);
    };
    // With the work block readable, the carried value must match it.
    BOOST_CHECK_THROW(modifier_at(seeded, 172), evo::SnapshotStateMismatchError);

    const AbsentBodies absent{m_node, 160, 172};
    BOOST_CHECK(modifier_at(seeded, 172) == certified);
    // NORMAL cannot read the block either and keeps its legacy fallback.
    BOOST_CHECK(modifier_at(Normal(), 172) == SerializeHash(std::make_pair(m_rotated.type, At(172)->GetBlockHash())));
    // Nothing is carried for block 165, so the snapshot view cannot tell.
    BOOST_CHECK_THROW(modifier_at(seeded, 165), BlockDataUnavailableError);

    // The prediction path and the ordinary member computation of N use the
    // same exact modifier, which differs from NORMAL's fallback.
    const auto predicted{Predicted(seeded, m_rotated, 180)};
    BOOST_REQUIRE(predicted.has_value());
    BOOST_CHECK(*predicted == Members(seeded, m_rotated, 180));
    BOOST_CHECK(*predicted != *Predicted(Normal(), m_rotated, 180));
}

BOOST_FIXTURE_TEST_CASE(work_block_modifiers_use_the_block_scheme, BasicTestingSetup)
{
    // A background chainstate connects pre-v19 blocks with the process-wide
    // BLS scheme set to legacy, while other threads compute members from later
    // work blocks. The coinbase ChainLock must be decoded and hashed in the
    // scheme of the block that carries it, whatever the process runs.
    const auto& consensus{Params().GetConsensus()};
    const int v19{consensus.DeploymentHeight(Consensus::DEPLOYMENT_V19)};
    const auto& params{evo::SnapshotLLMQParams(Consensus::LLMQType::LLMQ_TEST)};
    CBLSSecretKey key;
    key.MakeNewKey();
    const CBLSSignature signature{key.Sign(H(5), /*specificLegacyScheme=*/false)};
    const struct RestoreScheme {
        const bool saved{bls::bls_legacy_scheme.load()};
        ~RestoreScheme() { bls::bls_legacy_scheme.store(saved); }
    } restore;
    for (const int height : {v19 - 1, v19, v19 + 1}) {
        const bool block_legacy{height < v19};
        // The miner wrote the coinbase in the block's scheme.
        bls::bls_legacy_scheme.store(block_legacy);
        CMutableTransaction coinbase{*SyntheticCoinbase(height)};
        CCbTx cbtx{*Assert(DecodeCbTxPayload(CTransaction{coinbase}, block_legacy))};
        cbtx.bestCLSignature = signature;
        cbtx.bestCLHeightDiff = 2;
        SetTxPayload(coinbase, cbtx);

        // The expected modifier hashes the signature bytes as mined: the 96
        // bytes before the trailing credit pool balance.
        const auto& payload{coinbase.vExtraPayload};
        const std::vector<uint8_t> mined(payload.end() - sizeof(CAmount) - CBLSSignature::SerSize,
                                         payload.end() - sizeof(CAmount));
        BOOST_REQUIRE(mined == signature.ToByteVector(block_legacy));
        CHashWriter hasher{SER_GETHASH, PROTOCOL_VERSION};
        hasher << static_cast<uint8_t>(params.type) << height;
        hasher.write(MakeByteSpan(mined));
        const uint256 expected{hasher.GetHash()};

        CBlock block;
        block.vtx.push_back(MakeTransactionRef(std::move(coinbase)));
        const uint256 block_hash{H(151)};
        CBlockIndex work;
        work.nHeight = height;
        work.phashBlock = &block_hash;
        for (const bool ambient : {false, true}) {
            bls::bls_legacy_scheme.store(ambient);
            BOOST_CHECK_MESSAGE(llmq::utils::GetWorkBlockModifier(params, consensus, block, work) == expected,
                                strprintf("height %d, ambient legacy %d", height, ambient));
        }
    }
}

BOOST_FIXTURE_TEST_CASE(missing_quarters_in_the_snapshot_view, EvoSnapshotChainSetup)
{
    // B = 31 (h = 24) carries the quarters of 12 and 24. Cycle 24's members
    // read the quarters of 12 and of genesis, which no cycle ever has.
    SyncNormal();
    MineCycles(12, 180);
    const CBlockIndex* base{At(31)};
    auto snapshot{Build(base)};
    BOOST_REQUIRE(RotationHeights(snapshot) == (std::set<int>{12, 24}));
    const EvoViewToken seeded{Seed(snapshot, base)};
    // An inapplicable quarter keeps the legacy break, as NORMAL does.
    CheckMembers(seeded, m_rotated, 24);
    CheckMembers(seeded, m_rotated, 25);

    // An applicable one raises unavailable rather than different members.
    auto& rotated{const_cast<evo::QuorumSnapshotData&>(Type(snapshot, m_rotated.type))};
    std::erase_if(rotated.rotation_snapshots,
                  [&](const auto& entry) { return entry.cycle_base_block_hash == At(12)->GetBlockHash(); });
    const EvoViewToken incomplete{Seed(snapshot, base)};
    const EvoViewScope scope{incomplete};
    BOOST_CHECK_THROW(llmq::utils::GetAllQuorumMembers(m_rotated.type, Util(At(24)), /*reset_cache=*/true),
                      BlockDataUnavailableError);
}


std::vector<uint256> QuorumHashes(const std::vector<llmq::CQuorumCPtr>& quorums)
{
    std::vector<uint256> hashes;
    hashes.reserve(quorums.size());
    for (const auto& quorum : quorums)
        hashes.push_back(quorum->qc->quorumHash);
    return hashes;
}

BOOST_FIXTURE_TEST_CASE(scan_coverage_gates_export_and_load, EvoSnapshotChainSetup)
{
    SyncNormal();
    MineCycles(12, 180);
    const CBlockIndex* base{At(179)};
    const auto snapshot{Build(base)};
    std::string error;
    BOOST_REQUIRE_MESSAGE(Valid(snapshot, base, error), error);

    // ChainLocks on a type with retained history. These chains carry no
    // ChainLock, so B + 1 may certify any height and its verification scans
    // reach below the three carried commitments: export and load refuse.
    m_restorer.Get().llmqTypeChainLocks = m_plain.type;
    evo::EvoSnapshot refused;
    BOOST_CHECK(!TryBuild(base, refused, error));
    BOOST_CHECK_MESSAGE(error.find("cannot prove") != std::string::npos, error);
    BOOST_CHECK(!Valid(snapshot, base, error));
    BOOST_CHECK_MESSAGE(error.find("cannot prove") != std::string::npos, error);

    // A young chain's short history is complete, so it proves every scan.
    const CBlockIndex* young{At(31)};
    const auto young_snapshot{Build(young)};
    BOOST_CHECK_MESSAGE(Valid(young_snapshot, young, error), error);

    // The asset-unlock check of B + 1 scans the active quorums plus the
    // previous one, which the three carried commitments cover.
    m_restorer.Get().llmqTypeChainLocks = Consensus::LLMQType::LLMQ_TEST_INSTANTSEND;
    m_restorer.Get().llmqTypePlatform = m_plain.type;
    const auto platform_snapshot{Build(base)};
    BOOST_CHECK_MESSAGE(Valid(platform_snapshot, base, error), error);
    const EvoViewToken seeded{Seed(platform_snapshot, base)};
    const size_t count{static_cast<size_t>(m_plain.signingActiveQuorumCount) + 1};
    const auto prove = [&](const EvoViewToken& view) {
        const EvoViewScope scope{view};
        return Heights([&] {
            std::vector<uint256> hashes;
            for (const auto* index : m_node.llmq_ctx->qman->ProveScan(m_plain.type, base, count)) {
                hashes.push_back(index->GetBlockHash());
            }
            return hashes;
        }());
    };
    BOOST_CHECK(prove(seeded) == (std::set<int>{144, 156, 168}));
    BOOST_CHECK(prove(seeded) == prove(Normal()));
}

BOOST_FIXTURE_TEST_CASE(scans_prove_their_prefix_in_the_snapshot_view, EvoSnapshotChainSetup)
{
    // Plain quorums at 144, 156 and 168 (mined at +2) are carried at B = 179;
    // older ones exist only in NORMAL.
    SyncNormal();
    MineCycles(12, 180);
    const CBlockIndex* base{At(179)};
    const EvoViewToken seeded{Seed(Build(base), base)};
    auto& qman{*m_node.llmq_ctx->qman};
    const auto scan = [&](const EvoViewToken& view, int start, size_t count) {
        const EvoViewScope scope{view};
        return QuorumHashes(qman.ScanQuorums(m_plain.type, At(start), count));
    };

    // A covered request near the base selects what NORMAL selects, and a
    // recovered signature verifies (or fails) identically.
    BOOST_CHECK(scan(seeded, 171, 2) == scan(Normal(), 171, 2));
    const uint256 id{H(1)};
    const uint256 msg_hash{H(2)};
    for (const int quorum_height : {144, 156, 168}) {
        const auto& key{m_quorum_keys.at({m_plain.type, quorum_height})};
        const llmq::SignHash sign_hash{m_plain.type, At(quorum_height)->GetBlockHash(), id, msg_hash};
        const CBLSSignature signature{key.Sign(sign_hash.Get(), /*specificLegacyScheme=*/false)};
        const auto verify = [&](const EvoViewToken& view) {
            const EvoViewScope scope{view};
            return llmq::VerifyRecoveredSig(m_plain.type, qman, At(171), id, msg_hash, signature);
        };
        BOOST_CHECK(verify(seeded) == verify(Normal()));
    }

    // At start 160 (cutoff 160, in cycle 156's window) two carried quorums are
    // eligible. A request for them succeeds even though the usual warm-up
    // scan of keepOldConnections (3) could not be proven; a request for three
    // must prove three, and cannot.
    BOOST_CHECK(scan(seeded, 160, 1) == scan(Normal(), 160, 1));
    BOOST_CHECK(scan(seeded, 160, 2) == scan(Normal(), 160, 2));
    {
        const EvoViewScope scope{seeded};
        BOOST_CHECK_THROW(qman.ScanQuorums(m_plain.type, At(160), 3), BlockDataUnavailableError);
        BOOST_CHECK_THROW(qman.ScanCommitments(m_plain.type, At(160), 3), BlockDataUnavailableError);
    }
    // A smaller request cached first never truncates a larger one.
    BOOST_CHECK(scan(seeded, 175, 1) == scan(Normal(), 175, 1));
    BOOST_CHECK(scan(seeded, 175, 3) == scan(Normal(), 175, 3));
    BOOST_CHECK_EQUAL(scan(seeded, 175, 3).size(), 3U);
}

BOOST_FIXTURE_TEST_CASE(rotating_scans_never_return_a_short_vector, EvoSnapshotChainSetup)
{
    // Quorum index 0 never forms, so index 1 alone carries [169, 157] at
    // B = 179 and older index-1 quorums exist only in NORMAL. From start 170
    // the newest (mined at 171) is not yet eligible: rank 0 yields 157, and
    // rank 1 needs index 1's next record, which is not carried.
    SyncNormal();
    for (int cycle{12}; cycle <= 180; cycle += CYCLE_LENGTH) {
        Mine(m_plain, cycle);
        Mine(m_rotated, cycle + 1);
    }
    const CBlockIndex* base{At(179)};
    const EvoViewToken seeded{Seed(Build(base), base)};
    auto& qman{*m_node.llmq_ctx->qman};
    const auto normal{[&] {
        const EvoViewScope scope{Normal()};
        return Heights(QuorumHashes(qman.ScanQuorums(m_rotated.type, At(170), 2)));
    }()};
    BOOST_REQUIRE(normal == (std::set<int>{145, 157}));
    const EvoViewScope scope{seeded};
    BOOST_CHECK_THROW(qman.ScanQuorums(m_rotated.type, At(170), 2), BlockDataUnavailableError);
    BOOST_CHECK_THROW(qman.ScanCommitments(m_rotated.type, At(170), 2), BlockDataUnavailableError);
    BOOST_CHECK(Heights(QuorumHashes(qman.ScanQuorums(m_rotated.type, At(170), 1))) == std::set<int>{157});
}


BOOST_FIXTURE_TEST_CASE(uncarried_quorums_are_unavailable_not_absent, EvoSnapshotChainSetup)
{
    // An MNHF signal may be signed by any historical quorum of its type, which
    // block validation looks up by hash. At B = 179 the plain quorums 144, 156
    // and 168 are carried; 120 exists only in NORMAL.
    SyncNormal();
    MineCycles(12, 180);
    m_restorer.Get().llmqTypeMnhf = m_plain.type;
    const CBlockIndex* base{At(179)};
    const EvoViewToken seeded{Seed(Build(base), base)};
    auto& qman{*m_node.llmq_ctx->qman};
    const auto signal_tx = [&](int quorum_height) {
        MNHFTxPayload payload;
        payload.nVersion = 1;
        payload.signal.versionBit = 5;
        payload.signal.quorumHash = At(quorum_height)->GetBlockHash();
        CMutableTransaction tx;
        tx.nVersion = 3;
        tx.nType = TRANSACTION_MNHF_SIGNAL;
        SetTxPayload(tx, payload);
        return CTransaction{tx};
    };
    {
        const EvoViewScope scope{seeded};
        BOOST_CHECK(qman.GetQuorum(m_plain.type, At(156)->GetBlockHash()) != nullptr);
        // An older quorum is not carried: the snapshot view cannot tell, so the
        // block must stall rather than be found invalid.
        BOOST_CHECK_THROW(qman.GetQuorum(m_plain.type, At(120)->GetBlockHash()), BlockDataUnavailableError);
        TxValidationState state;
        BOOST_CHECK_THROW(CheckMNHFTx(m_node.chainman->m_blockman, qman, signal_tx(120), At(185), state),
                          BlockDataUnavailableError);
        // Headers alone prove there is no quorum at a non-quorum height.
        BOOST_CHECK(qman.GetQuorum(m_plain.type, At(121)->GetBlockHash()) == nullptr);
        BOOST_CHECK(!CheckMNHFTx(m_node.chainman->m_blockman, qman, signal_tx(121), At(185), state));
        BOOST_CHECK_EQUAL(state.GetRejectReason(), "bad-mnhf-missing-quorum");
    }
    const EvoViewScope normal{Normal()};
    BOOST_CHECK(qman.GetQuorum(m_plain.type, At(120)->GetBlockHash()) != nullptr);
}

//! The quorum merkle root the CbTx of the block after `height` must commit
//! to, as block validation computes it in `view`.
uint256 QuorumRootAfter(node::NodeContext& node, const EvoViewToken& view, const CBlockIndex* prev)
{
    const EvoViewScope scope{view};
    CBlock block;
    block.vtx.push_back(SyntheticCoinbase(prev->nHeight + 1));
    uint256 root;
    BlockValidationState state;
    BOOST_REQUIRE(CalcCbTxMerkleRootQuorums(block, prev, *node.llmq_ctx->quorum_block_processor, root, state));
    return root;
}

BOOST_FIXTURE_TEST_CASE(carried_scans_answer_what_the_base_successor_reads, EvoSnapshotChainSetup)
{
    // The CbTx of B + 1 commits to the newest commitments of every configured
    // type, read by scans that may end short. With the carried payload, or with
    // none below DIP3, every short read is an answer:
    //  - LLMQ_TEST_V17 is never enabled and has no history;
    //  - LLMQ_TEST_INSTANTSEND retires at 100 with history (LLMQ_50_60's shape);
    //  - rotated index 1 never forms a quorum;
    //  - at B = 31 the plain type has fewer commitments than its selection.
    // A need deeper than the payload carries still stalls. These views have no
    // chainstate to connect blocks with; the root is what that check computes.
    // ChainLocks and Platform move to the type that never forms a quorum.
    auto& consensus{m_restorer.Get()};
    consensus.llmqs.push_back(evo::SnapshotLLMQParams(Consensus::LLMQType::LLMQ_TEST_V17));
    consensus.llmqTypeChainLocks = Consensus::LLMQType::LLMQ_TEST_V17;
    consensus.llmqTypePlatform = Consensus::LLMQType::LLMQ_TEST_V17;
    consensus.DIP0024QuorumsHeight = 100;
    const auto retired{consensus.llmqs[2]};
    BOOST_REQUIRE(retired.type == Consensus::LLMQType::LLMQ_TEST_INSTANTSEND);
    SyncNormal();
    MineCycles(12, 180, /*rotated_index_one=*/false);
    for (int height{retired.dkgInterval}; height < consensus.DIP0024QuorumsHeight; height += retired.dkgInterval) {
        Mine(retired, height);
    }
    auto& qblockman{*m_node.llmq_ctx->quorum_block_processor};
    auto& qman{*m_node.llmq_ctx->qman};
    const auto roots_match = [&](const EvoViewToken& view, int first, int last) {
        for (int height{first}; height <= last; ++height) {
            BOOST_CHECK_MESSAGE(QuorumRootAfter(m_node, view, At(height)) == QuorumRootAfter(m_node, Normal(), At(height)),
                                strprintf("quorum root after block %d", height));
        }
    };
    const auto scan = [&](const EvoViewToken& view, const Consensus::LLMQParams& params, int start, size_t count) {
        const EvoViewScope scope{view};
        return qman.ProveScan(params.type, At(start), count);
    };
    const size_t selection{evo::SnapshotCommitmentCount(m_plain, /*rotation_enabled=*/false)};

    {
        const CBlockIndex* base{At(179)};
        const EvoViewToken seeded{Seed(Build(base), base)};
        roots_match(seeded, 179, 181);
        const EvoViewScope scope{seeded};
        BOOST_CHECK(qblockman.GetMinedCommitmentsUntilBlock(Consensus::LLMQType::LLMQ_TEST_V17, base, 3).empty());
        BOOST_CHECK(!qblockman.GetLastMinedCommitmentsByQuorumIndexUntilBlock(m_rotated.type, base, 1, 0).has_value());
        BOOST_CHECK_EQUAL(qblockman.GetMinedCommitmentsUntilBlock(m_plain.type, base, selection).size(), selection);
        BOOST_CHECK_THROW(qblockman.GetMinedCommitmentsUntilBlock(m_plain.type, base, selection + 1),
                          BlockDataUnavailableError);
        BOOST_CHECK_THROW(qblockman.GetLastMinedCommitmentsByQuorumIndexUntilBlock(m_rotated.type, base, 0, 2),
                          BlockDataUnavailableError);
    }
    {
        // Two plain quorums (12, 24) where the selection takes three.
        const CBlockIndex* base{At(31)};
        const EvoViewToken seeded{Seed(Build(base), base)};
        roots_match(seeded, 31, 33);
        BOOST_CHECK(scan(seeded, m_plain, 32, selection) == scan(Normal(), m_plain, 32, selection));
        const EvoViewScope scope{seeded};
        BOOST_CHECK_EQUAL(qblockman.GetMinedCommitmentsUntilBlock(m_plain.type, base, selection).size(), 2U);
    }
    {
        // A base below DIP3 carries no evo section, so nothing is seeded; the
        // commitments mined above it are this view's own.
        const CBlockIndex* base{At(1)};
        LOCK(::cs_main);
        BOOST_REQUIRE(m_node.evodb->AbandonSnapshotGeneration());
        BOOST_REQUIRE(m_node.evodb->CreateSnapshotGeneration(base->GetBlockHash(), base->nHeight));
        const EvoViewToken unseeded{m_node.evodb->ViewFor(EvoDbIdentity::SNAPSHOT)};
        const std::vector<std::pair<Consensus::LLMQParams, int>> above_base{{m_plain, 12},
                                                                            {m_plain, 24},
                                                                            {m_rotated, 12},
                                                                            {m_rotated, 24},
                                                                            {retired, 24}};
        for (const auto& [params, height] : above_base) {
            const auto [commitment, mined]{[&] {
                const EvoViewScope normal{Normal()};
                return qblockman.GetMinedCommitment(params.type, At(height)->GetBlockHash());
            }()};
            auto tx{m_node.evodb->BeginTransaction(EvoDbIdentity::SNAPSHOT)};
            const uint256 quorum_hash{At(height)->GetBlockHash()};
            BOOST_REQUIRE(qblockman.SeedMinedCommitment(unseeded, params.type, quorum_hash, commitment, mined));
            tx->Commit();
            BOOST_REQUIRE(m_node.evodb->CommitRootTransaction(EvoDbIdentity::SNAPSHOT));
        }
        roots_match(unseeded, 30, 32);
        BOOST_CHECK(scan(unseeded, m_plain, 32, selection) == scan(Normal(), m_plain, 32, selection));
        BOOST_CHECK(scan(unseeded, m_rotated, 32, 4) == scan(Normal(), m_rotated, 32, 4));
    }
}

BOOST_FIXTURE_TEST_CASE(rotating_types_enabled_after_the_base_read_empty, EvoSnapshotChainSetup)
{
    // A rotating type is enabled exactly when rotation is at its cycle base, so
    // it never writes the non-indexed family it is read through before then.
    auto& consensus{m_restorer.Get()};
    for (int activation{140}; activation <= 160; ++activation) {
        consensus.DIP0024Height = activation;
        for (int cycle{132}; cycle <= 168; cycle += CYCLE_LENGTH) {
            const CBlockIndex* quorum{At(cycle)};
            const bool enabled{
                WITH_LOCK(::cs_main, return m_node.chainman->IsQuorumTypeEnabled(m_rotated.type, quorum->pprev))};
            BOOST_CHECK_EQUAL(enabled, llmq::IsQuorumRotationEnabled(m_rotated, quorum));
        }
    }
    // DIP0024 at 150: the rotated type forms quorums from cycle 156 on, so a base
    // at 147 has none of it to need. B + 1 reads it non-indexed, 156 indexed.
    consensus.DIP0024Height = 150;
    consensus.DIP0024QuorumsHeight = 150;
    SyncNormal();
    for (int cycle{CYCLE_LENGTH}; cycle <= 180; cycle += CYCLE_LENGTH) {
        Mine(m_plain, cycle);
        if (cycle >= 156) {
            Mine(m_rotated, cycle);
            Mine(m_rotated, cycle + 1);
        }
    }
    const CBlockIndex* base{At(147)};
    const EvoViewToken seeded{Seed(Build(base), base)};
    for (int height{147}; height <= 157; ++height) {
        BOOST_CHECK_MESSAGE(QuorumRootAfter(m_node, seeded, At(height)) == QuorumRootAfter(m_node, Normal(), At(height)),
                            strprintf("quorum root after block %d", height));
    }
}

BOOST_FIXTURE_TEST_CASE(chainlock_coverage_starts_at_the_certified_height, EvoSnapshotChainSetup)
{
    // ChainLocks on the plain type, whose carried quorums at B = 179 were
    // mined at 146, 158 and 170. B + 1 may certify heights from
    // L = B - diff - 1 on, verified from start L - SIGN_HEIGHT_OFFSET with
    // a = 2 quorums. Two are eligible from cutoff 158 on, so start 158 is the
    // first covered one: diff 12 (L = 166) is covered, diff 13 (L = 165) not.
    SyncNormal();
    MineCycles(12, 180);
    const CBlockIndex* base{At(179)};
    const auto snapshot{Build(base)};
    m_restorer.Get().llmqTypeChainLocks = m_plain.type;
    const auto certified = [&](uint32_t diff) {
        auto carried{snapshot};
        CCbTx cbtx{*Assert(DecodeCbTxPayload(*carried.base_coinbase, /*legacy_bls_scheme=*/false))};
        CBLSSecretKey key;
        key.MakeNewKey();
        cbtx.bestCLSignature = key.Sign(H(3), /*specificLegacyScheme=*/false);
        cbtx.bestCLHeightDiff = diff;
        CMutableTransaction tx{*carried.base_coinbase};
        SetTxPayload(tx, cbtx);
        carried.base_coinbase = MakeTransactionRef(std::move(tx));
        return carried;
    };
    std::string error;
    BOOST_CHECK_MESSAGE(WITH_LOCK(::cs_main,
                                  return evo::CheckEvoSnapshotScanCoverage(certified(12), *m_node.chainman, base, error)),
                        error);
    BOOST_CHECK(
        !WITH_LOCK(::cs_main, return evo::CheckEvoSnapshotScanCoverage(certified(13), *m_node.chainman, base, error)));
    BOOST_CHECK_MESSAGE(error.find("scan of 2 from height 150") != std::string::npos, error);
}

BOOST_FIXTURE_TEST_CASE(asset_unlocks_signed_by_the_previous_platform_quorum, EvoSnapshotChainSetup)
{
    // The asset-unlock check at B + 1 accepts the active Platform quorums plus
    // the previous one: a + 1 = 3 scanned from B = 179, the carried 168, 156
    // and 144. A signature by 144 verifies; one by 132 is too old; a forged
    // one fails. Each outcome matches NORMAL.
    SyncNormal();
    MineCycles(12, 180);
    m_restorer.Get().llmqTypePlatform = m_plain.type;
    const CBlockIndex* base{At(179)};
    const EvoViewToken seeded{Seed(Build(base), base)};
    auto& qman{*m_node.llmq_ctx->qman};
    const uint256 msg_hash{H(4)};
    const auto verify = [&](const EvoViewToken& view, int quorum_height, const CBLSSecretKey& key) {
        const uint64_t index{7};
        const uint256 request_id{::SerializeHash(std::make_pair(ASSET_UNLOCK_REQUESTID_PREFIX, index))};
        const uint256 quorum_hash{At(quorum_height)->GetBlockHash()};
        const llmq::SignHash sign_hash{m_plain.type, quorum_hash, request_id, msg_hash};
        const CAssetUnlockPayload payload{CAssetUnlockPayload::CURRENT_VERSION,
                                          index,
                                          0,
                                          175,
                                          quorum_hash,
                                          key.Sign(sign_hash.Get(), /*specificLegacyScheme=*/false)};
        const EvoViewScope scope{view};
        TxValidationState state;
        const bool valid{payload.VerifySig(qman, msg_hash, base, state)};
        return valid ? std::string{"valid"} : state.GetRejectReason();
    };
    CBLSSecretKey forged;
    forged.MakeNewKey();
    BOOST_CHECK_EQUAL(verify(seeded, 144, m_quorum_keys.at({m_plain.type, 144})), "valid");
    BOOST_CHECK_EQUAL(verify(seeded, 132, m_quorum_keys.at({m_plain.type, 132})), "bad-assetunlock-too-old-quorum");
    BOOST_CHECK_EQUAL(verify(seeded, 144, forged), "bad-assetunlock-not-verified");
    for (const int height : {144, 132}) {
        const auto& key{m_quorum_keys.at({m_plain.type, height})};
        BOOST_CHECK_EQUAL(verify(seeded, height, key), verify(Normal(), height, key));
    }
}


BOOST_AUTO_TEST_SUITE_END()
