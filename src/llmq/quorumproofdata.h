// Copyright (c) 2025 The Dash Core developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_LLMQ_QUORUMPROOFDATA_H
#define BITCOIN_LLMQ_QUORUMPROOFDATA_H

#include <primitives/block.h>
#include <primitives/transaction.h>
#include <serialize.h>
#include <uint256.h>

#include <string>
#include <vector>

class UniValue;

namespace llmq {

// Maximum merkle path length (DoS protection)
// A path of 32 levels can support 2^32 leaves, which is more than sufficient
static constexpr size_t MAX_MERKLE_PATH_LENGTH = 32;

/**
 * Merkle proof for a quorum commitment within the merkleRootQuorums.
 * Allows verification that a commitment is included in a block's cbtx.
 */
struct QuorumMerkleProof {
    std::vector<uint256> merklePath;      // Sibling hashes from leaf to root
    std::vector<bool> merklePathSide;     // true = right sibling, false = left

    SERIALIZE_METHODS(QuorumMerkleProof, obj) {
        READWRITE(obj.merklePath, DYNBITSET(obj.merklePathSide));
    }

    /**
     * Verify the merkle proof for a given leaf hash against an expected root.
     * @param leafHash The hash of the commitment (SerializeHash of CFinalCommitment)
     * @param expectedRoot The merkleRootQuorums from the cbtx
     * @return true if the proof is valid
     */
    [[nodiscard]] bool Verify(const uint256& leafHash, const uint256& expectedRoot) const;

    [[nodiscard]] UniValue ToJson() const;
};

/**
 * Pre-computed proof data for a quorum commitment.
 * Stored in DB when a commitment is mined and retrieved for proof chain generation.
 * This avoids expensive disk reads and merkle proof computations at query time.
 */
struct QuorumProofData {
    QuorumMerkleProof quorumMerkleProof;       // Proof within merkleRootQuorums
    CTransactionRef coinbaseTx;                 // The coinbase transaction containing merkleRootQuorums
    std::vector<uint256> coinbaseMerklePath;    // Proof that coinbaseTx is in block's merkle root
    std::vector<bool> coinbaseMerklePathSide;   // Side indicators for coinbase merkle proof
    CBlockHeader header;                        // Block header where quorum was mined

    SERIALIZE_METHODS(QuorumProofData, obj) {
        READWRITE(obj.quorumMerkleProof, obj.coinbaseTx,
                  obj.coinbaseMerklePath, DYNBITSET(obj.coinbaseMerklePathSide),
                  obj.header);
    }
};

// Database key prefix for quorum proof data index
static const std::string DB_QUORUM_PROOF_DATA = "q_qpd";

} // namespace llmq

#endif // BITCOIN_LLMQ_QUORUMPROOFDATA_H
