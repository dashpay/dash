// Copyright (c) 2017-2025 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <evo/cbtx.h>

#include <evo/evodb.h>
#include <evo/snapshot_types.h>
#include <evo/specialtx.h>
#include <llmq/blockprocessor.h>
#include <llmq/commitment.h>
#include <llmq/options.h>
#include <llmq/quorumsman.h>
#include <llmq/utils.h>
#include <util/std23.h>

#include <chain.h>
#include <chainparams.h>
#include <consensus/merkle.h>
#include <consensus/validation.h>
#include <deploymentstatus.h>
#include <node/blockstorage.h>

using node::ReadBlockFromDisk;

bool CheckCbTx(const CCbTx& cbTx, const CBlockIndex* pindexPrev, bool is_v24_active, TxValidationState& state)
{
    if (cbTx.nVersion == CCbTx::Version::INVALID || cbTx.nVersion >= CCbTx::Version::UNKNOWN) {
        return state.Invalid(TxValidationResult::TX_CONSENSUS, "bad-cbtx-version");
    }

    if (pindexPrev) {
        if (pindexPrev->nHeight + 1 != cbTx.nHeight) {
            return state.Invalid(TxValidationResult::TX_CONSENSUS, "bad-cbtx-height");
        }

        const bool fDIP0008Active{DeploymentActiveAt(*pindexPrev, Params().GetConsensus(), Consensus::DEPLOYMENT_DIP0008)};
        if (fDIP0008Active && cbTx.nVersion < CCbTx::Version::MERKLE_ROOT_QUORUMS) {
            return state.Invalid(TxValidationResult::TX_CONSENSUS, "bad-cbtx-version");
        }

        const bool isV20{DeploymentActiveAfter(pindexPrev, Params().GetConsensus(), Consensus::DEPLOYMENT_V20)};
        if ((isV20 && cbTx.nVersion < CCbTx::Version::CLSIG_AND_BALANCE) || (!isV20 && cbTx.nVersion >= CCbTx::Version::CLSIG_AND_BALANCE)) {
            return state.Invalid(TxValidationResult::TX_CONSENSUS, "bad-cbtx-version");
        }

        // The asset unlock commitment extends the version 3 fields, so it is only required once
        // both forks are active (tests may activate v24 on a chain where v20 never activates).
        const bool requires_unlock_root{is_v24_active && isV20};
        if ((requires_unlock_root && cbTx.nVersion < CCbTx::Version::MERKLE_ROOT_ASSETUNLOCKS) ||
            (!requires_unlock_root && cbTx.nVersion >= CCbTx::Version::MERKLE_ROOT_ASSETUNLOCKS)) {
            return state.Invalid(TxValidationResult::TX_CONSENSUS, "bad-cbtx-version");
        }
    }

    // The signature is deserialized lazily. Get() decodes it in the active scheme only and
    // resets the wrapper on failure, which is what the eager parser did with bytes encoded
    // in the opposite scheme; bytes that decode in neither scheme made it throw, so they
    // are rejected here.
    if (cbTx.nVersion >= CCbTx::Version::CLSIG_AND_BALANCE) {
        CDataStream ss(SER_NETWORK, PROTOCOL_VERSION);
        ss << cbTx.bestCLSignature;
        if (!cbTx.bestCLSignature.Get().IsValid()) {
            try {
                CBLSSignature sig;
                ss >> sig;
            } catch (const std::ios_base::failure&) {
                return state.Invalid(TxValidationResult::TX_CONSENSUS, "bad-cbtx-payload");
            }
        }
    }

    return true;
}

using llmq::QcHashMap;

auto CalcHashCountFromQCHashes(const QcHashMap& qcHashes)
{
    return std23::ranges::fold_left(qcHashes, size_t{0}, [](size_t s, const auto& p) { return s + p.second.size(); });
}

