// Copyright (c) 2018-2025 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <active/dkgsessionhandler.h>

#include <active/dkgsession.h>
#include <active/masternode.h>
#include <llmq/debug.h>
#include <llmq/dkgsession.h>
#include <llmq/net_quorum.h>

#include <chainparams.h>
#include <deploymentstatus.h>
#include <logging.h>
#include <util/time.h>

namespace llmq {
ActiveDKGSessionHandler::ActiveDKGSessionHandler(
    CBLSWorker& bls_worker, CDeterministicMNManager& dmnman, CMasternodeMetaMan& mn_metaman,
    llmq::CDKGDebugManager& dkgdbgman, llmq::CDKGSessionManager& qdkgsman, llmq::CQuorumBlockProcessor& qblockman,
    llmq::CQuorumSnapshotManager& qsnapman, const CActiveMasternodeManager& mn_activeman, const ChainstateManager& chainman,
    const CSporkManager& sporkman, const Consensus::LLMQParams& llmq_params, bool quorums_watch, int quorums_idx) :
    llmq::CDKGSessionHandler(llmq_params),
    m_bls_worker{bls_worker},
    m_dmnman{dmnman},
    m_mn_metaman{mn_metaman},
    m_dkgdbgman{dkgdbgman},
    m_qdkgsman{qdkgsman},
    m_qblockman{qblockman},
    m_qsnapman{qsnapman},
    m_mn_activeman{mn_activeman},
    m_chainman{chainman},
    m_sporkman{sporkman},
    m_quorums_watch{quorums_watch},
    quorumIndex{quorums_idx}
{
}

ActiveDKGSessionHandler::~ActiveDKGSessionHandler() = default;

void ActiveDKGSessionHandler::UpdatedBlockTip(const CBlockIndex* pindexNew)
{
    //AssertLockNotHeld(cs_main);
    //Indexed quorums (greater than 0) are enabled with Quorum Rotation
    if (quorumIndex > 0 && !IsQuorumRotationEnabled(params, pindexNew)) {
        return;
    }
    LOCK(cs_phase_qhash);

    int quorumStageInt = (pindexNew->nHeight - quorumIndex) % params.dkgInterval;

    const CBlockIndex* pQuorumBaseBlockIndex = pindexNew->GetAncestor(pindexNew->nHeight - quorumStageInt);

    currentHeight = pindexNew->nHeight;
    quorumHash = pQuorumBaseBlockIndex->GetBlockHash();

    bool fNewPhase = (quorumStageInt % params.dkgPhaseBlocks) == 0;
    int phaseInt = quorumStageInt / params.dkgPhaseBlocks + 1;
    QuorumPhase oldPhase = phase;
    if (fNewPhase && phaseInt >= std23::to_underlying(QuorumPhase::Initialized) && phaseInt <= std23::to_underlying(QuorumPhase::Idle)) {
        phase = static_cast<QuorumPhase>(phaseInt);
    }

    LogPrint(BCLog::LLMQ_DKG, "ActiveDKGSessionHandler::%s -- %s qi[%d] currentHeight=%d, pQuorumBaseBlockIndex->nHeight=%d, oldPhase=%d, newPhase=%d\n", __func__,
             params.name, quorumIndex, currentHeight, pQuorumBaseBlockIndex->nHeight, std23::to_underlying(oldPhase), std23::to_underlying(phase));
}

uint256 ActiveDKGSessionHandler::GetCurrentQuorumHash() const { return WITH_LOCK(cs_phase_qhash, return quorumHash); }

std::pair<QuorumPhase, uint256> ActiveDKGSessionHandler::GetPhaseAndQuorumHash() const
{
    LOCK(cs_phase_qhash);
    return std::make_pair(phase, quorumHash);
}

bool ActiveDKGSessionHandler::InitNewQuorum(gsl::not_null<const CBlockIndex*> pQuorumBaseBlockIndex)
{
    if (!DeploymentDIP0003Enforced(pQuorumBaseBlockIndex->nHeight, Params().GetConsensus())) {
        return false;
    }

    curSession = std::make_unique<ActiveDKGSession>(m_bls_worker, m_dmnman, m_dkgdbgman, m_qdkgsman, m_mn_metaman,
                                                    m_qsnapman, m_mn_activeman, m_chainman, m_sporkman,
                                                    pQuorumBaseBlockIndex, params);

    if (!curSession->Init(m_mn_activeman.GetProTxHash(), quorumIndex)) {
        LogPrintf("ActiveDKGSessionHandler::%s -- height[%d] quorum initialization failed for %s qi[%d]\n", __func__,
                  pQuorumBaseBlockIndex->nHeight, params.name, quorumIndex);
        return false;
    }

    LogPrintf("ActiveDKGSessionHandler::%s -- height[%d] quorum initialization OK for %s qi[%d]\n", __func__, pQuorumBaseBlockIndex->nHeight, params.name, quorumIndex);
    return true;
}

void ActiveDKGSessionHandler::WaitForNextPhase(std::optional<QuorumPhase> curPhase, QuorumPhase nextPhase,
                                               const uint256& expectedQuorumHash, const WhileWaitFunc& shouldNotWait) const
{
    LogPrint(BCLog::LLMQ_DKG, "ActiveDKGSessionHandler::%s -- %s qi[%d] - starting, curPhase=%d, nextPhase=%d\n", __func__, params.name, quorumIndex, curPhase.has_value() ? std23::to_underlying(*curPhase) : -1, std23::to_underlying(nextPhase));

    while (true) {
        if (stopRequested) {
            LogPrint(BCLog::LLMQ_DKG, "ActiveDKGSessionHandler::%s -- %s qi[%d] - aborting due to stop/shutdown requested\n", __func__, params.name, quorumIndex);
            throw AbortPhaseException();
        }
        auto [_phase, _quorumHash] = GetPhaseAndQuorumHash();
        if (!expectedQuorumHash.IsNull() && _quorumHash != expectedQuorumHash) {
            LogPrint(BCLog::LLMQ_DKG, "ActiveDKGSessionHandler::%s -- %s qi[%d] - aborting due unexpected expectedQuorumHash change\n", __func__, params.name, quorumIndex);
            throw AbortPhaseException();
        }
        if (_phase == nextPhase) {
            break;
        }
        if (curPhase.has_value() && _phase != curPhase) {
            LogPrint(BCLog::LLMQ_DKG, "ActiveDKGSessionHandler::%s -- %s qi[%d] - aborting due unexpected phase change, _phase=%d, curPhase=%d\n", __func__, params.name, quorumIndex, std23::to_underlying(_phase), curPhase.has_value() ? std23::to_underlying(*curPhase) : -1);
            throw AbortPhaseException();
        }
        if (!shouldNotWait()) {
            UninterruptibleSleep(std::chrono::milliseconds{100});
        }
    }

    LogPrint(BCLog::LLMQ_DKG, "ActiveDKGSessionHandler::%s -- %s qi[%d] - done, curPhase=%d, nextPhase=%d\n", __func__, params.name, quorumIndex, curPhase.has_value() ? std23::to_underlying(*curPhase) : -1, std23::to_underlying(nextPhase));

    if (nextPhase == QuorumPhase::Initialized) {
        m_dkgdbgman.ResetLocalSessionStatus(params.type, quorumIndex);
    } else {
        m_dkgdbgman.UpdateLocalSessionStatus(params.type, quorumIndex, [&](CDKGDebugSessionStatus& status) {
            bool changed = status.phase != nextPhase;
            status.phase = nextPhase;
            return changed;
        });
    }
}

void ActiveDKGSessionHandler::WaitForNewQuorum(const uint256& oldQuorumHash) const
{
    LogPrint(BCLog::LLMQ_DKG, "ActiveDKGSessionHandler::%s -- %s qi[%d]- starting\n", __func__, params.name, quorumIndex);

    while (true) {
        if (stopRequested) {
            LogPrint(BCLog::LLMQ_DKG, "ActiveDKGSessionHandler::%s -- %s qi[%d] - aborting due to stop/shutdown requested\n", __func__, params.name, quorumIndex);
            throw AbortPhaseException();
        }
        auto [_, _quorumHash] = GetPhaseAndQuorumHash();
        if (_quorumHash != oldQuorumHash) {
            break;
        }
        UninterruptibleSleep(std::chrono::milliseconds{100});
    }

    LogPrint(BCLog::LLMQ_DKG, "ActiveDKGSessionHandler::%s -- %s qi[%d] - done\n", __func__, params.name, quorumIndex);
}

// Sleep some time to not fully overload the whole network
void ActiveDKGSessionHandler::SleepBeforePhase(QuorumPhase curPhase, const uint256& expectedQuorumHash,
                                               double randomSleepFactor, const WhileWaitFunc& runWhileWaiting) const
{
    if (!curSession->AreWeMember()) {
        // Non-members do not participate and do not create any network load, no need to sleep.
        return;
    }

    if (Params().MineBlocksOnDemand()) {
        // On regtest, blocks can be mined on demand without any significant time passing between these.
        // We shouldn't wait before phases in this case.
        return;
    }

    // Two blocks can come very close to each other, this happens pretty regularly. We don't want to be
    // left behind and marked as a bad member. This means that we should not count the last block of the
    // phase as a safe one to keep sleeping, that's why we calculate the phase sleep time as a time of
    // the full phase minus one block here.
    double phaseSleepTime = (params.dkgPhaseBlocks - 1) * Params().GetConsensus().nPowTargetSpacing * 1000;
    // Expected phase sleep time per member
    double phaseSleepTimePerMember = phaseSleepTime / params.size;
    // Don't expect perfect block times and thus reduce the phase time to be on the secure side (caller chooses factor)
    double adjustedPhaseSleepTimePerMember = phaseSleepTimePerMember * randomSleepFactor;

    int64_t sleepTime = (int64_t)(adjustedPhaseSleepTimePerMember * curSession->GetMyMemberIndex().value_or(0));
    const auto endTime = SteadyClock::now() + std::chrono::milliseconds{sleepTime};
    int heightTmp{currentHeight.load()};
    int heightStart{heightTmp};

    LogPrint(BCLog::LLMQ_DKG, "ActiveDKGSessionHandler::%s -- %s qi[%d] - starting sleep for %d ms, curPhase=%d\n", __func__, params.name, quorumIndex, sleepTime, std23::to_underlying(curPhase));

    while (SteadyClock::now() < endTime) {
        if (stopRequested) {
            LogPrint(BCLog::LLMQ_DKG, "ActiveDKGSessionHandler::%s -- %s qi[%d] - aborting due to stop/shutdown requested\n", __func__, params.name, quorumIndex);
            throw AbortPhaseException();
        }
        auto cur_height = currentHeight.load();
        if (cur_height > heightTmp) {
            // New block(s) just came in
            int64_t expectedBlockTime = (cur_height - heightStart) * Params().GetConsensus().nPowTargetSpacing * 1000;
            if (expectedBlockTime > sleepTime) {
                // Blocks came faster than we expected, jump into the phase func asap
                break;
            }
            heightTmp = cur_height;
        }
        if (WITH_LOCK(cs_phase_qhash, return phase != curPhase || quorumHash != expectedQuorumHash)) {
            // Something went wrong and/or we missed quite a few blocks and it's just too late now
            LogPrint(BCLog::LLMQ_DKG, "ActiveDKGSessionHandler::%s -- %s qi[%d] - aborting due unexpected phase/expectedQuorumHash change\n", __func__, params.name, quorumIndex);
            throw AbortPhaseException();
        }
        if (!runWhileWaiting()) {
            UninterruptibleSleep(std::chrono::milliseconds{100});
        }
    }

    LogPrint(BCLog::LLMQ_DKG, "ActiveDKGSessionHandler::%s -- %s qi[%d] - done, curPhase=%d\n", __func__, params.name, quorumIndex, std23::to_underlying(curPhase));
}

void ActiveDKGSessionHandler::HandlePhase(QuorumPhase curPhase, QuorumPhase nextPhase,
                                          const uint256& expectedQuorumHash, double randomSleepFactor,
                                          const StartPhaseFunc& startPhaseFunc, const WhileWaitFunc& runWhileWaiting)
{
    LogPrint(BCLog::LLMQ_DKG, "ActiveDKGSessionHandler::%s -- %s qi[%d] - starting, curPhase=%d, nextPhase=%d\n", __func__, params.name, quorumIndex, std23::to_underlying(curPhase), std23::to_underlying(nextPhase));

    SleepBeforePhase(curPhase, expectedQuorumHash, randomSleepFactor, runWhileWaiting);
    startPhaseFunc();
    WaitForNextPhase(curPhase, nextPhase, expectedQuorumHash, runWhileWaiting);

    LogPrint(BCLog::LLMQ_DKG, "ActiveDKGSessionHandler::%s -- %s qi[%d] - done, curPhase=%d, nextPhase=%d\n", __func__, params.name, quorumIndex, std23::to_underlying(curPhase), std23::to_underlying(nextPhase));
}

bool ActiveDKGSessionHandler::GetContribution(const uint256& hash, CDKGContribution& ret) const
{
    return curSession && curSession->GetContribution(hash, ret);
}

bool ActiveDKGSessionHandler::GetComplaint(const uint256& hash, CDKGComplaint& ret) const
{
    return curSession && curSession->GetComplaint(hash, ret);
}

bool ActiveDKGSessionHandler::GetJustification(const uint256& hash, CDKGJustification& ret) const
{
    return curSession && curSession->GetJustification(hash, ret);
}

bool ActiveDKGSessionHandler::GetPrematureCommitment(const uint256& hash, CDKGPrematureCommitment& ret) const
{
    return curSession && curSession->GetPrematureCommitment(hash, ret);
}

QuorumPhase ActiveDKGSessionHandler::GetPhase() const
{
    return WITH_LOCK(cs_phase_qhash, return phase);
}
} // namespace llmq
