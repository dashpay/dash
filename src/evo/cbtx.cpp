// Copyright (c) 2017-2025 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <evo/cbtx.h>

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

        if ((is_v24_active && cbTx.nVersion < CCbTx::Version::MERKLE_ROOT_ASSETUNLOCKS) ||
            (!is_v24_active && cbTx.nVersion >= CCbTx::Version::MERKLE_ROOT_ASSETUNLOCKS)) {
            return state.Invalid(TxValidationResult::TX_CONSENSUS, "bad-cbtx-version");
        }
    }

    return true;
}

using llmq::QcHashMap;
using llmq::QcIndexedHashMap;

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
            const auto opt_qc = GetTxPayload<llmq::CFinalCommitmentTxPayload>(*tx);
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
        return std::nullopt;
    }

    const CTransactionRef cbTx = block.vtx[0];
    const auto opt_cbtx = GetTxPayload<CCbTx>(*cbTx);

    if (!opt_cbtx.has_value()) {
        return std::nullopt;
    }

    if (!opt_cbtx->bestCLSignature.IsValid()) {
        return std::nullopt;
    }

    return std::make_pair(opt_cbtx->bestCLSignature, opt_cbtx->bestCLHeightDiff);
}