bool CalcCbTxMerkleRootQuorums(const CBlock& block, const CBlockIndex* pindexPrev,
                               const llmq::CQuorumBlockProcessor& quorum_block_processor, uint256& merkleRootRet,
                               BlockValidationState& state)
{
    static int64_t nTimeMined = 0;
    static int64_t nTimeLoop = 0;
    static int64_t nTimeMerkle = 0;

    int64_t nTime1 = GetTimeMicros();

    auto retVal = quorum_block_processor.GetQcHashes(pindexPrev);
    if (!retVal) {
        return state.Invalid(BlockValidationResult::BLOCK_CONSENSUS, "commitment-not-found");
    }
    // The returned quorums are in reversed order, so the most recent one is at index 0
    auto [qcHashes, qcIndexedHashes] = retVal.value();

    int64_t nTime2 = GetTimeMicros(); nTimeMined += nTime2 - nTime1;
    LogPrint(BCLog::BENCHMARK, "            - GetQcHashes: %.2fms [%.2fs]\n", 0.001 * (nTime2 - nTime1), nTimeMined * 0.000001);

    // now add the commitments from the current block, which are not returned by GetMinedAndActiveCommitmentsUntilBlock
    // due to the use of pindexPrev (we don't have the tip index here)
    for (size_t i = 1; i < block.vtx.size(); i++) {
        const auto& tx = block.vtx[i];

        if (tx->IsSpecialTxVersion() && tx->nType == TRANSACTION_QUORUM_COMMITMENT) {
            const auto opt_qc = quorum_block_processor.GetCommitmentTxPayload(*tx);
            if (!opt_qc) {
                return state.Invalid(BlockValidationResult::BLOCK_CONSENSUS, "bad-qc-payload-calc-cbtx-quorummerkleroot");
            }
            if (opt_qc->commitment.IsNull()) {
                // having null commitments is ok but we don't use them here, move to the next tx
                continue;
            }
            const auto& llmq_params_opt = Params().GetLLMQ(opt_qc->commitment.llmqType);
            if (!llmq_params_opt.has_value()) {
                return state.Invalid(BlockValidationResult::BLOCK_CONSENSUS, "bad-qc-commitment-type-calc-cbtx-quorummerkleroot");
            }
            const auto& llmq_params = llmq_params_opt.value();
            const auto qcHash = ::SerializeHash(opt_qc->commitment);
            if (llmq::IsQuorumRotationEnabled(llmq_params, pindexPrev)) {
                auto& map_indexed_hashes = qcIndexedHashes[opt_qc->commitment.llmqType];
                map_indexed_hashes[opt_qc->commitment.quorumIndex] = qcHash;
            } else {
                auto& vec_hashes = qcHashes[llmq_params.type];
                if (vec_hashes.size() == size_t(llmq_params.signingActiveQuorumCount)) {
                    // we pop the last entry, which is actually the oldest quorum as GetMinedAndActiveCommitmentsUntilBlock
                    // returned quorums in reversed order. This pop and later push can only work ONCE, but we rely on the
                    // fact that a block can only contain a single commitment for one LLMQ type
                    vec_hashes.pop_back();
                }
                vec_hashes.emplace_back(qcHash);
            }
        }
    }

    for (const auto& [llmqType, map_indexed_hashes] : qcIndexedHashes) {
        auto& vec_hashes = qcHashes[llmqType];
        for (const auto& [_, hash] : map_indexed_hashes) {
            vec_hashes.emplace_back(hash);
        }
    }

    std::vector<uint256> vec_hashes_final;
    vec_hashes_final.reserve(CalcHashCountFromQCHashes(qcHashes));

    for (const auto& [llmqType, vec_hashes] : qcHashes) {
        const auto& llmq_params_opt = Params().GetLLMQ(llmqType);
        assert(llmq_params_opt.has_value());
        if (vec_hashes.size() > size_t(llmq_params_opt->signingActiveQuorumCount)) {
            return state.Invalid(BlockValidationResult::BLOCK_CONSENSUS, "excess-quorums-calc-cbtx-quorummerkleroot");
        }
        // Copy vec_hashes into vec_hashes_final
        std::copy(vec_hashes.begin(), vec_hashes.end(), std::back_inserter(vec_hashes_final));
    }
    std::sort(vec_hashes_final.begin(), vec_hashes_final.end());

    int64_t nTime3 = GetTimeMicros(); nTimeLoop += nTime3 - nTime2;
    LogPrint(BCLog::BENCHMARK, "            - Loop: %.2fms [%.2fs]\n", 0.001 * (nTime3 - nTime2), nTimeLoop * 0.000001);

    bool mutated = false;
    merkleRootRet = ComputeMerkleRoot(vec_hashes_final, &mutated);

    int64_t nTime4 = GetTimeMicros(); nTimeMerkle += nTime4 - nTime3;
    LogPrint(BCLog::BENCHMARK, "            - ComputeMerkleRoot: %.2fms [%.2fs]\n", 0.001 * (nTime4 - nTime3), nTimeMerkle * 0.000001);

    if (mutated) {
        return state.Invalid(BlockValidationResult::BLOCK_CONSENSUS, "mutated-calc-cbtx-quorummerkleroot");
    }

    return true;
}

uint256 CalcCbTxMerkleRootAssetUnlocks(const CBlock& block)
{
    // Instance hashes cover the quorum signing info that the txids of these transactions - and
    // therefore the block's merkle root - exclude. Two instances of one withdrawal share a txid,
    // so duplicate leaves imply a duplicate transaction, which the block merkle-root check
    // (CheckMerkleRoot, run before this) already rejects; no mutated check is needed here.
    std::vector<uint256> instance_hashes;
    for (const auto& tx : block.vtx) {
        // The miner calls this while the coinbase slot is still an empty placeholder
        if (tx && IsAssetUnlockWithStableTxid(*tx)) {
            instance_hashes.push_back(tx->GetInstanceHash());
        }
    }
    return ComputeMerkleRoot(std::move(instance_hashes));
}

