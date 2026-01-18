// Copyright (c) 2025 The Dash Core developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_LLMQ_QUORUMPROOFS_H
#define BITCOIN_LLMQ_QUORUMPROOFS_H

#include <bls/bls.h>
#include <llmq/commitment.h>
#include <llmq/quorumproofdata.h>
#include <llmq/types.h>
#include <serialize.h>
#include <uint256.h>

#include <set>
#include <vector>

class CBlockIndex;
class CChain;
class CChainParams;
class CEvoDB;

namespace node {
class BlockManager;
} // namespace node

namespace llmq {

class CQuorumBlockProcessor;
class CQuorumManager;

/**
 * Entry stored in the chainlock index database.
 * Maps chainlocked height to the signature and where it was embedded.
 */
struct ChainlockIndexEntry {
    CBLSSignature signature;
    uint256 cbtxBlockHash;    // Block where this chainlock was embedded in cbtx
    int32_t cbtxHeight{0};    // Height of that block

    SERIALIZE_METHODS(ChainlockIndexEntry, obj) {
        READWRITE(obj.signature, obj.cbtxBlockHash, obj.cbtxHeight);
    }
};

/**
 * A chainlock proof entry for the proof chain.
 * Contains the chainlock signature for a specific block.
 */
struct ChainlockProofEntry {
    int32_t nHeight{0};
    uint256 blockHash;
    CBLSSignature signature;

    SERIALIZE_METHODS(ChainlockProofEntry, obj) {
        READWRITE(obj.nHeight, obj.blockHash, obj.signature);
    }

    [[nodiscard]] UniValue ToJson() const;
};

/**
 * Complete proof for a single quorum commitment.
 * Links a commitment to a chainlocked block via merkle proofs.
 */
struct QuorumCommitmentProof {
    CFinalCommitment commitment;
    uint32_t chainlockIndex{0};              // Index into chainlocks array that covers this commitment
    QuorumMerkleProof quorumMerkleProof;     // Proof within merkleRootQuorums
    CTransactionRef coinbaseTx;              // The coinbase transaction containing merkleRootQuorums
    std::vector<uint256> coinbaseMerklePath;  // Proof that coinbaseTx is in block's merkle root
    std::vector<bool> coinbaseMerklePathSide;

    SERIALIZE_METHODS(QuorumCommitmentProof, obj) {
        READWRITE(obj.commitment, obj.chainlockIndex,
                  obj.quorumMerkleProof,
                  obj.coinbaseTx, obj.coinbaseMerklePath, DYNBITSET(obj.coinbaseMerklePathSide));
    }

    [[nodiscard]] UniValue ToJson() const;
};

/**
 * Complete proof chain from a checkpoint to a target quorum.
 * Contains all data needed to verify a quorum's public key starting from
 * known chainlock quorums.
 */
struct QuorumProofChain {
    std::vector<CBlockHeader> headers;
    std::vector<ChainlockProofEntry> chainlocks;
    std::vector<QuorumCommitmentProof> quorumProofs;

    SERIALIZE_METHODS(QuorumProofChain, obj) {
        READWRITE(obj.headers, obj.chainlocks, obj.quorumProofs);
    }

    [[nodiscard]] UniValue ToJson() const;
};

/**
 * Checkpoint data provided by the verifier.
 * Contains known trusted chainlock quorum public keys.
 */
struct QuorumCheckpoint {
    uint256 blockHash;
    int32_t height{0};
    struct QuorumEntry {
        uint256 quorumHash;
        Consensus::LLMQType quorumType{Consensus::LLMQType::LLMQ_NONE};
        CBLSPublicKey publicKey;

        SERIALIZE_METHODS(QuorumEntry, obj) {
            READWRITE(obj.quorumHash, obj.quorumType, obj.publicKey);
        }
    };
    std::vector<QuorumEntry> chainlockQuorums;

    SERIALIZE_METHODS(QuorumCheckpoint, obj) {
        READWRITE(obj.blockHash, obj.height, obj.chainlockQuorums);
    }

    [[nodiscard]] UniValue ToJson() const;
    static QuorumCheckpoint FromJson(const UniValue& obj);
};

/**
 * Result of proof chain verification.
 */
struct QuorumProofVerifyResult {
    bool valid{false};
    CBLSPublicKey quorumPublicKey;
    std::string error;

    [[nodiscard]] UniValue ToJson() const;
};

// Type alias for commitment hash cache used in proof building
// Maps (llmqType, quorumHash) -> SerializeHash(commitment)
using CommitmentHashCache = std::map<std::pair<Consensus::LLMQType, uint256>, uint256>;

/**
 * Manager for chainlock indexing and quorum proof generation/verification.
 */
class CQuorumProofManager {
private:
    CEvoDB& m_evoDb;
    const CQuorumBlockProcessor& m_quorum_block_processor;

