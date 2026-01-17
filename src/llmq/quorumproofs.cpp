// Copyright (c) 2025 The Dash Core developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <llmq/quorumproofs.h>

#include <chain.h>
#include <chainlock/clsig.h>
#include <chainparams.h>
#include <consensus/merkle.h>
#include <evo/cbtx.h>
#include <evo/evodb.h>
#include <evo/specialtx.h>
#include <hash.h>
#include <llmq/blockprocessor.h>
#include <llmq/quorumsman.h>
#include <llmq/signhash.h>
#include <node/blockstorage.h>
#include <primitives/block.h>

#include <algorithm>
#include <set>

using node::ReadBlockFromDisk;

namespace llmq {

//
// JSON Serialization helpers
//

UniValue ChainlockProofEntry::ToJson() const
{
    UniValue obj(UniValue::VOBJ);
    obj.pushKV("height", nHeight);
    obj.pushKV("blockhash", blockHash.ToString());
    obj.pushKV("signature", signature.ToString());
    return obj;
}

UniValue QuorumMerkleProof::ToJson() const
{
    UniValue obj(UniValue::VOBJ);
    UniValue pathArr(UniValue::VARR);
    for (const auto& hash : merklePath) {
        pathArr.push_back(hash.ToString());
    }
    obj.pushKV("merklePath", pathArr);

    UniValue sideArr(UniValue::VARR);
    for (bool side : merklePathSide) {
        sideArr.push_back(side);
    }
    obj.pushKV("merklePathSide", sideArr);
    return obj;
}

bool QuorumMerkleProof::Verify(const uint256& leafHash, const uint256& expectedRoot) const
{
    if (merklePath.size() != merklePathSide.size()) {
        return false;
    }

    // DoS protection: reject excessively long merkle paths
    // A path longer than MAX_MERKLE_PATH_LENGTH would imply a tree with more than 2^32 leaves
    if (merklePath.size() > MAX_MERKLE_PATH_LENGTH) {
        return false;
    }

    uint256 current = leafHash;
    for (size_t i = 0; i < merklePath.size(); ++i) {
        if (merklePathSide[i]) {
            // Sibling is on the right
            current = Hash(current, merklePath[i]);
        } else {
            // Sibling is on the left
            current = Hash(merklePath[i], current);
        }
    }

    return current == expectedRoot;
}

UniValue QuorumCommitmentProof::ToJson() const
{
    UniValue obj(UniValue::VOBJ);
    obj.pushKV("commitment", commitment.ToJson());
    obj.pushKV("chainlockIndex", chainlockIndex);
    obj.pushKV("quorumMerkleProof", quorumMerkleProof.ToJson());
    obj.pushKV("coinbaseTxHash", coinbaseTx ? coinbaseTx->GetHash().ToString() : "");

    UniValue cbPathArr(UniValue::VARR);
    for (const auto& hash : coinbaseMerklePath) {
        cbPathArr.push_back(hash.ToString());
    }
    obj.pushKV("coinbaseMerklePath", cbPathArr);

    UniValue cbSideArr(UniValue::VARR);
    for (bool side : coinbaseMerklePathSide) {
        cbSideArr.push_back(side);
    }
    obj.pushKV("coinbaseMerklePathSide", cbSideArr);
    return obj;
}

UniValue QuorumProofChain::ToJson() const
{
    UniValue obj(UniValue::VOBJ);

    UniValue headersArr(UniValue::VARR);
    for (const auto& header : headers) {
        UniValue hObj(UniValue::VOBJ);
        hObj.pushKV("hash", header.GetHash().ToString());
        hObj.pushKV("version", header.nVersion);
        hObj.pushKV("prevBlockHash", header.hashPrevBlock.ToString());
        hObj.pushKV("merkleRoot", header.hashMerkleRoot.ToString());
        hObj.pushKV("time", header.nTime);
        hObj.pushKV("bits", header.nBits);
        hObj.pushKV("nonce", header.nNonce);
        headersArr.push_back(hObj);
    }
    obj.pushKV("headers", headersArr);

    UniValue chainlocksArr(UniValue::VARR);
    for (const auto& cl : chainlocks) {
        chainlocksArr.push_back(cl.ToJson());
    }
    obj.pushKV("chainlocks", chainlocksArr);

    UniValue proofsArr(UniValue::VARR);
    for (const auto& proof : quorumProofs) {
        proofsArr.push_back(proof.ToJson());
    }
    obj.pushKV("quorumProofs", proofsArr);

    return obj;
}

UniValue QuorumCheckpoint::ToJson() const
{
    UniValue obj(UniValue::VOBJ);
    obj.pushKV("blockHash", blockHash.ToString());
    obj.pushKV("height", height);

    UniValue quorumsArr(UniValue::VARR);
    for (const auto& q : chainlockQuorums) {
        UniValue qObj(UniValue::VOBJ);
        qObj.pushKV("quorumHash", q.quorumHash.ToString());
        qObj.pushKV("quorumType", static_cast<int>(q.quorumType));
        qObj.pushKV("publicKey", q.publicKey.ToString());
        quorumsArr.push_back(qObj);
    }
    obj.pushKV("chainlockQuorums", quorumsArr);

    return obj;
}

QuorumCheckpoint QuorumCheckpoint::FromJson(const UniValue& obj)
{
    QuorumCheckpoint checkpoint;

    checkpoint.blockHash = uint256S(obj["blockHash"].get_str());
    checkpoint.height = obj["height"].getInt<int32_t>();

    const UniValue& quorums = obj["chainlockQuorums"];
    for (size_t i = 0; i < quorums.size(); ++i) {
        const UniValue& q = quorums[i];
        QuorumEntry entry;
        entry.quorumHash = uint256S(q["quorumHash"].get_str());
        entry.quorumType = static_cast<Consensus::LLMQType>(q["quorumType"].getInt<int>());
        if (!entry.publicKey.SetHexStr(q["publicKey"].get_str(), /*specificLegacyScheme=*/false)) {
            throw std::runtime_error("Invalid publicKey in checkpoint");
        }
        checkpoint.chainlockQuorums.push_back(entry);
    }

    return checkpoint;
}

UniValue QuorumProofVerifyResult::ToJson() const
{
    UniValue obj(UniValue::VOBJ);
    obj.pushKV("valid", valid);
    if (valid) {
        obj.pushKV("quorumPublicKey", quorumPublicKey.ToString());
    } else {
        obj.pushKV("error", error);
    }
    return obj;
}

//
// CQuorumProofManager implementation
//

void CQuorumProofManager::IndexChainlock(int32_t chainlockedHeight, const uint256& blockHash,
                                          const CBLSSignature& signature, const uint256& cbtxBlockHash,
                                          int32_t cbtxHeight)
{
    ChainlockIndexEntry entry;
    entry.signature = signature;
    entry.cbtxBlockHash = cbtxBlockHash;
    entry.cbtxHeight = cbtxHeight;

    m_evoDb.Write(std::make_pair(DB_CHAINLOCK_BY_HEIGHT, chainlockedHeight), entry);
}

void CQuorumProofManager::RemoveChainlockIndex(int32_t chainlockedHeight)
{
    m_evoDb.Erase(std::make_pair(DB_CHAINLOCK_BY_HEIGHT, chainlockedHeight));
}

std::optional<ChainlockIndexEntry> CQuorumProofManager::GetChainlockByHeight(int32_t height) const
{
    ChainlockIndexEntry entry;
    if (m_evoDb.Read(std::make_pair(DB_CHAINLOCK_BY_HEIGHT, height), entry)) {
        return entry;
    }
    return std::nullopt;
}

/**
 * Verify a merkle proof by computing the root from a leaf hash and comparing to expected.
 * @param leafHash The hash of the leaf element
 * @param merklePath The sibling hashes from leaf to root
 * @param merklePathSide Side indicators (true = sibling on right, false = sibling on left)
 * @param expectedRoot The expected merkle root
 * @return true if proof is valid
 */
static bool VerifyMerkleProof(const uint256& leafHash,
                               const std::vector<uint256>& merklePath,
                               const std::vector<bool>& merklePathSide,
                               const uint256& expectedRoot)
{
    if (merklePath.size() != merklePathSide.size()) {
        return false;
    }

    if (merklePath.size() > MAX_MERKLE_PATH_LENGTH) {
        return false;
    }

    uint256 current = leafHash;
    for (size_t i = 0; i < merklePath.size(); ++i) {
        if (merklePathSide[i]) {
            current = Hash(current, merklePath[i]);
        } else {
            current = Hash(merklePath[i], current);
        }
    }

    return current == expectedRoot;
}

/**
 * Helper function to build merkle proof with path tracking.
 * Returns the merkle path (sibling hashes) and side indicators.
 *
 * The algorithm works by iteratively building each level of the merkle tree
 * from leaves to root, tracking the target element's position at each level.
 *
 * At each level:
 * - We pair up elements and hash them together
 * - We record the sibling of our target element in the merkle path
 * - We track where our combined hash will be in the next level
 */
static std::pair<std::vector<uint256>, std::vector<bool>> BuildMerkleProofPath(
    const std::vector<uint256>& hashes, size_t targetIndex)
{
    std::vector<uint256> merklePath;
    std::vector<bool> merklePathSide;

    if (hashes.empty()) {
        return {merklePath, merklePathSide};
    }

    std::vector<uint256> current = hashes;
    size_t index = targetIndex;

    while (current.size() > 1) {
        std::vector<uint256> next;
        size_t nextIndex = 0;

        for (size_t i = 0; i < current.size(); i += 2) {
            size_t left = i;
            size_t right = (i + 1 < current.size()) ? i + 1 : i; // Duplicate last if odd

            // Check if our target is in this pair
            if (index == left || index == right) {
                // Record the sibling and its position
                if (index == left) {
                    merklePath.push_back(current[right]);
                    merklePathSide.push_back(true); // sibling is on right
                } else {
                    merklePath.push_back(current[left]);
                    merklePathSide.push_back(false); // sibling is on left
                }
                // The combined hash will be at position next.size() in the next level
                nextIndex = next.size();
            }

            next.push_back(Hash(current[left], current[right]));
        }

        // Update index to track our element in the next level
        index = nextIndex;
        current = std::move(next);
    }

    return {merklePath, merklePathSide};
}

std::optional<QuorumMerkleProof> CQuorumProofManager::BuildQuorumMerkleProof(
    const CBlockIndex* pindex,
    Consensus::LLMQType llmqType,
    const uint256& quorumHash) const
{
    if (pindex == nullptr) {
        return std::nullopt;
    }

    // Get all active commitments at this block
    auto commitmentsMap = m_quorum_block_processor.GetMinedAndActiveCommitmentsUntilBlock(pindex);

    // Collect all commitment hashes (matching CalcCbTxMerkleRootQuorums logic)
    std::vector<uint256> commitmentHashes;
    uint256 targetCommitmentHash;
    bool targetFound = false;

    for (const auto& [type, blockIndexes] : commitmentsMap) {
        for (const auto* blockIndex : blockIndexes) {
            auto [commitment, minedBlockHash] = m_quorum_block_processor.GetMinedCommitment(type, blockIndex->GetBlockHash());
            if (minedBlockHash == uint256::ZERO) {
                continue;
            }

            uint256 commitmentHash = ::SerializeHash(commitment);
            commitmentHashes.push_back(commitmentHash);

            if (type == llmqType && commitment.quorumHash == quorumHash) {
                targetCommitmentHash = commitmentHash;
                targetFound = true;
            }
        }
    }

    if (!targetFound) {
        return std::nullopt;
    }

    // Sort hashes to match CalcCbTxMerkleRootQuorums
    std::sort(commitmentHashes.begin(), commitmentHashes.end());

    // Find target index in sorted list
    auto it = std::find(commitmentHashes.begin(), commitmentHashes.end(), targetCommitmentHash);
    if (it == commitmentHashes.end()) {
        return std::nullopt;
    }
    size_t targetIndex = std::distance(commitmentHashes.begin(), it);

    // Build the merkle proof
    auto [path, side] = BuildMerkleProofPath(commitmentHashes, targetIndex);

    QuorumMerkleProof proof;
    proof.merklePath = std::move(path);
    proof.merklePathSide = std::move(side);

    return proof;
}

int32_t CQuorumProofManager::FindChainlockCoveringBlock(const CBlockIndex* pMinedBlock) const
{
    if (pMinedBlock == nullptr) {
        return -1;
    }

    // Search for the first chainlock that covers this block
    // A chainlock at height H covers all blocks from genesis to H
    // We search forward from the mined block's height up to MAX_CHAINLOCK_SEARCH_OFFSET blocks
    const int32_t maxHeight = pMinedBlock->nHeight + MAX_CHAINLOCK_SEARCH_OFFSET;
    for (int32_t height = pMinedBlock->nHeight; height <= maxHeight; ++height) {
        if (GetChainlockByHeight(height).has_value()) {
            return height;
        }
    }
    return -1;
}

CQuorumCPtr CQuorumProofManager::DetermineChainlockSigningQuorum(
    int32_t chainlockHeight,
    const CChain& active_chain,
    const CQuorumManager& qman) const
{
    // Get the chainlock LLMQ type from consensus parameters
    const auto llmqType = Params().GetConsensus().llmqTypeChainLocks;
    const auto& llmq_params_opt = Params().GetLLMQ(llmqType);
    if (!llmq_params_opt.has_value()) {
        return nullptr;
    }
    const auto& llmq_params = llmq_params_opt.value();

    // Generate the request ID for the chainlock at this height
    const uint256 requestId = chainlock::GenSigRequestId(chainlockHeight);

    // Use the existing SelectQuorumForSigning logic
    return SelectQuorumForSigning(llmq_params, active_chain, qman,
                                   requestId, chainlockHeight, SIGN_HEIGHT_OFFSET);
}

std::optional<QuorumProofChain> CQuorumProofManager::BuildProofChain(
    const QuorumCheckpoint& checkpoint,
    Consensus::LLMQType targetQuorumType,
    const uint256& targetQuorumHash,
    const CQuorumManager& qman,
    const CChain& active_chain) const
{
    // Phase 1: Build set of known chainlock quorum public keys from checkpoint
    std::set<CBLSPublicKey> knownQuorumPubKeys;
    for (const auto& q : checkpoint.chainlockQuorums) {
        knownQuorumPubKeys.insert(q.publicKey);
    }

    // Phase 2: Work backwards from target to find the dependency chain
    // Each ProofStep represents a quorum that needs to be proven and
    // the chainlock height that covers its mined block
    struct ProofStep {
        CQuorumCPtr quorum;
        int32_t chainlockHeight;
    };
    std::vector<ProofStep> proofSteps;
    std::set<uint256> visitedQuorums; // Cycle detection

    // Start with the target quorum
    CQuorumCPtr currentQuorum = qman.GetQuorum(targetQuorumType, targetQuorumHash);
    if (!currentQuorum) {
        return std::nullopt;
    }

    while (true) {
        // Cycle detection
        if (visitedQuorums.count(currentQuorum->qc->quorumHash)) {
            return std::nullopt; // Cycle detected - invalid chain
        }
        visitedQuorums.insert(currentQuorum->qc->quorumHash);

        // DoS protection: limit chain length
        if (proofSteps.size() >= MAX_PROOF_CHAIN_LENGTH) {
            return std::nullopt;
        }

        // Find the first chainlock that covers this quorum's mined block
        const CBlockIndex* pMinedBlock = currentQuorum->m_quorum_base_block_index;
        if (!pMinedBlock) {
            return std::nullopt;
        }

        int32_t chainlockHeight = FindChainlockCoveringBlock(pMinedBlock);
        if (chainlockHeight < 0) {
            return std::nullopt; // No chainlock found covering this quorum
        }

        proofSteps.push_back({currentQuorum, chainlockHeight});

        // Determine which quorum signed this chainlock
        CQuorumCPtr signingQuorum = DetermineChainlockSigningQuorum(chainlockHeight, active_chain, qman);
        if (!signingQuorum) {
            return std::nullopt; // Could not determine signing quorum
        }

        // Check if the signing quorum's public key is in the checkpoint's known quorums
        if (knownQuorumPubKeys.count(signingQuorum->qc->quorumPublicKey)) {
            // We've reached a quorum that's trusted by the checkpoint - done!
            break;
        }

        // The signing quorum is not in the checkpoint, so we need to prove it first
        currentQuorum = signingQuorum;
    }

    // Phase 3: Build proofs in forward order (reverse the dependency chain)
    std::reverse(proofSteps.begin(), proofSteps.end());

    // Phase 4: Construct the QuorumProofChain
    QuorumProofChain chain;
    std::set<int32_t> includedChainlockHeights;

    for (const auto& step : proofSteps) {
        // Add chainlock entry if not already included
        if (!includedChainlockHeights.count(step.chainlockHeight)) {
            auto clEntry = GetChainlockByHeight(step.chainlockHeight);
            if (!clEntry.has_value()) {
                return std::nullopt;
            }

            // Get the block hash at the chainlock height
            const CBlockIndex* pClBlock = step.quorum->m_quorum_base_block_index->GetAncestor(step.chainlockHeight);
            if (!pClBlock) {
                return std::nullopt;
            }

            ChainlockProofEntry clProof;
            clProof.nHeight = step.chainlockHeight;
            clProof.blockHash = pClBlock->GetBlockHash();
            clProof.signature = clEntry->signature;
            chain.chainlocks.push_back(clProof);
            includedChainlockHeights.insert(step.chainlockHeight);
        }

        // Build the quorum commitment proof
        const CBlockIndex* pMinedBlock = step.quorum->m_quorum_base_block_index;

        auto merkleProof = BuildQuorumMerkleProof(pMinedBlock, step.quorum->qc->llmqType, step.quorum->qc->quorumHash);
        if (!merkleProof.has_value()) {
            return std::nullopt;
        }

        // Read the block to get coinbase transaction
        CBlock block;
        if (!ReadBlockFromDisk(block, pMinedBlock, Params().GetConsensus())) {
            return std::nullopt;
        }

        // Build coinbase merkle proof
        std::vector<uint256> txHashes;
        for (const auto& tx : block.vtx) {
            txHashes.push_back(tx->GetHash());
        }

        auto [cbPath, cbSide] = BuildMerkleProofPath(txHashes, 0); // Coinbase is at index 0

        // Find the chainlock index for this proof step
        uint32_t chainlockIndex = 0;
        for (size_t i = 0; i < chain.chainlocks.size(); ++i) {
            if (chain.chainlocks[i].nHeight == step.chainlockHeight) {
                chainlockIndex = static_cast<uint32_t>(i);
                break;
            }
        }

        QuorumCommitmentProof commitmentProof;
        commitmentProof.commitment = *step.quorum->qc;
        commitmentProof.chainlockIndex = chainlockIndex;
        commitmentProof.quorumMerkleProof = merkleProof.value();
        commitmentProof.coinbaseTx = block.vtx[0];
        commitmentProof.coinbaseMerklePath = std::move(cbPath);
        commitmentProof.coinbaseMerklePathSide = std::move(cbSide);

        chain.quorumProofs.push_back(commitmentProof);

        // Add the block header
        chain.headers.push_back(block.GetBlockHeader());
    }

    return chain;
}

QuorumProofVerifyResult CQuorumProofManager::VerifyProofChain(
    const QuorumCheckpoint& checkpoint,
    const QuorumProofChain& proof,
    Consensus::LLMQType expectedType,
    const uint256& expectedQuorumHash) const
{
    QuorumProofVerifyResult result;

    // DoS protection: limit proof chain length
    if (proof.quorumProofs.size() > MAX_PROOF_CHAIN_LENGTH) {
        result.error = "Proof chain exceeds maximum length";
        return result;
    }

    if (proof.chainlocks.empty() || proof.quorumProofs.empty()) {
        result.error = "Proof chain is empty";
        return result;
    }

    if (proof.headers.size() != proof.quorumProofs.size()) {
        result.error = "Headers count does not match quorum proofs count";
        return result;
    }

    // Verify header chain continuity - each header's prevBlockHash must match the previous header's hash
    // This prevents an attacker from mixing headers from different blockchain forks
    for (size_t i = 1; i < proof.headers.size(); ++i) {
        if (proof.headers[i].hashPrevBlock != proof.headers[i - 1].GetHash()) {
            result.error = "Header chain is not continuous - prevBlockHash mismatch at index " + std::to_string(i);
            return result;
        }
    }

    // Phase 1: Build initial set of known chainlock quorum public keys from checkpoint
    // We use a set of public keys since that's what we actually verify signatures against
    std::set<CBLSPublicKey> knownQuorumPubKeys;
    for (const auto& q : checkpoint.chainlockQuorums) {
        knownQuorumPubKeys.insert(q.publicKey);
    }

    // Phase 2: Process quorum proofs IN ORDER
    // Each proven quorum adds its public key to the known set for subsequent proofs
    // This allows bridging: checkpoint proves A, A proves B, B proves C (target)
    const QuorumCommitmentProof* targetProof = nullptr;
    std::set<int32_t> verifiedChainlockHeights;

    for (size_t proofIdx = 0; proofIdx < proof.quorumProofs.size(); ++proofIdx) {
        const auto& qProof = proof.quorumProofs[proofIdx];

        // Get the chainlock that covers this commitment
        if (qProof.chainlockIndex >= proof.chainlocks.size()) {
            result.error = "Invalid chainlock index " + std::to_string(qProof.chainlockIndex);
            return result;
        }
        const auto& chainlock = proof.chainlocks[qProof.chainlockIndex];

        // Verify chainlock signature if we haven't verified this chainlock yet
        if (!verifiedChainlockHeights.count(chainlock.nHeight)) {
            if (!chainlock.signature.IsValid()) {
                result.error = "Invalid chainlock signature format at height " + std::to_string(chainlock.nHeight);
                return result;
            }

            // Verify the chainlock signature against current known quorum keys
            // For chainlocks, the message being signed is the block hash
            // Try both BLS schemes (non-legacy post-v19, legacy pre-v19)
            const auto verifyAgainstKey = [&chainlock](const CBLSPublicKey& pubKey) {
                return chainlock.signature.VerifyInsecure(pubKey, chainlock.blockHash, /*specificLegacyScheme=*/false) ||
                       chainlock.signature.VerifyInsecure(pubKey, chainlock.blockHash, /*specificLegacyScheme=*/true);
            };

            const bool signatureVerified = std::any_of(knownQuorumPubKeys.begin(), knownQuorumPubKeys.end(), verifyAgainstKey);

            if (!signatureVerified) {
                result.error = "Chainlock signature verification failed at height " +
                              std::to_string(chainlock.nHeight) +
                              " - signature does not match any known quorum key";
                return result;
            }

            verifiedChainlockHeights.insert(chainlock.nHeight);
        }

        // Get the corresponding header for this quorum proof
        const CBlockHeader& header = proof.headers[proofIdx];

        // Verify coinbase tx is in the block via merkle proof
        if (!qProof.coinbaseTx) {
            result.error = "Missing coinbase transaction in proof " + std::to_string(proofIdx);
            return result;
        }

        const uint256 coinbaseTxHash = qProof.coinbaseTx->GetHash();
        if (!VerifyMerkleProof(coinbaseTxHash, qProof.coinbaseMerklePath,
                               qProof.coinbaseMerklePathSide, header.hashMerkleRoot)) {
            result.error = "Coinbase merkle proof verification failed in proof " + std::to_string(proofIdx);
            return result;
        }

        // Extract merkleRootQuorums from cbtx
        auto opt_cbtx = GetTxPayload<CCbTx>(*qProof.coinbaseTx);
        if (!opt_cbtx.has_value()) {
            result.error = "Invalid coinbase transaction payload in proof " + std::to_string(proofIdx);
            return result;
        }

        const CCbTx& cbtx = opt_cbtx.value();

        // Verify the quorum commitment merkle proof against merkleRootQuorums
        uint256 commitmentHash = ::SerializeHash(qProof.commitment);
        if (!qProof.quorumMerkleProof.Verify(commitmentHash, cbtx.merkleRootQuorums)) {
            result.error = "Quorum commitment merkle proof verification failed in proof " + std::to_string(proofIdx);
            return result;
        }

        // This quorum is now proven! Add its public key to known keys for subsequent proofs
        knownQuorumPubKeys.insert(qProof.commitment.quorumPublicKey);

        // Check if this is the target quorum
        if (qProof.commitment.llmqType == expectedType &&
            qProof.commitment.quorumHash == expectedQuorumHash) {
            targetProof = &qProof;
        }
    }

    // Phase 3: Verify target quorum was proven
    if (!targetProof) {
        result.error = "Target quorum not found in proof chain";
        return result;
    }

    // All verifications passed
    result.valid = true;
    result.quorumPublicKey = targetProof->commitment.quorumPublicKey;

    return result;
}

} // namespace llmq
