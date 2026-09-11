// Copyright (c) 2025-2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying file COPYING.
#ifndef BITCOIN_LLMQ_QUORUMPROOFS_H
#define BITCOIN_LLMQ_QUORUMPROOFS_H
#include <bls/bls.h>
#include <llmq/commitment.h>
#include <llmq/quorumproofdata.h>
#include <optional>
class CBlockIndex;
class CChain;
class CDataStream;
class ChainstateManager;
namespace chainlock {
class CoinbaseChainLockReader;
struct ChainLockSig;
} // namespace chainlock
namespace llmq {
class CQuorumBlockProcessor;
class CQuorumManager;
struct ProofState {
    uint8_t network{0};
    uint32_t height{0};
    uint256 blockHash;
    uint256 masternodeRoot;
    uint256 quorumRoot;
    SERIALIZE_METHODS(ProofState, obj) { READWRITE(obj.network, obj.height, obj.blockHash, obj.masternodeRoot, obj.quorumRoot); }
    bool operator==(const ProofState&) const = default;
    UniValue ToJson() const;
    static ProofState FromJson(const UniValue& value);
};
struct ProofCertificate {
    uint32_t height{0};
    CBlockHeader header;
    CBLSSignature signature;
    bool Verify(const CFinalCommitment& signer, uint32_t minimum, Consensus::LLMQType kind) const;
};
struct ProofLink {
    ProofCertificate certificate;
    ProofTransaction mining;
    std::vector<CBlockHeader> ancestors;
};
struct QuorumProofChain {
    ProofState anchor;
    std::vector<unsigned char> seed;
    ProofMerklePath seedPath;
    std::vector<ProofLink> links;
    ProofCertificate target;
    ProofTransaction coinbase;
    std::vector<unsigned char> Encode() const;
    static QuorumProofChain Decode(const std::vector<unsigned char>& bytes);
    ProofState Verify(const ProofState& trusted) const;
};
struct ProofProjection {
    uint8_t kind{0};
    std::vector<unsigned char> leaf;
    ProofMerklePath path;
};
std::vector<unsigned char> EncodeBootstrap(const QuorumProofChain& proof, const std::vector<ProofProjection>& records);
class QuorumProofBuilder
{
    const CQuorumBlockProcessor& m_quorum_block_processor;
    const CQuorumManager& m_qman;
    const CChain& m_chain;
    const ChainstateManager& m_chainman;
    chainlock::CoinbaseChainLockReader& m_chainlocks;
    std::optional<CFinalCommitment> DetermineChainlockSigningCommitment(int32_t height) const;

public:
    QuorumProofBuilder(const CQuorumBlockProcessor& processor, const CQuorumManager& qman, const CChain& chain,
                       const ChainstateManager& chainman, chainlock::CoinbaseChainLockReader& chainlocks) :
        m_quorum_block_processor(processor),
        m_qman(qman),
        m_chain(chain),
        m_chainman(chainman),
        m_chainlocks(chainlocks)
    {
    }
    std::vector<CFinalCommitment> ActiveCommitments(const CBlockIndex* index) const;
    static ProofState StateAt(const CBlockIndex* index);
    QuorumProofChain Build(const CBlockIndex* checkpoint, const chainlock::ChainLockSig& target) const;
};
} // namespace llmq
#endif // BITCOIN_LLMQ_QUORUMPROOFS_H
