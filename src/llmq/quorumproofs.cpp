// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying file COPYING.
#include <algorithm>
#include <array>
#include <chain.h>
#include <chainlock/clsig.h>
#include <chainparams.h>
#include <consensus/merkle.h>
#include <evo/cbtx.h>
#include <evo/deterministicmns.h>
#include <evo/simplifiedmns.h>
#include <evo/specialtx.h>
#include <hash.h>
#include <limits>
#include <llmq/blockprocessor.h>
#include <llmq/params.h>
#include <llmq/quorumproofs.h>
#include <llmq/quorumsman.h>
#include <llmq/signhash.h>
#include <node/blockstorage.h>
#include <saltedhasher.h>
#include <set>
#include <shutdown.h>
#include <stdexcept>
#include <streams.h>
#include <sync.h>
#include <unordered_lru_cache.h>
#include <validation.h>

namespace llmq {
// Eight-byte, versioned wire identifier: DASH + Network Compact + format 02.
static constexpr std::array<char, 8> PROOF_MAGIC{'D', 'A', 'S', 'H', 'N', 'C', '0', '2'};

// Keys bind immutable verification inputs or an exact block history. Checkpoint
// membership, heights, ancestry and roots are still checked on every request.
// Explicit eviction thresholds bound memory without evicting on every insertion.
template <typename T, size_t Capacity>
class ProofCache
{
    Mutex m_mutex;
    Uint256LruHashMap<T, Capacity, Capacity + Capacity / 8> m_entries GUARDED_BY(m_mutex);

public:
    bool Get(const uint256& key, T& value) EXCLUSIVE_LOCKS_REQUIRED(!m_mutex)
    {
        LOCK(m_mutex);
        return m_entries.get(key, value);
    }

    void Insert(const uint256& key, const T& value) EXCLUSIVE_LOCKS_REQUIRED(!m_mutex)
    {
        LOCK(m_mutex);
        m_entries.insert(key, value);
    }