    // Helper to determine which quorum signed a chainlock at a given height
    [[nodiscard]] std::optional<CFinalCommitment> DetermineChainlockSigningCommitment(
        int32_t chainlockHeight,
        const CChain& active_chain,
        const CQuorumManager& qman) const;

    // Helper to find the first chainlock covering a block
    [[nodiscard]] int32_t FindChainlockCoveringBlock(const CBlockIndex* pMinedBlock) const;

    // Helper to find a chainlock covering a block that is signed by a known quorum
    // This allows us to skip intermediate quorums when a direct path exists
    [[nodiscard]] int32_t FindChainlockSignedByKnownQuorum(
        const CBlockIndex* pMinedBlock,
        const std::set<CBLSPublicKey>& knownQuorumPubKeys,
        const CChain& active_chain,
        const CQuorumManager& qman) const;

public:
    CQuorumProofManager(CEvoDB& evoDb, const CQuorumBlockProcessor& quorum_block_processor)
        : m_evoDb(evoDb), m_quorum_block_processor(quorum_block_processor) {}

    CQuorumProofManager() = delete;
    CQuorumProofManager(const CQuorumProofManager&) = delete;
    CQuorumProofManager& operator=(const CQuorumProofManager&) = delete;

    // Chainlock Index Management
    void IndexChainlock(int32_t chainlockedHeight, const uint256& blockHash,
                        const CBLSSignature& signature, const uint256& cbtxBlockHash,
                        int32_t cbtxHeight);
    void RemoveChainlockIndex(int32_t chainlockedHeight);
    [[nodiscard]] std::optional<ChainlockIndexEntry> GetChainlockByHeight(int32_t height) const;

    // Merkle Proof Building
    [[nodiscard]] std::optional<QuorumMerkleProof> BuildQuorumMerkleProof(
        const CBlockIndex* pindex,
        Consensus::LLMQType llmqType,
        const uint256& quorumHash,
        const CBlock* pBlock = nullptr,
        CommitmentHashCache* pHashCache = nullptr) const;

    // Proof Chain Generation
    [[nodiscard]] std::optional<QuorumProofChain> BuildProofChain(
        const QuorumCheckpoint& checkpoint,
        Consensus::LLMQType targetQuorumType,
        const uint256& targetQuorumHash,
        const CQuorumManager& qman,
        const CChain& active_chain,
        const node::BlockManager& block_man) const;

    // Proof Chain Verification
    [[nodiscard]] QuorumProofVerifyResult VerifyProofChain(
        const QuorumCheckpoint& checkpoint,
        const QuorumProofChain& proof,
        Consensus::LLMQType expectedType,
        const uint256& expectedQuorumHash) const;

    // Migration: Build chainlock index from historical blocks
    // Should be called once during startup after chain is loaded
    void MigrateChainlockIndex(const CChain& active_chain, const CChainParams& chainparams);

    // Migration: Build quorum proof data index from historical commitments
    // Should be called once during startup after chain is loaded
    void MigrateQuorumProofIndex(const CChain& active_chain, const CChainParams& chainparams,
                                  const node::BlockManager& block_man);

    // Quorum Proof Data Index Management
    void StoreQuorumProofData(Consensus::LLMQType llmqType, const uint256& quorumHash,
                               const QuorumProofData& proofData);
    void EraseQuorumProofData(Consensus::LLMQType llmqType, const uint256& quorumHash);
    [[nodiscard]] std::optional<QuorumProofData> GetQuorumProofData(Consensus::LLMQType llmqType,
                                                                     const uint256& quorumHash) const;

    // Helper to compute proof data for a commitment at mining time
    [[nodiscard]] std::optional<QuorumProofData> ComputeQuorumProofData(
        const CBlockIndex* pMinedBlock,
        Consensus::LLMQType llmqType,
        const uint256& quorumHash,
        const CBlock& block) const;
};

// Database key prefix for chainlock index
static const std::string DB_CHAINLOCK_BY_HEIGHT = "q_clh";

// Database key for chainlock index version (for migration tracking)
static const std::string DB_CHAINLOCK_INDEX_VERSION = "q_clv";

// Current version of the chainlock index
// Increment this when the index format changes to trigger re-migration
static constexpr int CHAINLOCK_INDEX_VERSION = 2;

// Maximum proof chain length (DoS protection)
// Limits how many intermediate quorums can be proven in a single chain
static constexpr size_t MAX_PROOF_CHAIN_LENGTH = 500;

// Maximum height offset to search for a chainlock covering a block
// This limits how far forward we search from a block's height to find coverage
static constexpr int32_t MAX_CHAINLOCK_SEARCH_OFFSET = 100;

// Database key for quorum proof index version (for migration tracking)
static const std::string DB_QUORUM_PROOF_INDEX_VERSION = "q_qpv";

// Current version of the quorum proof index
// Increment this when the index format changes to trigger re-migration
static constexpr int QUORUM_PROOF_INDEX_VERSION = 1;

} // namespace llmq

#endif // BITCOIN_LLMQ_QUORUMPROOFS_H
