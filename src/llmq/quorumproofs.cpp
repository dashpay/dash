// Copyright (c) 2025-2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying file COPYING.
#include <algorithm>
#include <chain.h>
#include <chainlock/clsig.h>
#include <chainparams.h>
#include <consensus/merkle.h>
#include <evo/cbtx.h>
#include <evo/specialtx.h>
#include <hash.h>
#include <limits>
#include <llmq/blockprocessor.h>
#include <llmq/quorumproofs.h>
#include <llmq/quorumsman.h>
#include <llmq/signhash.h>
#include <node/blockstorage.h>
#include <set>
#include <shutdown.h>
#include <stdexcept>
#include <streams.h>
#include <sync.h>
#include <unordered_lru_cache.h>
#include <validation.h>

namespace llmq {
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

static CFinalCommitment ParseCommitment(const std::vector<unsigned char>& bytes)
{
    Require(bytes.size() <= 1024, "commitment size");
    static ProofCache<CFinalCommitment, 4096> cache;
    const auto key = Hash(bytes);
    CFinalCommitment commitment;
    if (cache.Get(key, commitment)) return commitment;
    CDataStream in(bytes, SER_NETWORK, PROTOCOL_VERSION);
    in >> commitment;
    Require(in.empty() && (commitment.nVersion == 3 || commitment.nVersion == 4) &&
            !commitment.IsNull() && commitment.quorumPublicKey.IsValid() &&
            !commitment.quorumHash.IsNull(), "invalid Basic BLS commitment");
    int size{0}, threshold{0};
    switch (int(commitment.llmqType)) {
    case 1: size = 50; threshold = 30; break;
    case 2: size = 400; threshold = 240; break;
    case 3: size = 400; threshold = 340; break;
    case 4: size = 100; threshold = 67; break;
    case 5: size = 60; threshold = 45; break;
    case 6: size = 25; threshold = 17; break;
    default: throw std::runtime_error("unsupported quorum type");
    }
    Require((commitment.nVersion == 4) == (int(commitment.llmqType) == 5) &&
            commitment.quorumIndex >= 0 && commitment.quorumIndex < 32 &&
            commitment.signers.size() == size_t(size) && commitment.validMembers.size() == size_t(size) &&
            commitment.CountSigners() >= threshold && commitment.CountValidMembers() >= threshold &&
            ConsensusBytes(commitment) == bytes, "noncanonical or undersized commitment");
    cache.Insert(key, commitment);
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

static CCbTx Coinbase(const ProofTransaction& proof, const ProofCertificate& cert)
{
    Require(proof.path.index == 0 && proof.Verify(cert.header), "coinbase inclusion");
    const auto tx = ParseTransaction(proof.transaction);
    Require(tx->IsCoinBase() && tx->nVersion == 3 && tx->nType == TRANSACTION_COINBASE &&
            !tx->vin[0].scriptSig.empty() && tx->vin[0].scriptSig.size() <= 100 &&
            !tx->vout.empty() && tx->vout.size() <= 4096, "coinbase envelope");
    auto payload = GetTxPayload<CCbTx>(*tx);
    Require(payload && payload->nVersion == CCbTx::Version::CLSIG_AND_BALANCE &&
            payload->nHeight == int64_t(cert.height) && payload->bestCLHeightDiff < cert.height &&
            !payload->merkleRootQuorums.IsNull(), "coinbase payload");
    return *payload;
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
    Require(anchor == trusted && anchor.height > (anchor.network == 0 ? 1987776U : 905100U) && anchor.height <= INT32_MAX &&
            !anchor.blockHash.IsNull() && !anchor.quorumRoot.IsNull(), "untrusted snapshot");
    // The application fixes the network and initial roots; the relay cannot choose them.
    Require(anchor.network <= 1, "unsupported proof network");
    const auto kind = anchor.network == 0 ? Consensus::LLMQType::LLMQ_400_60 : Consensus::LLMQType::LLMQ_50_60;
    auto signer = ParseCommitment(seed);
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
        auto tx = ParseTransaction(link.mining.transaction);
        Require(tx->nVersion == 3 && tx->nType == TRANSACTION_QUORUM_COMMITMENT &&
                tx->vin.empty() && tx->vout.empty() && tx->nLockTime == 0, "quorum transaction envelope");
        auto payload = GetTxPayload<CFinalCommitmentTxPayload>(*tx);
        Require(payload && payload->nVersion == 1 && payload->nHeight == link.certificate.height - link.ancestors.size(), "quorum mining height");
        auto next = ParseCommitment(ConsensusBytes(payload->commitment));
        Require(next.quorumHash != signer.quorumHash, "redundant signer");
        signer = std::move(next);
        height = link.certificate.height;
    }
    Require(target.Verify(signer, height, kind), "final ChainLock signature/height");
    auto cb = Coinbase(coinbase, target);
    return {anchor.network, target.height, target.header.GetHash(), cb.merkleRootMNList, cb.merkleRootQuorums};
}

std::vector<unsigned char> EncodeBootstrap(const QuorumProofChain& proof, const std::vector<ProofProjection>& records)
{
    Require(!records.empty() && records.size() <= 16, "projection count");
    const auto state = proof.Verify(proof.anchor);
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
    std::vector<CFinalCommitment> result;
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
    std::sort(result.begin(), result.end(), [](const auto& a, const auto& b) { return SerializeHash(a) < SerializeHash(b); });
    std::vector<uint256> hashes;
    for (const auto& commitment : result) hashes.push_back(SerializeHash(commitment));
    Require(ComputeMerkleRoot(hashes) == StateAt(index).quorumRoot, "active quorum root mismatch");
    return result;
}

ProofState QuorumProofBuilder::StateAt(const CBlockIndex* index)
{
    Require(index != nullptr && index->nHeight > 0, "invalid checkpoint block");
    const auto network = Params().NetworkIDString();
    Require(network == "main" || network == "test", "proofs support mainnet/testnet");
    CBlock block;
    Require(node::ReadBlockFromDisk(block, index, Params().GetConsensus()) && !block.vtx.empty(), "historical block unavailable");
    ProofCertificate cert{uint32_t(index->nHeight), block.GetBlockHeader(), {}};
    auto cb = Coinbase(ProofTransaction::Build(block, 0), cert);
    return {uint8_t(network == "main" ? 0 : 1), uint32_t(index->nHeight), index->GetBlockHash(), cb.merkleRootMNList, cb.merkleRootQuorums};
}

std::optional<CFinalCommitment> QuorumProofBuilder::DetermineChainlockSigningCommitment(int32_t height) const
{
    const auto params = Params().GetLLMQ(Params().GetConsensus().llmqTypeChainLocks);
    if (!params || height < SIGN_HEIGHT_OFFSET) return std::nullopt;
    const auto* start = m_chain[height - SIGN_HEIGHT_OFFSET];
    if (!start) return std::nullopt;
    // This hash commits to the entire selection history, including its branch.
    // The type and request height fix the remaining signing-selection inputs.
    const auto key = (CHashWriter(SER_GETHASH, 0) << start->GetBlockHash() << params->type << height).GetHash();
    static ProofCache<CFinalCommitment, 8192> cache;
    CFinalCommitment cached;
    if (cache.Get(key, cached)) return cached;
    LOCK(cs_main);
    // The commitment database follows the active chain. Do not populate a
    // snapshot's cache from another branch if a reorg raced this request.
    Require(m_chainman.ActiveChain().Contains(start), "proof chain changed during construction");
    auto result = SelectCommitmentForSigning(*params, m_chain, m_qman, chainlock::GenSigRequestId(height), height,
                                             SIGN_HEIGHT_OFFSET);
    if (result) cache.Insert(key, *result);
    return result;
}

static const CBlockIndex* MinedCommitmentBlock(const CQuorumBlockProcessor& processor, const CChain& chain,
                                               const node::BlockManager& blocks, Consensus::LLMQType kind,
                                               const uint256& quorum)
{
    const auto key = (CHashWriter(SER_GETHASH, 0) << kind << quorum).GetHash();
    static ProofCache<uint256, 4096> cache;
    uint256 mined_hash;
    LOCK(cs_main);
    // A quorum can be mined differently after a reorg. Reuse an entry only if
    // its exact mining block is an ancestor in this request's chain snapshot.
    if (cache.Get(key, mined_hash)) {
        const auto* mined = blocks.LookupBlockIndex(mined_hash);
        if (mined && chain.Contains(mined)) return mined;
    }
    const auto result = processor.GetMinedCommitment(kind, quorum);
    const auto* mined = blocks.LookupBlockIndex(result.second);
    if (result.first.IsNull() || !mined || !chain.Contains(mined)) return nullptr;
    cache.Insert(key, result.second);
    return mined;
}

QuorumProofChain QuorumProofBuilder::Build(const CBlockIndex* checkpoint, const chainlock::ChainLockSig& target) const
{
    const auto& chain = m_chain;
    const auto& blocks = m_chainman.m_blockman;
    const auto* targetIndex = chain[target.getHeight()];
    Require(checkpoint && targetIndex && chain.Contains(checkpoint) && checkpoint->nHeight < targetIndex->nHeight,
            "checkpoint/target not on chain");
    Require(targetIndex->GetBlockHash() == target.getBlockHash(), "target ChainLock block mismatch");
    auto certificate = [&](const chainlock::CoinbaseChainLock& entry) {
        return ProofCertificate{uint32_t(entry.clsig.getHeight()), chain[entry.clsig.getHeight()]->GetBlockHeader(),
                                entry.clsig.getSig()};
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
        const auto* mined = MinedCommitmentBlock(m_quorum_block_processor, chain, blocks, kind, needed->quorumHash);
        Require(mined && mined->nHeight > checkpoint->nHeight, "no bridge to snapshot");
        CBlock block;
        Require(node::ReadBlockFromDisk(block, mined, Params().GetConsensus()), "mining block unavailable");
        std::optional<ProofTransaction> mining;
        for (size_t i = 1; i < block.vtx.size(); ++i) {
            if (block.vtx[i]->nType != TRANSACTION_QUORUM_COMMITMENT) continue;
            const auto payload = GetTxPayload<CFinalCommitmentTxPayload>(*block.vtx[i]);
            if (payload && !payload->commitment.IsNull() && payload->commitment.llmqType == kind &&
                payload->commitment.quorumHash == needed->quorumHash) {
                mining = ProofTransaction::Build(block, i);
                break;
            }
        }
        Require(mining.has_value(), "mining transaction unavailable");
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
            height = entry->clsig.getHeight();
            if (height > mined->nHeight + 8 && best) break;
            auto signer = DetermineChainlockSigningCommitment(height);
            if (!signer || signer->quorumHash == needed->quorumHash) continue;
            const auto* previous = MinedCommitmentBlock(m_quorum_block_processor, chain, blocks, kind, signer->quorumHash);
            if (!previous || previous->nHeight >= mined->nHeight || (previous->nHeight <= checkpoint->nHeight && !seedHashes.count(signer->quorumHash))) continue;
            auto cert = certificate(*entry);
            if (!cert.Verify(*signer, checkpoint->nHeight, kind)) continue;
            const auto cost = 180 + 4 + mining->transaction.size() + 9 + 32 * mining->path.siblings.size() + 2 +
                              80 * (height - mined->nHeight);
            const double score = double(cost) / (mined->nHeight - std::max(checkpoint->nHeight, previous->nHeight));
            if (score >= bestScore) continue;
            bestScore = score;
            best = ProofLink{cert, *mining, {}};
            predecessor = std::move(signer);
        }
        Require(best && predecessor, "no B-only route within proof budget");
        for (int32_t h = mined->nHeight; h < int64_t(best->certificate.height); ++h) best->ancestors.push_back(chain[h]->GetBlockHeader());
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