    void Clear() EXCLUSIVE_LOCKS_REQUIRED(!m_mutex)
    {
        LOCK(m_mutex);
        m_entries.clear();
    }
};

static void Require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

static void WriteBlob(CDataStream& out, const std::vector<unsigned char>& bytes)
{
    Require(bytes.size() <= MAX_PROOF_BYTES, "proof blob limit");
    out << uint32_t(bytes.size());
    out.write(AsBytes(Span{bytes}));
}

static std::vector<unsigned char> ReadBlob(CDataStream& in, size_t limit)
{
    uint32_t size;
    in >> size;
    Require(size <= limit && size <= in.size(), "proof blob length");
    std::vector<unsigned char> bytes(size);
    in.read(AsWritableBytes(Span{bytes}));
    return bytes;
}

static void WritePath(CDataStream& out, const ProofMerklePath& path)
{
    Require(path.siblings.size() <= 17, "proof path limit");
    out << path.index << path.count << uint8_t(path.siblings.size());
    for (const auto& hash : path.siblings) out << hash;
}

static ProofMerklePath ReadPath(CDataStream& in)
{
    ProofMerklePath path;
    uint8_t size;
    in >> path.index >> path.count >> size;
    Require(size <= 17 && size_t(size) * 32 <= in.size(), "proof path length");
    path.siblings.resize(size);
    for (auto& hash : path.siblings) in >> hash;
    return path;
}

static void WriteTransaction(CDataStream& out, const ProofTransaction& tx)
{
    Require(tx.transaction.size() <= 100000, "proof transaction limit");
    WriteBlob(out, tx.transaction);
    WritePath(out, tx.path);
}

static ProofTransaction ReadTransaction(CDataStream& in)
{
    ProofTransaction tx;
    tx.transaction = ReadBlob(in, 100000);
    tx.path = ReadPath(in);
    return tx;
}

static void WriteCertificate(CDataStream& out, const ProofCertificate& cert)
{
    out << cert.height << cert.header;
    cert.signature.Serialize(out, false);
}

static ProofCertificate ReadCertificate(CDataStream& in)
{
    ProofCertificate cert;
    in >> cert.height >> cert.header;
    cert.signature.Unserialize(in, false);
    return cert;
}

template <typename T> static std::vector<unsigned char> ConsensusBytes(const T& value)
{
    CDataStream out(SER_NETWORK, PROTOCOL_VERSION);
    out << value;
    return {UCharCast(out.data()), UCharCast(out.data()) + out.size()};
}

static CFinalCommitment ParseCommitment(const std::vector<unsigned char>& bytes, Consensus::LLMQType expectedType)
{
    Require(bytes.size() <= 1024, "commitment size");
    static ProofCache<CFinalCommitment, 4096> cache;
    const auto key = Hash(bytes);
    CFinalCommitment commitment;
    if (!cache.Get(key, commitment)) {
        CDataStream in(bytes, SER_NETWORK, PROTOCOL_VERSION);
        in >> commitment;
        Require(in.empty() && (commitment.nVersion == 3 || commitment.nVersion == 4) &&
                !commitment.IsNull() && commitment.quorumPublicKey.IsValid() &&
                !commitment.quorumHash.IsNull(), "invalid Basic BLS commitment");
        const auto params = std::find_if(Consensus::available_llmqs.begin(), Consensus::available_llmqs.end(),
                                         [&](const Consensus::LLMQParams& item) { return item.type == commitment.llmqType; });
        Require(params != Consensus::available_llmqs.end(), "unsupported quorum type");
        Require((commitment.nVersion == 4) == (int(commitment.llmqType) == 5) &&
                commitment.quorumIndex >= 0 && commitment.quorumIndex < 32 &&
                commitment.signers.size() == size_t(params->size) && commitment.validMembers.size() == size_t(params->size) &&
                commitment.CountSigners() >= params->threshold && commitment.CountValidMembers() >= params->threshold &&
                ConsensusBytes(commitment) == bytes, "noncanonical or undersized commitment");
        cache.Insert(key, commitment);
    }
    Require(commitment.llmqType == expectedType, "unexpected quorum type");
    return commitment;
}

static CTransactionRef ParseTransaction(const std::vector<unsigned char>& bytes)
{
    Require(bytes.size() != 64 && bytes.size() <= 100000, "transaction size");
    CDataStream in(bytes, SER_NETWORK, PROTOCOL_VERSION);
    CMutableTransaction tx;
    in >> tx;
    Require(in.empty(), "trailing transaction data");
    return MakeTransactionRef(tx);
}

/** The fields Verify needs from a mined commitment transaction. Parsing the
 *  payload decodes the commitment's BLS key and signatures; the result is a pure
 *  function of the transaction bytes, so it is memoized across verifications. */
struct MiningPayload {
    bool envelope_ok{false};
    bool payload_ok{false};
    uint16_t version{0};
    uint32_t height{0};
    std::vector<unsigned char> commitment;
};

static MiningPayload ParseMiningPayload(const std::vector<unsigned char>& bytes)
{
    const auto key = Hash(bytes);
    static ProofCache<MiningPayload, 4096> cache;
    MiningPayload result;
    if (cache.Get(key, result)) return result;
    const auto tx = ParseTransaction(bytes);
    result.envelope_ok = tx->IsSpecialTxVersion() && tx->nType == TRANSACTION_QUORUM_COMMITMENT;
    if (result.envelope_ok) {
        if (auto payload = GetTxPayload<CFinalCommitmentTxPayload>(*tx)) {
            auto commitment = ConsensusBytes(payload->commitment);
            // ParseCommitment rejects these anyway; do not let a caller-supplied
            // proof park up to ~100 KB per entry in this cache first.
            if (commitment.size() <= 1024) {
                result.payload_ok = true;
                result.version = payload->nVersion;
                result.height = payload->nHeight;
                result.commitment = std::move(commitment);
            }
        }
    }
    cache.Insert(key, result);
    return result;
}

/** Coinbase envelope and the payload fields Verify reads, memoized by transaction bytes. */
struct CoinbasePayload {
    bool envelope_ok{false};
    bool payload_ok{false};
    CCbTx::Version version{CCbTx::Version::INVALID};
    int32_t height{0};
    uint32_t best_cl_height_diff{0};
    uint256 merkle_root_mn_list;
    uint256 merkle_root_quorums;
};

static CoinbasePayload ParseCoinbasePayload(const std::vector<unsigned char>& bytes)
{
    const auto key = Hash(bytes);
    static ProofCache<CoinbasePayload, 1024> cache;
    CoinbasePayload result;
    if (cache.Get(key, result)) return result;
    const auto tx = ParseTransaction(bytes);
    result.envelope_ok = tx->IsCoinBase() && tx->IsSpecialTxVersion() && tx->nType == TRANSACTION_COINBASE &&
                         !tx->vin[0].scriptSig.empty() && tx->vin[0].scriptSig.size() <= 100 && !tx->vout.empty();
    if (result.envelope_ok) {
        if (const auto payload = GetTxPayload<CCbTx>(*tx)) {
            result.payload_ok = true;
            result.version = payload->nVersion;
            result.height = payload->nHeight;
            result.best_cl_height_diff = payload->bestCLHeightDiff;
            result.merkle_root_mn_list = payload->merkleRootMNList;
            result.merkle_root_quorums = payload->merkleRootQuorums;
        }
    }
    cache.Insert(key, result);
    return result;
}

static CoinbasePayload Coinbase(const ProofTransaction& proof, const ProofCertificate& cert)
{
    Require(proof.path.index == 0 && proof.Verify(cert.header), "coinbase inclusion");
    const auto payload = ParseCoinbasePayload(proof.transaction);
    Require(payload.envelope_ok, "coinbase envelope");
    Require(payload.payload_ok && payload.version >= CCbTx::Version::CLSIG_AND_BALANCE &&
                payload.height == int64_t(cert.height) && payload.best_cl_height_diff < cert.height &&
                !payload.merkle_root_quorums.IsNull(),
            "coinbase payload");
    return payload;
}

bool ProofMerklePath::Verify(uint256 leaf, const uint256& root) const
{
    if (count == 0 || count > 100000 || index >= count || siblings.size() > 17) return false;
    uint32_t width = count;
    uint32_t position = index;
    for (const auto& sibling : siblings) {
        if (width <= 1) return false;
        const bool duplicate = (position ^ 1U) >= width;
        if (duplicate ? sibling != leaf : sibling == leaf) return false;
        leaf = position & 1 ? Hash(sibling, leaf) : Hash(leaf, sibling);
        position >>= 1;
        width = width / 2 + width % 2;
    }
    return width == 1 && position == 0 && leaf == root;
}

ProofMerklePath ProofMerklePath::Build(const std::vector<uint256>& leaves, uint32_t index)
{
    Require(index < leaves.size() && leaves.size() <= std::numeric_limits<uint32_t>::max(), "merkle index");
    ProofMerklePath path{index, uint32_t(leaves.size()), {}};
    auto layer = leaves;
    while (layer.size() > 1) {
        path.siblings.push_back(layer[std::min(size_t(index ^ 1U), layer.size() - 1)]);
        if (layer.size() & 1) layer.push_back(layer.back());
        for (size_t i = 0; i < layer.size(); i += 2) layer[i / 2] = Hash(layer[i], layer[i + 1]);
        layer.resize(layer.size() / 2);
        index >>= 1;
    }
    Require(path.Verify(leaves[path.index], layer[0]), "mutated merkle tree");
    return path;
}

bool ProofTransaction::Verify(const CBlockHeader& header) const
{
    return transaction.size() != 64 && transaction.size() <= 100000 && path.Verify(Hash(transaction), header.hashMerkleRoot);
}

ProofTransaction ProofTransaction::Build(const CBlock& block, uint32_t index)
{
    Require(index < block.vtx.size(), "transaction index");
    std::vector<uint256> hashes;
    hashes.reserve(block.vtx.size());
    for (const auto& tx : block.vtx) hashes.push_back(tx->GetHash());
    ProofTransaction proof{ConsensusBytes(*block.vtx[index]), ProofMerklePath::Build(hashes, index)};
    Require(proof.Verify(block.GetBlockHeader()), "transaction root mismatch");
    return proof;
}

bool ProofCertificate::Verify(const CFinalCommitment& signer, uint32_t minimum, Consensus::LLMQType kind) const
{
    if (height <= minimum || height > INT32_MAX || signer.llmqType != kind || !signature.IsValid()) return false;
    SignHash hash{kind, signer.quorumHash, chainlock::GenSigRequestId(height), header.GetHash()};
    // Bind every input to Basic BLS verification, including the public key and
    // signature themselves. A quorum hash or signed message alone is insufficient.
    CHashWriter writer(SER_GETHASH, 0);
    writer << hash.Get();
    signer.quorumPublicKey.Serialize(writer, false);
    signature.Serialize(writer, false);
    const auto key = writer.GetHash();
    static ProofCache<bool, 16384> cache;
    bool valid{false};
    if (cache.Get(key, valid)) return valid;
    if (!signature.VerifyInsecure(signer.quorumPublicKey, hash.Get(), false)) return false;
    cache.Insert(key, true);
    return true;
}

std::vector<unsigned char> QuorumProofChain::Encode() const
{
    Require(links.size() < MAX_PROOF_CERTIFICATES, "certificate limit");
    CDataStream out(SER_NETWORK, PROTOCOL_VERSION);
    out.write(AsBytes(Span{PROOF_MAGIC}));
    out << anchor;
    WriteBlob(out, seed);
    WritePath(out, seedPath);
    out << uint16_t(links.size());
    size_t remaining = MAX_PROOF_HEADERS;
    for (const auto& link : links) {
        Require(link.ancestors.size() <= remaining, "total ancestor limit");
        remaining -= link.ancestors.size();
        WriteCertificate(out, link.certificate);
        WriteTransaction(out, link.mining);
        out << uint16_t(link.ancestors.size());
        for (const auto& header : link.ancestors) out << header;
        Require(out.size() <= MAX_PROOF_BYTES, "proof size limit");
    }
    WriteCertificate(out, target);
    WriteTransaction(out, coinbase);
    Require(out.size() <= MAX_PROOF_BYTES, "proof size limit");
    return {UCharCast(out.data()), UCharCast(out.data()) + out.size()};
}

QuorumProofChain QuorumProofChain::Decode(const std::vector<unsigned char>& bytes)
{
    Require(bytes.size() <= MAX_PROOF_BYTES, "proof size limit");
    CDataStream in(bytes, SER_NETWORK, PROTOCOL_VERSION);
    std::array<char, 8> magic;
    in.read(AsWritableBytes(Span{magic}));
    Require(magic == PROOF_MAGIC, "proof version");
    QuorumProofChain proof;
    in >> proof.anchor;
    proof.seed = ReadBlob(in, 1024);
    proof.seedPath = ReadPath(in);
    uint16_t count;
    in >> count;
    Require(count < MAX_PROOF_CERTIFICATES, "certificate limit");
    size_t remaining = MAX_PROOF_HEADERS;
    for (size_t i = 0; i < count; ++i) {
        ProofLink link;
        link.certificate = ReadCertificate(in);
        link.mining = ReadTransaction(in);
        uint16_t headers;
        in >> headers;
        Require(headers <= remaining && size_t(headers) * 80 <= in.size(), "ancestor length/budget");
        remaining -= headers;
        link.ancestors.resize(headers);
        for (auto& header : link.ancestors) in >> header;
        proof.links.push_back(std::move(link));
    }
    proof.target = ReadCertificate(in);
    proof.coinbase = ReadTransaction(in);
    Require(in.empty(), "trailing proof data");
    return proof;
}

ProofState QuorumProofChain::Verify(const ProofState& trusted) const
{
    Require(anchor == trusted && anchor.height >= (anchor.network == 0 ? 1987776U : 905100U) &&
                anchor.height <= INT32_MAX && !anchor.blockHash.IsNull() && !anchor.quorumRoot.IsNull(),
            "untrusted snapshot");
    // The application fixes the network and initial roots; the relay cannot choose them.
    Require(anchor.network <= 1, "unsupported proof network");
    const auto kind = anchor.network == 0 ? Consensus::LLMQType::LLMQ_400_60 : Consensus::LLMQType::LLMQ_50_60;
    auto signer = ParseCommitment(seed, kind);
    Require(seedPath.Verify(Hash(seed), anchor.quorumRoot), "seed membership");
    Require(links.size() < MAX_PROOF_CERTIFICATES, "certificate limit");
    uint32_t height = anchor.height;
    size_t remaining = MAX_PROOF_HEADERS;
    for (const auto& link : links) {
        Require(link.certificate.Verify(signer, height, kind), "ChainLock signature/height");
        Require(link.ancestors.size() <= remaining && link.ancestors.size() < link.certificate.height, "ancestor budget/height");
        remaining -= link.ancestors.size();
        const CBlockHeader* descendant = &link.certificate.header;
        for (auto it = link.ancestors.rbegin(); it != link.ancestors.rend(); ++it) {
            Require(descendant->hashPrevBlock == it->GetHash(), "ancestor continuity");
            descendant = &*it;
        }
        Require(link.mining.path.index != 0 && link.mining.Verify(*descendant), "mining inclusion");
        const auto mining = ParseMiningPayload(link.mining.transaction);
        Require(mining.envelope_ok, "quorum transaction envelope");
        Require(mining.payload_ok && mining.version == 1 &&
                    mining.height == link.certificate.height - link.ancestors.size(),
                "quorum mining height");
        auto next = ParseCommitment(mining.commitment, kind);
        Require(next.quorumHash != signer.quorumHash, "redundant signer");
        signer = std::move(next);
        height = link.certificate.height;
    }
    Require(target.Verify(signer, height, kind), "final ChainLock signature/height");
    auto cb = Coinbase(coinbase, target);
    return {anchor.network, target.height, target.header.GetHash(), cb.merkle_root_mn_list, cb.merkle_root_quorums};
}

std::vector<unsigned char> EncodeBootstrap(const QuorumProofChain& proof, const std::vector<ProofProjection>& records)
{
    return EncodeBootstrap(proof, proof.Verify(proof.anchor), records);
}

std::vector<unsigned char> EncodeBootstrap(const QuorumProofChain& proof, const ProofState& state,
                                           const std::vector<ProofProjection>& records)
{
    Require(!records.empty() && records.size() <= 16, "projection count");
    // No BLS work: this re-reads only the target coinbase the proof carries.
    const auto cb = Coinbase(proof.coinbase, proof.target);
    Require(state.network == proof.anchor.network && state.height == proof.target.height &&
                state.blockHash == proof.target.header.GetHash() && state.masternodeRoot == cb.merkle_root_mn_list &&
                state.quorumRoot == cb.merkle_root_quorums,
            "state does not match proof target");
    CDataStream out(SER_NETWORK, PROTOCOL_VERSION);
    WriteBlob(out, proof.Encode());
    out << uint8_t(records.size());
    for (const auto& record : records) {
        Require(record.kind <= 1 && !record.leaf.empty() && record.leaf.size() != 64 && record.leaf.size() <= 4096 &&
                record.path.Verify(Hash(record.leaf), record.kind == 0 ? state.quorumRoot : state.masternodeRoot), "projection inclusion");
        out << record.kind;
        WriteBlob(out, record.leaf);
        WritePath(out, record.path);
    }
    Require(out.size() <= MAX_PROOF_BYTES, "bootstrap size limit");
    return {UCharCast(out.data()), UCharCast(out.data()) + out.size()};
}

UniValue ProofState::ToJson() const
{
    UniValue value(UniValue::VOBJ);
    value.pushKV("network", network);
    value.pushKV("height", height);
    value.pushKV("block_hash", blockHash.ToString());
    value.pushKV("masternode_root", masternodeRoot.ToString());
    value.pushKV("quorum_root", quorumRoot.ToString());
    return value;
}

ProofState ProofState::FromJson(const UniValue& value)
{
    Require(value.isObject() && value.exists("network") && value.exists("height") &&
                value.exists("block_hash") && value.exists("masternode_root") && value.exists("quorum_root"),
            "snapshot fields missing");
    ProofState state;
    state.network = value["network"].getInt<uint8_t>();
    state.height = value["height"].getInt<uint32_t>();
    auto parse = [&](const char* name) {
        const auto text = value[name].get_str();
        Require(text.size() == 64 && IsHex(text), "snapshot hash encoding");
        return uint256S(text);
    };
    state.blockHash = parse("block_hash");
    state.masternodeRoot = parse("masternode_root");
    state.quorumRoot = parse("quorum_root");
    return state;
}

std::vector<CFinalCommitment> QuorumProofBuilder::ActiveCommitments(const CBlockIndex* index) const
{
    Require(index != nullptr, "missing block index");
    // The active set at a block is a function of that block's history, and it is
    // checked against the block's own quorum root below before it is kept. Every
    // proof from one checkpoint needs the same set, as does every quorum record at
    // one target. Reading it decodes each commitment's BLS key and signatures.
    static ProofCache<std::vector<CFinalCommitment>, 64> sets;
    std::vector<CFinalCommitment> result;
    if (sets.Get(index->GetBlockHash(), result)) return result;
    {
        LOCK(cs_main);
        for (const auto& [type, indexes] : m_quorum_block_processor.GetMinedAndActiveCommitmentsUntilBlock(index)) {
            for (const auto* base : indexes) {
                auto [commitment, mined] = m_quorum_block_processor.GetMinedCommitment(type, base->GetBlockHash());
                Require(!commitment.IsNull() && !mined.IsNull(), "missing active commitment");
                result.push_back(std::move(commitment));
            }
        }
    }
    std::vector<std::pair<uint256, size_t>> order;
    order.reserve(result.size());
    for (size_t i = 0; i < result.size(); ++i)
        order.emplace_back(SerializeHash(result[i]), i);
    std::sort(order.begin(), order.end());
    std::vector<CFinalCommitment> sorted;
    std::vector<uint256> hashes;
    sorted.reserve(result.size());
    hashes.reserve(result.size());
    for (const auto& [hash, i] : order) {
        sorted.push_back(std::move(result[i]));
        hashes.push_back(hash);
    }
    bool mutated{false};
    Require(ComputeMerkleRoot(hashes, &mutated) == StateAt(index).quorumRoot && !mutated,
            "active quorum root mismatch");
    sets.Insert(index->GetBlockHash(), sorted);
    return sorted;
}

static ProofCache<ProofState, 256>& StateCache()
{
    static ProofCache<ProofState, 256> cache;
    return cache;
}

void ClearProofStateCacheForTesting() { StateCache().Clear(); }

ProofState QuorumProofBuilder::StateAt(const CBlockIndex* index)
{
    Require(index != nullptr && index->nHeight > 0, "invalid checkpoint block");
    const auto network = Params().NetworkIDString();
    Require(network == "main" || network == "test", "proofs support mainnet/testnet");
    // A block hash fixes the coinbase, and the network is fixed for the process.
    // Every request reads the checkpoint's state and the target's more than once.
    ProofState cached;
    if (StateCache().Get(index->GetBlockHash(), cached)) return cached;
    CBlock block;
    Require(node::ReadBlockFromDisk(block, index, Params().GetConsensus()) && !block.vtx.empty(), "historical block unavailable");
    ProofCertificate cert{uint32_t(index->nHeight), block.GetBlockHeader(), {}};
    auto cb = Coinbase(ProofTransaction::Build(block, 0), cert);
    const ProofState state{uint8_t(network == "main" ? 0 : 1), uint32_t(index->nHeight), index->GetBlockHash(),
                           cb.merkle_root_mn_list, cb.merkle_root_quorums};
    StateCache().Insert(index->GetBlockHash(), state);
    return state;
}

MasternodeLeaves MasternodeLeavesAt(CDeterministicMNManager& dmnman, const CBlockIndex* index)
{
    Require(index != nullptr, "missing block index");
    // Every EvoNode record at one target opens the same list, and hashing its
    // entries costs more than the lookup. The list is kept only once it hashes
    // to the block's own masternode root.
    static ProofCache<MasternodeLeaves, 8> cache;
    MasternodeLeaves result;
    if (cache.Get(index->GetBlockHash(), result)) return result;
    const auto list = WITH_LOCK(cs_main, return dmnman.GetListForBlock(index));
    result.sml = list.to_sml();
    result.leaves.reserve(result.sml->mnList.size());
    for (const auto& entry : result.sml->mnList)
        result.leaves.push_back(entry->CalcHash());
    const auto root = QuorumProofBuilder::StateAt(index).masternodeRoot;
    bool mutated{false};
    Require(ComputeMerkleRoot(result.leaves, &mutated) == root && !mutated, "masternode root mismatch");
    cache.Insert(index->GetBlockHash(), result);
    return result;
}

std::optional<CFinalCommitment> QuorumProofBuilder::DetermineChainlockSigningCommitment(int32_t height) const
{
    const auto params = Params().GetLLMQ(Params().GetConsensus().llmqTypeChainLocks);
    if (!params || height < SIGN_HEIGHT_OFFSET) return std::nullopt;
    const auto* start = m_tip->GetAncestor(height - SIGN_HEIGHT_OFFSET);
    if (!start) return std::nullopt;
    // This hash commits to the entire selection history, including its branch.
    // The type and request height fix the remaining signing-selection inputs.
    const auto key = (CHashWriter(SER_GETHASH, 0) << start->GetBlockHash() << params->type << height).GetHash();
    // Two levels: every candidate height needs an entry, but thousands of heights
    // share a few dozen commitments. Heights map to a commitment's content hash,
    // which cannot alias across branches, and each commitment is stored once.
    // A year of testnet (hourly quorums) is ~80k candidate heights.
    static ProofCache<uint256, 131072> selected;
    static ProofCache<CFinalCommitment, 4096> commitments;
    uint256 commitment_hash;
    CFinalCommitment cached;
    if (selected.Get(key, commitment_hash) && commitments.Get(commitment_hash, cached)) return cached;
    std::optional<CFinalCommitment> result;
    {
        LOCK(cs_main);
        // The commitment database follows the active chain. Do not populate a
        // snapshot's cache from another branch if a reorg raced this request.
        Require(m_chainman.ActiveChain().Contains(start), "proof chain changed during construction");
        result = SelectCommitmentForSigning(*params, m_qman, chainlock::GenSigRequestId(height), start);
    }
    if (result) {
        commitment_hash = ::SerializeHash(*result);
        commitments.Insert(commitment_hash, *result);
        selected.Insert(key, commitment_hash);
    }
    return result;
}

static bool OnBranch(const CBlockIndex* tip, const CBlockIndex* index)
{
    return tip->GetAncestor(index->nHeight) == index;
}

static const CBlockIndex* MinedCommitmentBlock(const CQuorumBlockProcessor& processor, const CBlockIndex* tip,
                                               const node::BlockManager& blocks, Consensus::LLMQType kind,
                                               const uint256& quorum)
{
    const auto key = (CHashWriter(SER_GETHASH, 0) << kind << quorum).GetHash();
    static ProofCache<uint256, 4096> cache;
    uint256 mined_hash;
    LOCK(cs_main);
    // A quorum can be mined differently after a reorg. Reuse an entry only if
    // its exact mining block is an ancestor of this request's tip.
    if (cache.Get(key, mined_hash)) {
        const auto* mined = blocks.LookupBlockIndex(mined_hash);
        if (mined && OnBranch(tip, mined)) return mined;
    }
    const auto result = processor.GetMinedCommitment(kind, quorum);
    const auto* mined = blocks.LookupBlockIndex(result.second);
    if (result.first.IsNull() || !mined || !OnBranch(tip, mined)) return nullptr;
    cache.Insert(key, result.second);
    return mined;
}

/** The mining transaction of one quorum commitment, with its Merkle path.
 *  Every proof whose route passes through a quorum needs this exact result; it
 *  depends only on the mining block's contents, so it is memoized by block hash.
 *  A miss reads the block and scans its commitments by type and quorum hash
 *  before fully deserializing any payload (a mining block holds dozens of
 *  commitments, each carrying BLS points that are costly to decode). */
static ProofTransaction MiningTransaction(const CBlockIndex* mined, Consensus::LLMQType kind, const uint256& quorum)
{
    const auto key = (CHashWriter(SER_GETHASH, 0) << mined->GetBlockHash() << kind << quorum).GetHash();
    static ProofCache<ProofTransaction, 2048> cache;
    ProofTransaction cached;
    if (cache.Get(key, cached)) return cached;
    CBlock block;
    Require(node::ReadBlockFromDisk(block, mined, Params().GetConsensus()), "mining block unavailable");
    for (size_t i = 1; i < block.vtx.size(); ++i) {
        const auto& tx = *block.vtx[i];
        if (tx.nType != TRANSACTION_QUORUM_COMMITMENT) continue;
        // CFinalCommitmentTxPayload: nVersion u16, nHeight u32, then the commitment,
        // which starts with nVersion u16, llmqType u8, quorumHash. Compare the
        // prefix first; only a candidate match pays for full deserialization.
        const auto& raw = tx.vExtraPayload;
        if (raw.size() < 2 + 4 + 2 + 1 + 32 || raw[2 + 4 + 2] != uint8_t(kind) ||
            !std::equal(quorum.begin(), quorum.end(), raw.begin() + 2 + 4 + 2 + 1)) {
            continue;
        }
        const auto payload = GetTxPayload<CFinalCommitmentTxPayload>(tx);
        if (payload && !payload->commitment.IsNull() && payload->commitment.llmqType == kind &&
            payload->commitment.quorumHash == quorum) {
            auto proof = ProofTransaction::Build(block, i);
            cache.Insert(key, proof);
            return proof;
        }
    }
    throw std::runtime_error("mining transaction unavailable");
}

std::optional<QuorumProofChain> QuorumProofBuilder::Build(const CBlockIndex* checkpoint,
                                                          const chainlock::ChainLockSig& target) const
{
    const auto& blocks = m_chainman.m_blockman;
    const auto* targetIndex = m_tip->GetAncestor(target.getHeight());
    Require(checkpoint && targetIndex && OnBranch(m_tip, checkpoint) && checkpoint->nHeight < targetIndex->nHeight,
            "checkpoint/target not on chain");
    Require(targetIndex->GetBlockHash() == target.getBlockHash(), "target ChainLock block mismatch");
    auto certificate = [&](const chainlock::CoinbaseChainLock& entry) {
        return ProofCertificate{uint32_t(entry.height), m_tip->GetAncestor(entry.height)->GetBlockHeader(),
                                entry.Signed().getSig()};
    };
    QuorumProofChain proof;
    proof.anchor = StateAt(checkpoint);
    auto seeds = ActiveCommitments(checkpoint);
    std::set<uint256> seedHashes;
    std::vector<uint256> seedLeaves;
    const auto kind = Params().GetConsensus().llmqTypeChainLocks;
    for (const auto& seed : seeds) {
        seedLeaves.push_back(SerializeHash(seed));
        if (seed.llmqType == kind && seed.nVersion >= 3) seedHashes.insert(seed.quorumHash);
    }
    proof.target = {uint32_t(target.getHeight()), targetIndex->GetBlockHeader(), target.getSig()};
    auto needed = DetermineChainlockSigningCommitment(targetIndex->nHeight);
    Require(needed.has_value(), "target signer unavailable");
    int32_t later = targetIndex->nHeight;
    size_t remaining = MAX_PROOF_HEADERS;
    while (!seedHashes.count(needed->quorumHash)) {
        Require(proof.links.size() < MAX_PROOF_CERTIFICATES - 1, "certificate budget exhausted");
        Require(!ShutdownRequested(), "proof construction interrupted");
        const auto* mined = MinedCommitmentBlock(m_quorum_block_processor, m_tip, blocks, kind, needed->quorumHash);
        Require(mined != nullptr, "mining block unavailable");
        if (mined->nHeight <= checkpoint->nHeight) return std::nullopt;
        const auto mining = MiningTransaction(mined, kind, needed->quorumHash);
        std::optional<ProofLink> best;
        std::optional<CFinalCommitment> predecessor;
        double bestScore = std::numeric_limits<double>::infinity();
        // Exact bytes/progress over nearby candidates. Expand the search only
        // when the near window cannot bridge; the verifier does not depend on this heuristic.
        const int32_t maximum = std::min(later - 1, mined->nHeight + int32_t(remaining));
        for (int32_t height = mined->nHeight; height <= maximum; ++height) {
            if (height > mined->nHeight + 8 && best) break;
            const auto entry = m_chainlocks.Find(height, maximum);
            if (!entry) break;
            height = entry->height;
            if (height > mined->nHeight + 8 && best) break;
            auto signer = DetermineChainlockSigningCommitment(height);
            if (!signer || signer->quorumHash == needed->quorumHash) continue;
            const auto* previous = MinedCommitmentBlock(m_quorum_block_processor, m_tip, blocks, kind, signer->quorumHash);
            if (!previous || previous->nHeight >= mined->nHeight || (previous->nHeight <= checkpoint->nHeight && !seedHashes.count(signer->quorumHash))) continue;
            auto cert = certificate(*entry);
            if (!cert.Verify(*signer, checkpoint->nHeight, kind)) continue;
            const auto cost = 180 + 4 + mining.transaction.size() + 9 + 32 * mining.path.siblings.size() + 2 +
                              80 * (height - mined->nHeight);
            const double score = double(cost) / (mined->nHeight - std::max(checkpoint->nHeight, previous->nHeight));
            if (score >= bestScore) continue;
            bestScore = score;
            best = ProofLink{cert, mining, {}};
            predecessor = std::move(signer);
        }
        Require(best && predecessor, "no B-only route within proof budget");
        for (int32_t h = mined->nHeight; h < int64_t(best->certificate.height); ++h) {
            best->ancestors.push_back(m_tip->GetAncestor(h)->GetBlockHeader());
        }
        remaining -= best->ancestors.size();
        later = best->certificate.height;
        needed = std::move(predecessor);
        proof.links.push_back(std::move(*best));
    }
    std::reverse(proof.links.begin(), proof.links.end());
    for (size_t i = 0; i < seeds.size(); ++i) {
        if (seeds[i].llmqType == kind && seeds[i].quorumHash == needed->quorumHash) {
            proof.seed = ConsensusBytes(seeds[i]);
            proof.seedPath = ProofMerklePath::Build(seedLeaves, i);
            break;
        }
    }
    CBlock finalBlock;
    Require(node::ReadBlockFromDisk(finalBlock, targetIndex, Params().GetConsensus()), "final block unavailable");
    proof.coinbase = ProofTransaction::Build(finalBlock, 0);
    Require(proof.Verify(proof.anchor) == StateAt(targetIndex), "constructed proof does not match chain");
    proof.Encode();
    return proof;
}

} // namespace llmq
