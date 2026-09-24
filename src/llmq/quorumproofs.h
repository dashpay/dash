// Copyright (c) 2025-2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying file COPYING.
#ifndef BITCOIN_LLMQ_QUORUMPROOFS_H
#define BITCOIN_LLMQ_QUORUMPROOFS_H
#include <bls/bls.h>
#include <llmq/commitment.h>
#include <llmq/quorumproofdata.h>
#include <optional>
class CBlockIndex;
class CDeterministicMNManager;
class CSimplifiedMNList;
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
/** As above, for a proof whose Verify(proof.anchor) result the caller already holds. */
std::vector<unsigned char> EncodeBootstrap(const QuorumProofChain& proof, const ProofState& verified,
                                           const std::vector<ProofProjection>& records);
/** The simplified masternode list at a block with its entry hashes, the leaves
 *  of that block's masternode root. */
struct MasternodeLeaves {
    std::shared_ptr<const CSimplifiedMNList> sml;
    std::vector<uint256> leaves;
};
/** Memoized by block hash once the list hashes to the block's masternode root. */
MasternodeLeaves MasternodeLeavesAt(CDeterministicMNManager& dmnman, const CBlockIndex* index);
/** Checkpoint and target states are memoized by block hash; a state read once
 *  stays available even if the block's data is later removed. */
void ClearProofStateCacheForTesting();

class QuorumProofBuilder
{
    const CQuorumBlockProcessor& m_quorum_block_processor;
    const CQuorumManager& m_qman;
    const CBlockIndex* m_tip;
    const ChainstateManager& m_chainman;
    chainlock::CoinbaseChainLockReader& m_chainlocks;
    std::optional<CFinalCommitment> DetermineChainlockSigningCommitment(int32_t height) const;

public:
    /** Every block the proof touches must be an ancestor of tip, which pins one
     *  branch for the whole build. */
    QuorumProofBuilder(const CQuorumBlockProcessor& processor, const CQuorumManager& qman, const CBlockIndex* tip,
                       const ChainstateManager& chainman, chainlock::CoinbaseChainLockReader& chainlocks) :
        m_quorum_block_processor(processor),
        m_qman(qman),
        m_tip(tip),
        m_chainman(chainman),
        m_chainlocks(chainlocks)
    {
    }
    std::vector<CFinalCommitment> ActiveCommitments(const CBlockIndex* index) const;
    static ProofState StateAt(const CBlockIndex* index);
    /** No value when the target requires a retired checkpoint quorum; other failures throw. */
    std::optional<QuorumProofChain> Build(const CBlockIndex* checkpoint, const chainlock::ChainLockSig& target) const;
};
} // namespace llmq
#endif // BITCOIN_LLMQ_QUORUMPROOFS_H
