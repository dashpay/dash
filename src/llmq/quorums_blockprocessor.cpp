// Copyright (c) 2018 The Dash Core developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "quorums_blockprocessor.h"
#include "quorums_commitment.h"
#include "quorums_utils.h"

#include "evo/specialtx.h"

#include "chain.h"
#include "chainparams.h"
#include "consensus/validation.h"
#include "primitives/block.h"
#include "validation.h"

namespace llmq
{

CQuorumBlockProcessor* quorumBlockProcessor;

static const std::string DB_MINED_COMMITMENT = "q_mc";

bool CQuorumBlockProcessor::ProcessBlock(const CBlock& block, const CBlockIndex* pindexPrev, CValidationState& state)
{
    AssertLockHeld(cs_main);

    bool fDIP0003Active = VersionBitsState(pindexPrev, Params().GetConsensus(), Consensus::DEPLOYMENT_DIP0003, versionbitscache) == THRESHOLD_ACTIVE;
    if (!fDIP0003Active) {
        return true;
    }

    int nHeight = pindexPrev->nHeight + 1;

    std::map<Consensus::LLMQType, CFinalCommitment> qcs;
    if (!GetCommitmentsFromBlock(block, pindexPrev, qcs, state)) {
        return false;
    }

    for (const auto& p : Params().GetConsensus().llmqs) {
        auto type = p.first;

        uint256 quorumHash = GetQuorumBlockHash(type, pindexPrev);

        if (!quorumHash.IsNull() && IsMiningPhase(type, nHeight)) {
            if (HasMinedCommitment(type, quorumHash)) {
                if (qcs.count(type)) {
                    // If in the current mining phase a previous block already mined a non-null commitment and the new
                    // block contains another (null or non-null) commitment, the block should be rejected.
                    return state.DoS(100, false, REJECT_INVALID, "bad-qc-already-mined");
                }
            } else {
                if (!qcs.count(type)) {
                    // If no final commitment was mined for the current DKG yet and the new block does not include
                    // a (possibly null) commitment, the block should be rejected.
                    return state.DoS(100, false, REJECT_INVALID, "bad-qc-missing");
                }
            }
        } else {
            if (qcs.count(type)) {
                // If the DKG is not in the mining phase and the new block contains a (null or non-null) commitment,
                // the block should be rejected.
                return state.DoS(100, false, REJECT_INVALID, "bad-qc-not-mining-phase");
            }
        }
    }

    for (auto& p : qcs) {
        auto& qc = p.second;
        if (!ProcessCommitment(pindexPrev, qc, state)) {
            return false;
        }
    }
    return true;
}

bool CQuorumBlockProcessor::ProcessCommitment(const CBlockIndex* pindexPrev, const CFinalCommitment& qc, CValidationState& state)
{
    auto& params = Params().GetConsensus().llmqs.at((Consensus::LLMQType)qc.llmqType);

    uint256 quorumHash = GetQuorumBlockHash((Consensus::LLMQType)qc.llmqType, pindexPrev);
    if (quorumHash.IsNull()) {
        return state.DoS(100, false, REJECT_INVALID, "bad-qc-block");
    }
    if (quorumHash != qc.quorumHash) {
        return state.DoS(100, false, REJECT_INVALID, "bad-qc-block");
    }

    if (qc.IsNull()) {
        if (!qc.VerifyNull()) {
            return state.DoS(100, false, REJECT_INVALID, "bad-qc-invalid-null");
        }
        return true;
    }

    if (HasMinedCommitment(params.type, quorumHash)) {
        // should not happen as it's already handled in ProcessBlock
        return state.DoS(100, false, REJECT_INVALID, "bad-qc-dup");
    }

    if (!IsMiningPhase(params.type, pindexPrev->nHeight + 1)) {
        // should not happen as it's already handled in ProcessBlock
        return state.DoS(100, false, REJECT_INVALID, "bad-qc-height");
    }

    auto members = CLLMQUtils::GetAllQuorumMembers(params.type, quorumHash);

    if (!qc.Verify(members)) {
        return state.DoS(100, false, REJECT_INVALID, "bad-qc-invalid");
    }

    // Store commitment in DB
    evoDb.Write(std::make_pair(DB_MINED_COMMITMENT, std::make_pair((uint8_t)params.type, quorumHash)), qc);

    LogPrintf("CQuorumBlockProcessor::%s -- processed commitment from block. type=%d, quorumHash=%s, signers=%s, validMembers=%d, quorumPublicKey=%s\n", __func__,
              qc.llmqType, quorumHash.ToString(), qc.CountSigners(), qc.CountValidMembers(), qc.quorumPublicKey.ToString());

    return true;
}

bool CQuorumBlockProcessor::UndoBlock(const CBlock& block, const CBlockIndex* pindex)
{
    AssertLockHeld(cs_main);

    std::map<Consensus::LLMQType, CFinalCommitment> qcs;
    CValidationState dummy;
    if (!GetCommitmentsFromBlock(block, pindex->pprev, qcs, dummy)) {
        return false;
    }

    for (auto& p : qcs) {
        auto& qc = p.second;
        if (qc.IsNull()) {
            continue;
        }

        evoDb.Erase(std::make_pair(DB_MINED_COMMITMENT, std::make_pair(qc.llmqType, qc.quorumHash)));

        if (!qc.IsNull()) {
            // if a reorg happened, we should allow to mine this commitment later
            AddMinableCommitment(qc);
        }
    }

    return true;
}

bool CQuorumBlockProcessor::GetCommitmentsFromBlock(const CBlock& block, const CBlockIndex* pindexPrev, std::map<Consensus::LLMQType, CFinalCommitment>& ret, CValidationState& state)
{
    AssertLockHeld(cs_main);

    auto& consensus = Params().GetConsensus();
    bool fDIP0003Active = VersionBitsState(pindexPrev, consensus, Consensus::DEPLOYMENT_DIP0003, versionbitscache) == THRESHOLD_ACTIVE;

    ret.clear();

    for (const auto& tx : block.vtx) {
        if (tx->nType == TRANSACTION_QUORUM_COMMITMENT) {
            CFinalCommitment qc;
            if (!GetTxPayload(*tx, qc)) {
                return state.DoS(100, false, REJECT_INVALID, "bad-tx-payload");
            }

            if (!consensus.llmqs.count((Consensus::LLMQType)qc.llmqType)) {
                return state.DoS(100, false, REJECT_INVALID, "bad-qc-type");
            }

            // only allow one commitment per type and per block
            if (ret.count((Consensus::LLMQType)qc.llmqType)) {
                return state.DoS(100, false, REJECT_INVALID, "bad-qc-dup");
            }

            ret.emplace((Consensus::LLMQType)qc.llmqType, std::move(qc));
        }
    }

    if (!fDIP0003Active && !ret.empty()) {
        return state.DoS(100, false, REJECT_INVALID, "bad-qc-premature");
    }

    return true;
}

bool CQuorumBlockProcessor::IsMiningPhase(Consensus::LLMQType llmqType, int nHeight)
{
    const auto& params = Params().GetConsensus().llmqs.at(llmqType);
    int phaseIndex = nHeight % params.dkgInterval;
    if (phaseIndex >= params.dkgMiningWindowStart && phaseIndex <= params.dkgMiningWindowEnd) {
        return true;
    }
    return false;
}

// WARNING: This method returns uint256() on the first block of the DKG interval (because the block hash is not known yet)
uint256 CQuorumBlockProcessor::GetQuorumBlockHash(Consensus::LLMQType llmqType, const CBlockIndex* pindexPrev)
{
    auto& params = Params().GetConsensus().llmqs.at(llmqType);

    int nHeight = pindexPrev->nHeight + 1;
    int quorumStartHeight = nHeight - (nHeight % params.dkgInterval);
    if (quorumStartHeight >= pindexPrev->nHeight) {
        return uint256();
    }
    auto quorumIndex = pindexPrev->GetAncestor(quorumStartHeight);
    assert(quorumIndex);
    return quorumIndex->GetBlockHash();
}

bool CQuorumBlockProcessor::HasMinedCommitment(Consensus::LLMQType llmqType, const uint256& quorumHash)
{
    auto key = std::make_pair(DB_MINED_COMMITMENT, std::make_pair((uint8_t)llmqType, quorumHash));
    return evoDb.Exists(key);
}

bool CQuorumBlockProcessor::GetMinedCommitment(Consensus::LLMQType llmqType, const uint256& quorumHash, CFinalCommitment& ret)
{
    auto key = std::make_pair(DB_MINED_COMMITMENT, std::make_pair((uint8_t)llmqType, quorumHash));
    return evoDb.Read(key, ret);
}

bool CQuorumBlockProcessor::HasMinableCommitment(const uint256& hash)
{
    LOCK(minableCommitmentsCs);
    return minableCommitments.count(hash) != 0;
}

void CQuorumBlockProcessor::AddMinableCommitment(const CFinalCommitment& fqc)
{
    uint256 commitmentHash = ::SerializeHash(fqc);

    {
        LOCK(minableCommitmentsCs);

        auto k = std::make_pair((Consensus::LLMQType) fqc.llmqType, fqc.quorumHash);
        auto ins = minableCommitmentsByQuorum.emplace(k, commitmentHash);
        if (ins.second) {
            minableCommitments.emplace(commitmentHash, fqc);
        } else {
            auto& oldFqc = minableCommitments.at(ins.first->second);
            if (fqc.CountSigners() > oldFqc.CountSigners()) {
                // new commitment has more signers, so override the known one
                ins.first->second = commitmentHash;
                minableCommitments.erase(ins.first->second);
                minableCommitments.emplace(commitmentHash, fqc);
            }
        }
    }
}

bool CQuorumBlockProcessor::GetMinableCommitmentByHash(const uint256& commitmentHash, llmq::CFinalCommitment& ret)
{
    LOCK(minableCommitmentsCs);
    auto it = minableCommitments.find(commitmentHash);
    if (it == minableCommitments.end()) {
        return false;
    }
    ret = it->second;
    return true;
}

// Will return false if no commitment should be mined
// Will return true and a null commitment if no minable commitment is known and none was mined yet
bool CQuorumBlockProcessor::GetMinableCommitment(Consensus::LLMQType llmqType, const CBlockIndex* pindexPrev, CFinalCommitment& ret)
{
    AssertLockHeld(cs_main);

    int nHeight = pindexPrev->nHeight + 1;

    uint256 quorumHash = GetQuorumBlockHash(llmqType, pindexPrev);
    if (quorumHash.IsNull()) {
        return false;
    }

    if (!IsMiningPhase(llmqType, nHeight)) {
        // no commitment required for next block
        return false;
    }

    if (HasMinedCommitment(llmqType, quorumHash)) {
        // a non-null commitment has already been mined for a previous block
        return false;
    }

    LOCK(minableCommitmentsCs);

    auto k = std::make_pair(llmqType, quorumHash);
    auto it = minableCommitmentsByQuorum.find(k);
    if (it == minableCommitmentsByQuorum.end()) {
        // null commitment required
        ret = CFinalCommitment(Params().GetConsensus().llmqs.at(llmqType), quorumHash);
        return true;
    }

    ret = minableCommitments.at(it->second);

    return true;
}

bool CQuorumBlockProcessor::GetMinableCommitmentTx(Consensus::LLMQType llmqType, const CBlockIndex* pindexPrev, CTransactionRef& ret)
{
    AssertLockHeld(cs_main);

    CFinalCommitment qc;
    if (!GetMinableCommitment(llmqType, pindexPrev, qc)) {
        return false;
    }

    CMutableTransaction tx;
    tx.nVersion = 3;
    tx.nType = TRANSACTION_QUORUM_COMMITMENT;
    SetTxPayload(tx, qc);

    ret = MakeTransactionRef(tx);

    return true;
}

}