std::string CCbTx::ToString() const
{
    return strprintf("CCbTx(nVersion=%d, nHeight=%d, merkleRootMNList=%s, merkleRootQuorums=%s, bestCLHeightDiff=%d, bestCLSig=%s, creditPoolBalance=%d.%08d, merkleRootAssetUnlocks=%s)",
        static_cast<uint16_t>(nVersion), nHeight, merkleRootMNList.ToString(), merkleRootQuorums.ToString(), bestCLHeightDiff, bestCLSignature.ToString(),
        creditPoolBalance / COIN, creditPoolBalance % COIN, merkleRootAssetUnlocks.ToString());
}

std::optional<CCbTx> DecodeCbTxPayload(const CTransaction& tx, bool legacy_bls_scheme)
{
    if (!tx.IsCoinBase() || tx.nType != TRANSACTION_COINBASE) return std::nullopt;
    CDataStream ds(tx.vExtraPayload, SER_NETWORK, PROTOCOL_VERSION);
    CCbTx cbtx;
    try {
        ds >> cbtx.nVersion >> cbtx.nHeight >> cbtx.merkleRootMNList;
        if (cbtx.nVersion >= CCbTx::Version::MERKLE_ROOT_QUORUMS) {
            ds >> cbtx.merkleRootQuorums;
            if (cbtx.nVersion >= CCbTx::Version::CLSIG_AND_BALANCE) {
                ds >> COMPACTSIZE(cbtx.bestCLHeightDiff);
                // Decoded eagerly: bytes valid in neither scheme reject the payload.
                CBLSSignature signature;
                ds >> CBLSSignatureVersionWrapper(signature, legacy_bls_scheme);
                cbtx.bestCLSignature.Set(signature, legacy_bls_scheme);
                ds >> cbtx.creditPoolBalance;
                if (cbtx.nVersion >= CCbTx::Version::MERKLE_ROOT_ASSETUNLOCKS) ds >> cbtx.merkleRootAssetUnlocks;
            }
        }
    } catch (const std::exception&) {
        return std::nullopt;
    }
    if (!ds.empty()) return std::nullopt;
    return cbtx;
}

std::optional<std::pair<CBLSSignature, uint32_t>> GetNonNullCoinbaseChainlock(const CBlockIndex* pindex)
{
    if (pindex == nullptr) {
        return std::nullopt;
    }

    // There's no CL in CbTx before v20 activation
    if (!DeploymentActiveAt(*pindex, Params().GetConsensus(), Consensus::DEPLOYMENT_V20)) {
        return std::nullopt;
    }

    CBlock block;
    if (!ReadBlockFromDisk(block, pindex, Params().GetConsensus())) {
        // A snapshot view lacks the bodies at or below its base, where a
        // coinbase ChainLock that cannot be read cannot be told from none.
        if (const auto view{EvoViewScope::InnermostAny()};
            view && view->identity == EvoDbIdentity::SNAPSHOT && pindex->nHeight <= view->base_height &&
            !WITH_LOCK(::cs_main, return pindex->nStatus & BLOCK_HAVE_DATA)) {
            throw BlockDataUnavailableError(
                strprintf("coinbase of block %s at or below the snapshot base is not available",
                          pindex->GetBlockHash().ToString()));
        }
        return std::nullopt;
    }

    return GetNonNullCoinbaseChainlock(block, *pindex, Params().GetConsensus());
}

std::optional<std::pair<CBLSSignature, uint32_t>> GetNonNullCoinbaseChainlock(const CBlock& block, const CBlockIndex& index,
                                                                              const Consensus::Params& consensus)
{
    if (block.vtx.empty()) return std::nullopt;
    // The signature encoding is the block's own scheme, not the one this node
    // happens to be in (a background chainstate connects pre-v19 blocks in
    // the legacy scheme while other threads read v19 blocks).
    const auto payload{DecodeCbTxPayload(*block.vtx[0], !DeploymentActiveAt(index, consensus, Consensus::DEPLOYMENT_V19))};
    if (!payload || payload->nVersion < CCbTx::Version::CLSIG_AND_BALANCE || payload->nHeight != index.nHeight ||
        !payload->bestCLSignature.Get().IsValid() || payload->bestCLHeightDiff >= static_cast<uint32_t>(index.nHeight)) {
        return std::nullopt;
    }
    return std::make_pair(payload->bestCLSignature.Get(), payload->bestCLHeightDiff);
}
