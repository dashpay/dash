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
#include <llmq/quorums.h>
#include <llmq/quorumsman.h>
#include <llmq/signhash.h>
#include <node/blockstorage.h>
#include <primitives/block.h>
#include <sync.h>
#include <tinyformat.h>
#include <node/interface_ui.h>

#include <algorithm>
#include <set>

using node::ReadBlockFromDisk;

namespace llmq {

/**
 * Lightweight commitment info for proof chain building.
 * Avoids repeated full CFinalCommitment deserialization by caching only the data we need.
 */
struct CachedCommitmentInfo {
    uint256 quorumHash;
    CBLSPublicKey publicKey;
    const CBlockIndex* pMinedBlock;
    uint16_t quorumIndex;
    Consensus::LLMQType llmqType;
};

/**
 * Compute which commitment would sign a given height using cached commitment data.
 * This is a lightweight version of SelectCommitmentForSigning that avoids DB reads.
 *
 * @param llmq_params The LLMQ parameters
 * @param commitments Cached commitment info (must be non-empty)
 * @param selectionHash The request ID for the chainlock (GenSigRequestId(height))
 * @return Index into commitments vector of the selected quorum
 */
static size_t ComputeSigningCommitmentIndex(
    const Consensus::LLMQParams& llmq_params,
    const std::vector<CachedCommitmentInfo>& commitments,
    const uint256& selectionHash)
{
    assert(!commitments.empty());

    if (llmq_params.useRotation) {
        // For rotated quorums, selection is based on quorumIndex
        int n = std::log2(llmq_params.signingActiveQuorumCount);
        uint64_t b = selectionHash.GetUint64(3);
        uint64_t signer = (((1ull << n) - 1) & (b >> (64 - n - 1)));

        for (size_t i = 0; i < commitments.size(); ++i) {
            if (static_cast<uint64_t>(commitments[i].quorumIndex) == signer) {
                return i;
            }
        }
        return 0; // Fallback to first if not found
    } else {
        // For non-rotated quorums, selection is based on hash score
        std::vector<std::pair<uint256, size_t>> scores;
        scores.reserve(commitments.size());
        for (size_t i = 0; i < commitments.size(); ++i) {
            CHashWriter h(SER_NETWORK, 0);
            h << llmq_params.type;
            h << commitments[i].quorumHash;
            h << selectionHash;
            scores.emplace_back(h.GetHash(), i);
        }
        std::sort(scores.begin(), scores.end());
        return scores.front().second;
    }
}

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
    const uint256& quorumHash,
    const CBlock* pBlock,
    CommitmentHashCache* pHashCache) const
{
    if (pindex == nullptr || pindex->pprev == nullptr) {
        return std::nullopt;
    }

    // Get all active commitments UNTIL the previous block (matching CalcCbTxMerkleRootQuorums logic)
    // CalcCbTxMerkleRootQuorums uses pindexPrev, then adds commitments from the current block
    auto commitmentsMap = m_quorum_block_processor.GetMinedAndActiveCommitmentsUntilBlock(pindex->pprev);

    // Collect all commitment hashes (matching CalcCbTxMerkleRootQuorums logic)
    std::vector<uint256> commitmentHashes;
    uint256 targetCommitmentHash;
    bool targetFound = false;

    for (const auto& [type, blockIndexes] : commitmentsMap) {
        for (const auto* blockIndex : blockIndexes) {
            const uint256& blockHash = blockIndex->GetBlockHash();
            auto cacheKey = std::make_pair(type, blockHash);

            uint256 commitmentHash;

            // Check cache first to avoid DB reads
            if (pHashCache) {
                auto it = pHashCache->find(cacheKey);
                if (it != pHashCache->end()) {
                    commitmentHash = it->second;
                } else {
                    // Cache miss - fetch from DB and cache
                    commitmentHash = m_quorum_block_processor.GetMinedCommitmentTxHash(type, blockHash);
                    if (commitmentHash != uint256::ZERO) {
                        pHashCache->emplace(cacheKey, commitmentHash);
                    }
                }
            } else {
                // No cache provided - fetch directly
                commitmentHash = m_quorum_block_processor.GetMinedCommitmentTxHash(type, blockHash);
            }

            if (commitmentHash == uint256::ZERO) {
                continue;
            }

            commitmentHashes.push_back(commitmentHash);

            if (type == llmqType && blockHash == quorumHash) {
                targetCommitmentHash = commitmentHash;
                targetFound = true;
            }
        }
    }

    // Now add commitments from the current block's transactions (matching CalcCbTxMerkleRootQuorums logic)
    // This is necessary because GetMinedAndActiveCommitmentsUntilBlock uses pindexPrev
    CBlock block_local;
    const CBlock* block_ptr = pBlock;
    if (!block_ptr) {
        if (!ReadBlockFromDisk(block_local, pindex, Params().GetConsensus())) {
            return std::nullopt;
        }
        block_ptr = &block_local;
    }
    const CBlock& block = *block_ptr;

    for (size_t i = 1; i < block.vtx.size(); ++i) {
        const auto& tx = block.vtx[i];
        if (tx->IsSpecialTxVersion() && tx->nType == TRANSACTION_QUORUM_COMMITMENT) {
            const auto opt_qc = GetTxPayload<CFinalCommitmentTxPayload>(*tx);
            if (!opt_qc || opt_qc->commitment.IsNull()) {
                continue;
            }

            uint256 commitmentHash = ::SerializeHash(opt_qc->commitment);
            commitmentHashes.push_back(commitmentHash);

            if (opt_qc->commitment.llmqType == llmqType && opt_qc->commitment.quorumHash == quorumHash) {
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

int32_t CQuorumProofManager::FindChainlockSignedByKnownQuorum(
    const CBlockIndex* pMinedBlock,
    const std::set<CBLSPublicKey>& knownQuorumPubKeys,
    const CChain& active_chain,
    const CQuorumManager& qman) const
{
    if (pMinedBlock == nullptr) {
        return -1;
    }

    // Search for a chainlock that covers this block AND is signed by a known quorum
    // This is more efficient than taking any chainlock and hoping its signer is known
    // With 4 active quorums and pseudo-random selection, we expect to find a match
    // within ~4 chainlocks on average
    const int32_t maxHeight = pMinedBlock->nHeight + MAX_CHAINLOCK_SEARCH_OFFSET;
    for (int32_t height = pMinedBlock->nHeight; height <= maxHeight; ++height) {
        if (!GetChainlockByHeight(height).has_value()) {
            continue;
        }

        // Check if the signing quorum for this chainlock height is in our known set
        auto signingCommitment = DetermineChainlockSigningCommitment(height, active_chain, qman);
        if (signingCommitment && knownQuorumPubKeys.count(signingCommitment->quorumPublicKey)) {
            LogPrint(BCLog::LLMQ, "CQuorumProofManager::%s -- Found chainlock at height %d signed by known quorum %s\n",
                     __func__, height, signingCommitment->quorumHash.ToString());
            return height;
        }
    }
    return -1;
}

std::optional<CFinalCommitment> CQuorumProofManager::DetermineChainlockSigningCommitment(
    int32_t chainlockHeight,
    const CChain& active_chain,
    const CQuorumManager& qman) const
{
    // Get the chainlock LLMQ type from consensus parameters
    const auto llmqType = Params().GetConsensus().llmqTypeChainLocks;
    const auto& llmq_params_opt = Params().GetLLMQ(llmqType);
    if (!llmq_params_opt.has_value()) {
        return std::nullopt;
    }
    const auto& llmq_params = llmq_params_opt.value();

    // Generate the request ID for the chainlock at this height
    const uint256 requestId = chainlock::GenSigRequestId(chainlockHeight);

    // Use SelectCommitmentForSigning which avoids building full quorum objects
    return SelectCommitmentForSigning(llmq_params, active_chain, qman,
                                   requestId, chainlockHeight, SIGN_HEIGHT_OFFSET);
}

std::optional<QuorumProofChain> CQuorumProofManager::BuildProofChain(
    const QuorumCheckpoint& checkpoint,
    Consensus::LLMQType targetQuorumType,
    const uint256& targetQuorumHash,
    const CQuorumManager& qman,
    const CChain& active_chain,
    const node::BlockManager& block_man) const
{
    LogPrint(BCLog::LLMQ, "CQuorumProofManager::%s -- Building proof chain for quorum type=%d hash=%s\n",
             __func__, static_cast<int>(targetQuorumType), targetQuorumHash.ToString());

    // Phase 1: Build set of known chainlock quorum public keys from checkpoint
    std::set<CBLSPublicKey> knownQuorumPubKeys;
    for (const auto& q : checkpoint.chainlockQuorums) {
        knownQuorumPubKeys.insert(q.publicKey);
        LogPrint(BCLog::LLMQ, "CQuorumProofManager::%s -- Checkpoint quorum: hash=%s type=%d pubkey=%s\n",
                 __func__, q.quorumHash.ToString(), static_cast<int>(q.quorumType), q.publicKey.ToString().substr(0, 32) + "...");
    }
    LogPrint(BCLog::LLMQ, "CQuorumProofManager::%s -- Checkpoint has %d known quorum public keys\n",
             __func__, knownQuorumPubKeys.size());

    // Phase 2: Work backwards from target to find the dependency chain
    // Each ProofStep represents a quorum that needs to be proven and
    // the chainlock height that covers its mined block
    struct ProofStep {
        CFinalCommitment commitment;
        const CBlockIndex* pMinedBlockIndex;  // Block where commitment was actually mined
        int32_t chainlockHeight;
        std::optional<ChainlockIndexEntry> chainlockEntry;
    };
    std::vector<ProofStep> proofSteps;
    std::set<uint256> visitedQuorums; // Cycle detection

    // Start with the target quorum
    // For the first step, we need the full commitment object and we need to know where it was mined.
    // We use GetMinedCommitment which gives us both.
    auto [targetQc, targetMinedHash] = m_quorum_block_processor.GetMinedCommitment(targetQuorumType, targetQuorumHash);
    
    if (targetQc.IsNull()) {
        LogPrint(BCLog::LLMQ, "CQuorumProofManager::%s -- Target quorum not found\n", __func__);
        return std::nullopt;
    }
    
    CFinalCommitment currentCommitment = targetQc;
    uint256 currentMinedBlockHash = targetMinedHash;

    while (true) {
        // Cycle detection
        if (visitedQuorums.count(currentCommitment.quorumHash)) {
            return std::nullopt; // Cycle detected - invalid chain
        }
        visitedQuorums.insert(currentCommitment.quorumHash);

        // DoS protection: limit chain length
        if (proofSteps.size() >= MAX_PROOF_CHAIN_LENGTH) {
            LogPrint(BCLog::LLMQ, "CQuorumProofManager::%s -- Proof chain length limit reached (%d steps) without finding checkpoint quorum\n",
                     __func__, MAX_PROOF_CHAIN_LENGTH);
            return std::nullopt;
        }

        // Look up the block where the commitment was actually mined
        const CBlockIndex* pMinedBlock = WITH_LOCK(cs_main, return block_man.LookupBlockIndex(currentMinedBlockHash));
        if (!pMinedBlock) {
            LogPrint(BCLog::LLMQ, "CQuorumProofManager::%s -- Could not find mined block %s for quorum %s\n",
                     __func__, currentMinedBlockHash.ToString(), currentCommitment.quorumHash.ToString());
            return std::nullopt;
        }

        // OPTIMIZATION: Search for the best block to prove this quorum
        // We can use ANY block where the quorum is active, not just the mined block.
        // We prioritize:
        // 1. Blocks signed by a KNOWN quorum (direct bridge)
        // 2. Blocks signed by the oldest possible quorum (maximize jump)

        const auto& llmq_params_opt = Params().GetLLMQ(Params().GetConsensus().llmqTypeChainLocks);
        assert(llmq_params_opt.has_value());
        const auto& llmq_params = llmq_params_opt.value();
        // Quorum is active for signingActiveQuorumCount * dkgInterval blocks
        int activeDuration = std::min(llmq_params.signingActiveQuorumCount * llmq_params.dkgInterval, 100);
        int maxSearchHeight = std::min(active_chain.Height(), pMinedBlock->nHeight + activeDuration);

        int32_t bestBlockHeight = -1;
        int32_t bestChainlockHeight = -1;
        size_t bestSignerIndex = 0;
        std::optional<ChainlockIndexEntry> bestChainlockEntry;

        // Metrics for selection
        bool foundKnownSigner = false;
        int32_t oldestSignerHeight = std::numeric_limits<int32_t>::max();

        // PERFORMANCE OPTIMIZATION: Fetch active commitments ONCE and cache them
        // This avoids repeated calls to ScanCommitments and GetMinedCommitment for each height
        std::vector<CachedCommitmentInfo> cachedCommitments;
        {
            // Get commitments at the start of the search window
            int refHeight = pMinedBlock->nHeight - SIGN_HEIGHT_OFFSET;
            if (refHeight < 0) refHeight = 0;
            const CBlockIndex* pRefIndex = active_chain[refHeight];
            if (!pRefIndex) {
                LogPrint(BCLog::LLMQ, "CQuorumProofManager::%s -- Could not get reference block at height %d\n",
                         __func__, refHeight);
                return std::nullopt;
            }

            // Fetch commitments once
            auto commitments = qman.ScanCommitments(llmq_params.type, pRefIndex, llmq_params.signingActiveQuorumCount);
            if (commitments.empty()) {
                LogPrint(BCLog::LLMQ, "CQuorumProofManager::%s -- No active commitments found at height %d\n",
                         __func__, refHeight);
                return std::nullopt;
            }

            // Build cached info with mined block pointers
            cachedCommitments.reserve(commitments.size());
            for (const auto& qc : commitments) {
                CachedCommitmentInfo info;
                info.quorumHash = qc.quorumHash;
                info.publicKey = qc.quorumPublicKey;
                info.quorumIndex = qc.quorumIndex;
                info.llmqType = qc.llmqType;

                // Get mined block (single DB read per commitment, not per height)
                uint256 minedBlockHash = m_quorum_block_processor.GetMinedCommitmentBlockHash(qc.llmqType, qc.quorumHash);
                info.pMinedBlock = WITH_LOCK(cs_main, return block_man.LookupBlockIndex(minedBlockHash));
                if (!info.pMinedBlock) continue;
                cachedCommitments.push_back(std::move(info));
            }

            if (cachedCommitments.empty()) {
                LogPrint(BCLog::LLMQ, "CQuorumProofManager::%s -- Could not resolve mined blocks for commitments\n", __func__);
                return std::nullopt;
            }
        }

        // Search window using cached data - no DB reads in the inner loop
        for (int32_t h = pMinedBlock->nHeight; h <= maxSearchHeight; ++h) {
            // Compute which commitment would sign this height using cached data
            const uint256 requestId = chainlock::GenSigRequestId(h);
            size_t signerIdx = ComputeSigningCommitmentIndex(llmq_params, cachedCommitments, requestId);
            const auto& signer = cachedCommitments[signerIdx];

            bool isKnown = knownQuorumPubKeys.count(signer.publicKey);
            int32_t signerHeight = signer.pMinedBlock->nHeight;

            // Skip if this signer is not interesting
            bool isInteresting = isKnown || bestBlockHeight == -1 || signerHeight < oldestSignerHeight;
            if (!isInteresting) continue;

            // Get chainlock for this height (DB read, but only for interesting heights)
            auto clEntry = GetChainlockByHeight(h);
            if (!clEntry.has_value()) continue;

            if (isKnown) {
                // Found a direct bridge!
                bestBlockHeight = h;
                bestChainlockHeight = h;
                bestSignerIndex = signerIdx;
                bestChainlockEntry = clEntry;
                foundKnownSigner = true;
                break;
            } else {
                // Not known. Pick the oldest signer to maximize the jump back.
                if (bestBlockHeight == -1 || signerHeight < oldestSignerHeight) {
                    bestBlockHeight = h;
                    bestChainlockHeight = h;
                    bestSignerIndex = signerIdx;
                    bestChainlockEntry = clEntry;
                    oldestSignerHeight = signerHeight;
                }
            }
        }

        if (bestBlockHeight == -1) {
             LogPrint(BCLog::LLMQ, "CQuorumProofManager::%s -- No suitable chainlock found in active window [%d, %d]\n",
                     __func__, pMinedBlock->nHeight, maxSearchHeight);
             return std::nullopt;
        }

        const CBlockIndex* pProofBlock = active_chain[bestBlockHeight];
        LogPrint(BCLog::LLMQ, "CQuorumProofManager::%s -- Selected proof block %d (mined %d). KnownSigner=%d\n",
                 __func__, bestBlockHeight, pMinedBlock->nHeight, foundKnownSigner);

        proofSteps.push_back({currentCommitment, pProofBlock, bestChainlockHeight, bestChainlockEntry});

        if (foundKnownSigner) {
             LogPrint(BCLog::LLMQ, "CQuorumProofManager::%s -- Signing quorum's public key is in checkpoint - chain complete!\n", __func__);
             break;
        }

        // Need to prove the signer - fetch full commitment now (only one DB read per step)
        const auto& bestSigner = cachedCommitments[bestSignerIndex];
        auto [signerCommitment, signerMinedHash] = m_quorum_block_processor.GetMinedCommitment(bestSigner.llmqType, bestSigner.quorumHash);
        if (signerCommitment.IsNull()) {
            LogPrint(BCLog::LLMQ, "CQuorumProofManager::%s -- Could not fetch commitment for signer %s\n",
                     __func__, bestSigner.quorumHash.ToString());
            return std::nullopt;
        }
        currentCommitment = std::move(signerCommitment);
        currentMinedBlockHash = signerMinedHash;
    }

    // Phase 3: Build proofs in forward order (reverse the dependency chain)
    std::reverse(proofSteps.begin(), proofSteps.end());

    // Phase 4: Construct the QuorumProofChain
    QuorumProofChain chain;
    std::set<int32_t> includedChainlockHeights;

    // Cache commitment hashes across proof steps to avoid repeated DB reads
    // Consecutive steps often share many of the same active commitments
    CommitmentHashCache commitmentHashCache;

    for (const auto& step : proofSteps) {
        // Add chainlock entry if not already included
        if (!includedChainlockHeights.count(step.chainlockHeight)) {
            std::optional<ChainlockIndexEntry> clEntry = step.chainlockEntry;
            if (!clEntry.has_value()) {
                clEntry = GetChainlockByHeight(step.chainlockHeight);
            }
            if (!clEntry.has_value()) {
                return std::nullopt;
            }

            // Get the block at the chainlock height from the active chain
            // Note: chainlock height can be >= mined block height, so we can't use GetAncestor
            const CBlockIndex* pClBlock = active_chain[step.chainlockHeight];
            if (!pClBlock) {
                LogPrint(BCLog::LLMQ, "CQuorumProofManager::%s -- FAILED: Could not get block at chainlock height %d from active chain\n",
                         __func__, step.chainlockHeight);
                return std::nullopt;
            }

            ChainlockProofEntry clProof;
            clProof.nHeight = step.chainlockHeight;
            clProof.blockHash = pClBlock->GetBlockHash();
            clProof.signature = clEntry->signature;
            chain.chainlocks.push_back(clProof);
            includedChainlockHeights.insert(step.chainlockHeight);
        }

        // Build the quorum commitment proof - use the MINED block where the commitment is
        const CBlockIndex* pMinedBlock = step.pMinedBlockIndex;

        // Read the block to get coinbase transaction
        CBlock block;
        if (!ReadBlockFromDisk(block, pMinedBlock, Params().GetConsensus())) {
            return std::nullopt;
        }

        auto merkleProof = BuildQuorumMerkleProof(pMinedBlock, step.commitment.llmqType, step.commitment.quorumHash, &block, &commitmentHashCache);
        if (!merkleProof.has_value()) {
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
        commitmentProof.commitment = step.commitment;
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
            result.error = strprintf("Header chain is not continuous - prevBlockHash mismatch at index %d", i);
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
            result.error = strprintf("Invalid chainlock index %d", qProof.chainlockIndex);
            return result;
        }
        const auto& chainlock = proof.chainlocks[qProof.chainlockIndex];

        // Verify chainlock signature if we haven't verified this chainlock yet
        if (!verifiedChainlockHeights.count(chainlock.nHeight)) {
            if (!chainlock.signature.IsValid()) {
                result.error = strprintf("Invalid chainlock signature format at height %d", chainlock.nHeight);
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
                result.error = strprintf("Chainlock signature verification failed at height %d - signature does not match any known quorum key", chainlock.nHeight);
                return result;
            }

            verifiedChainlockHeights.insert(chainlock.nHeight);
        }

        // Get the corresponding header for this quorum proof
        const CBlockHeader& header = proof.headers[proofIdx];

        // Verify coinbase tx is in the block via merkle proof
        if (!qProof.coinbaseTx) {
            result.error = strprintf("Missing coinbase transaction in proof %d", proofIdx);
            return result;
        }

        const uint256 coinbaseTxHash = qProof.coinbaseTx->GetHash();
        QuorumMerkleProof coinbaseMerkleProof{qProof.coinbaseMerklePath, qProof.coinbaseMerklePathSide};
        if (!coinbaseMerkleProof.Verify(coinbaseTxHash, header.hashMerkleRoot)) {
            result.error = strprintf("Coinbase merkle proof verification failed in proof %d", proofIdx);
            return result;
        }

        // Extract merkleRootQuorums from cbtx
        auto opt_cbtx = GetTxPayload<CCbTx>(*qProof.coinbaseTx);
        if (!opt_cbtx.has_value()) {
            result.error = strprintf("Invalid coinbase transaction payload in proof %d", proofIdx);
            return result;
        }

        const CCbTx& cbtx = opt_cbtx.value();

        // Verify the quorum commitment merkle proof against merkleRootQuorums
        uint256 commitmentHash = ::SerializeHash(qProof.commitment);
        if (!qProof.quorumMerkleProof.Verify(commitmentHash, cbtx.merkleRootQuorums)) {
            result.error = strprintf("Quorum commitment merkle proof verification failed in proof %d", proofIdx);
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

void CQuorumProofManager::MigrateChainlockIndex(const CChain& active_chain, const CChainParams& chainparams)
{
    // Check if migration is needed
    int version{0};
    if (m_evoDb.Read(DB_CHAINLOCK_INDEX_VERSION, version) && version >= CHAINLOCK_INDEX_VERSION) {
        LogPrintf("CQuorumProofManager: Chainlock index is up to date (version %d)\n", version);
        return;
    }

    LogPrintf("CQuorumProofManager: Building chainlock index from historical blocks...\n");

    // Start from V20 activation height - chainlocks in cbtx (CLSIG_AND_BALANCE) were introduced in V20
    const int v20Height = chainparams.GetConsensus().V20Height;
    const CBlockIndex* pindex = active_chain[v20Height];
    if (!pindex) {
        // V20 not yet reached, nothing to migrate
        LogPrintf("CQuorumProofManager: V20 not yet active (height %d), skipping migration\n", v20Height);
        // Still write version so we don't check every startup
        m_evoDb.Write(DB_CHAINLOCK_INDEX_VERSION, CHAINLOCK_INDEX_VERSION);
        return;
    }

    int indexed_count = 0;
    int blocks_processed = 0;
    const int tip_height = active_chain.Height();
    const int total_blocks = tip_height - v20Height + 1;

    LogPrintf("CQuorumProofManager: Starting migration from V20 height %d to tip %d (%d blocks)\n",
             v20Height, tip_height, total_blocks);

    // Show initial progress in UI
    uiInterface.ShowProgress(_("Building chainlock index…").translated, 0, false);

    // Iterate through blocks from V20 activation
    while (pindex) {
        blocks_processed++;

        // Update progress every 10000 blocks
        if (blocks_processed % 10000 == 0) {
            int percentageDone = std::max(1, std::min(99, (int)((double)blocks_processed / total_blocks * 100)));
            uiInterface.ShowProgress(_("Building chainlock index…").translated, percentageDone, false);
            LogPrintf("CQuorumProofManager: Migration progress: %d/%d blocks processed (%d%%), %d chainlocks indexed\n",
                     blocks_processed, total_blocks, percentageDone, indexed_count);
        }

        // Read block from disk
        CBlock block;
        if (!ReadBlockFromDisk(block, pindex, chainparams.GetConsensus())) {
            LogPrintf("CQuorumProofManager: Failed to read block at height %d, skipping\n", pindex->nHeight);
            pindex = active_chain.Next(pindex);
            continue;
        }

        // Check if block has transactions
        if (block.vtx.empty()) {
            pindex = active_chain.Next(pindex);
            continue;
        }

        // Try to extract CCbTx from coinbase
        auto opt_cbtx = GetTxPayload<CCbTx>(*block.vtx[0]);
        if (!opt_cbtx.has_value()) {
            pindex = active_chain.Next(pindex);
            continue;
        }

        const CCbTx& cbtx = opt_cbtx.value();

        // Check if this cbtx contains a chainlock signature
        if (cbtx.bestCLSignature.IsValid()) {
            // Calculate the chainlocked height
            int32_t chainlockedHeight = pindex->nHeight - static_cast<int32_t>(cbtx.bestCLHeightDiff) - 1;
            const CBlockIndex* pChainlockedBlock = pindex->GetAncestor(chainlockedHeight);

            if (pChainlockedBlock) {
                IndexChainlock(
                    chainlockedHeight,
                    pChainlockedBlock->GetBlockHash(),
                    cbtx.bestCLSignature,
                    pindex->GetBlockHash(),
                    pindex->nHeight);
                indexed_count++;
            }
        }

        pindex = active_chain.Next(pindex);
    }

    // Write version to mark migration complete
    m_evoDb.Write(DB_CHAINLOCK_INDEX_VERSION, CHAINLOCK_INDEX_VERSION);

    // Hide progress indicator
    uiInterface.ShowProgress("", 100, false);

    LogPrintf("CQuorumProofManager: Chainlock index migration complete. Processed %d blocks, indexed %d chainlocks\n",
             blocks_processed, indexed_count);
}

} // namespace llmq