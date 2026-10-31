// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

// Chain-aware companions to evo/snapshot.cpp, deliberately header-less: these
// implementations are declared in evo/snapshot.h but need validation.h and
// llmq internals, so they live in their own compilation unit to keep the
// snapshot codec free of an evo/snapshot -> validation -> evo/snapshot cycle.

#include <evo/snapshot.h>

#include <chain.h>
#include <chainparams.h>
#include <consensus/amount.h>
#include <dbwrapper.h>
#include <deploymentstatus.h>
#include <evo/cbtx.h>
#include <evo/creditpool.h>
#include <evo/deterministicmns.h>
#include <evo/evodb.h>
#include <evo/mnhftx.h>
#include <evo/snapshot_types.h>
#include <evo/specialtxman.h>
#include <llmq/blockprocessor.h>
#include <llmq/options.h>
#include <llmq/quorumsman.h>
#include <llmq/snapshot.h>
#include <llmq/utils.h>
#include <node/blockstorage.h>
#include <primitives/block.h>
#include <tinyformat.h>
#include <uint256.h>
#include <util/check.h>
#include <validation.h>
#include <versionbits.h>

#include <algorithm>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace evo {
namespace {

//! V20 is buried, so its state after a height is known even for a cycle that
//! is not mined yet.
bool V20ActiveAfterHeight(const Consensus::Params& consensus, int height)
{
    return height + 1 >= consensus.DeploymentHeight(Consensus::DEPLOYMENT_V20);
}

bool ValidateCommitmentAgainstChain(const MinedQuorumCommitment& entry, const ChainstateManager& chainman,
                                    const CBlockIndex* base_index, const Consensus::LLMQParams& params,
                                    bool rotation_enabled) EXCLUSIVE_LOCKS_REQUIRED(::cs_main)
{
    const CBlockIndex* quorum_index{chainman.m_blockman.LookupBlockIndex(entry.quorum_base_block_hash)};
    const CBlockIndex* mined_index{chainman.m_blockman.LookupBlockIndex(entry.mined_block_hash)};
    if (quorum_index == nullptr || mined_index == nullptr ||
        base_index->GetAncestor(quorum_index->nHeight) != quorum_index ||
        base_index->GetAncestor(mined_index->nHeight) != mined_index)
        return false;
    const int cycle_height{quorum_index->nHeight - quorum_index->nHeight % params.dkgInterval};
    if (rotation_enabled) {
        if (entry.commitment.quorumIndex != quorum_index->nHeight % params.dkgInterval ||
            entry.commitment.quorumIndex < 0 || entry.commitment.quorumIndex >= params.signingActiveQuorumCount)
            return false;
    } else if (quorum_index->nHeight != cycle_height || entry.commitment.quorumIndex != 0) {
        return false;
    }
    const int mined_cycle{mined_index->nHeight - mined_index->nHeight % params.dkgInterval};
    if (mined_cycle != cycle_height || mined_index->nHeight % params.dkgInterval < params.dkgMiningWindowStart ||
        mined_index->nHeight % params.dkgInterval > params.dkgMiningWindowEnd)
        return false;
    const uint16_t expected_version{
        llmq::CFinalCommitment::GetVersion(rotation_enabled, DeploymentActiveAfter(quorum_index, chainman.GetConsensus(),
                                                                                   Consensus::DEPLOYMENT_V19))};
    return entry.commitment.nVersion == expected_version && entry.commitment.VerifySizes(params);
}

//! Applicable(Y): a rotation cycle at or below the base whose quarter the
//! ordinary computation builds and later reads.
bool IsApplicableRotationCycle(const ChainstateManager& chainman, const Consensus::LLMQParams& params,
                               const CBlockIndex* base_index, int cycle_height) EXCLUSIVE_LOCKS_REQUIRED(::cs_main)
{
    if (cycle_height < 0 || cycle_height > base_index->nHeight || cycle_height % params.dkgInterval != 0) return false;
    const CBlockIndex* cycle{base_index->GetAncestor(cycle_height)};
    return cycle != nullptr && llmq::IsQuorumRotationEnabled(params, cycle) &&
           chainman.IsQuorumTypeEnabled(params.type, cycle->pprev);
}

//! The modifier of the cycle at `work.cycle_height`, from the work block's
//! data or, when that is unreadable, from `seeded`. Mirrors the ordinary
//! calculation (llmq/utils.cpp) without its block-hash fallback for an
//! unreadable v20 work block, and without needing an unmined cycle's block.
std::optional<uint256> ExactWorkModifier(const Consensus::LLMQParams& params, const Consensus::Params& consensus,
                                         const CBlockIndex* base_index, const EvoSnapshotWorkBlock& work,
                                         const std::optional<uint256>& seeded, std::string& error)
    EXCLUSIVE_LOCKS_REQUIRED(::cs_main)
{
    const CBlockIndex* modifier_work{base_index->GetAncestor(work.cycle_height - llmq::WORK_DIFF_DEPTH)};
    if (modifier_work == nullptr) {
        error = "evo quorum modifier work block is not an ancestor of the base";
        return std::nullopt;
    }
    if (DeploymentActiveAfter(modifier_work, consensus, Consensus::DEPLOYMENT_V20)) {
        CBlock block;
        if ((modifier_work->nStatus & BLOCK_HAVE_DATA) == 0 || !node::ReadBlockFromDisk(block, modifier_work, consensus)) {
            if (!seeded) error = "quorum work block data unavailable for its exact score modifier";
            return seeded;
        }
        return llmq::utils::GetWorkBlockModifier(params, consensus, block, *modifier_work);
    }
    if (params.useRotation) return ::SerializeHash(std::make_pair(params.type, modifier_work->GetBlockHash()));
    if (work.cycle_height > base_index->nHeight) {
        error = "pre-v20 modifier of a cycle above the base is undecidable";
        return std::nullopt;
    }
    return ::SerializeHash(std::make_pair(params.type, base_index->GetAncestor(work.cycle_height)->GetBlockHash()));
}

//! A carried list for the snapshot: an unbound pre-DIP3 list is the empty list
//! bound to its block, which is what the snapshot view reads there.
CDeterministicMNList BoundList(const CDeterministicMNList& list, const CBlockIndex* index)
{
    if (list.GetBlockHash().IsNull()) return CDeterministicMNList{index->GetBlockHash(), index->nHeight, 0};
    return list;
}

//! Whether every activation input of a quorum type is the same for the block
//! after `target_height` as for the block after the base: no buried deployment
//! or DIP0024 quorum height switches in between, and no versionbits deployment
//! that is not yet active crosses a period boundary.
bool ContextUnchangedUntil(const ChainstateManager& chainman, const CBlockIndex* base_index, int64_t target_height)
    EXCLUSIVE_LOCKS_REQUIRED(::cs_main)
{
    const auto& consensus{chainman.GetConsensus()};
    const int64_t base_height{base_index->nHeight};
    for (int16_t dep{Consensus::DEPLOYMENT_HEIGHTINCB};
         Consensus::ValidDeployment(static_cast<Consensus::BuriedDeployment>(dep)); ++dep) {
        const int64_t height{consensus.DeploymentHeight(static_cast<Consensus::BuriedDeployment>(dep))};
        if ((base_height + 1 >= height) != (target_height + 1 >= height)) return false;
    }
    if ((base_height >= consensus.DIP0024QuorumsHeight) != (target_height >= consensus.DIP0024QuorumsHeight)) {
        return false;
    }
    for (int pos{0}; pos < Consensus::MAX_VERSION_BITS_DEPLOYMENTS; ++pos) {
        const auto dep{static_cast<Consensus::DeploymentPos>(pos)};
        if (DeploymentActiveAfter(base_index, chainman, dep)) continue;
        const auto& deployment{consensus.vDeployments[dep]};
        const int64_t period{deployment.nWindowSize ? deployment.nWindowSize : int64_t{consensus.nMinerConfirmationWindow}};
        // A state is constant within a period: blocks base+1 .. target+1 share
        // one unless one of them starts a new period.
        if (period > 0 && (target_height + 1) / period != (base_height + 1) / period) return false;
    }
    return true;
}

//! I6 for the next cycle N: its members read work data at or below the base
//! once N - D <= B, and the planner derives h and N from the context at B.
//! A type enabled, or switching rotation, between B and N would need inputs
//! nothing carries, so such a base is refused at export and load alike.
bool CheckNextCycleContext(const ChainstateManager& chainman, const CBlockIndex* base_index, std::string& error)
    EXCLUSIVE_LOCKS_REQUIRED(::cs_main)
{
    for (const auto& params : chainman.GetConsensus().llmqs) {
        const int64_t base_height{base_index->nHeight};
        const int64_t next{base_height - base_height % params.dkgInterval + params.dkgInterval};
        // N - 1 == B makes N's context the base's own.
        if (next - llmq::WORK_DIFF_DEPTH > base_height || next - 1 == base_height) continue;
        if (!ContextUnchangedUntil(chainman, base_index, next - 1)) {
            error = strprintf("evo snapshot base precedes an undecidable activation context at the next %s cycle %d",
                              params.name, next);
            return false;
        }
    }
    return true;
}

//! The carried records of `params`' type in the family its scans read, newest
//! mined first: per quorum index with rotation, one sequence otherwise.
std::vector<std::vector<std::pair<int, const CBlockIndex*>>> CarriedScanRecords(const EvoSnapshot& snapshot,
                                                                                const ChainstateManager& chainman,
                                                                                const Consensus::LLMQParams& params)
    EXCLUSIVE_LOCKS_REQUIRED(::cs_main)
{
    std::vector<std::vector<std::pair<int, const CBlockIndex*>>> records(
        params.useRotation ? static_cast<size_t>(params.signingActiveQuorumCount) : 1);
    const auto data{std::ranges::find_if(snapshot.quorums, [&](const auto& q) { return q.llmq_type == params.type; })};
    // Scans of a rotating type read the indexed family only. Without rotation
    // at the base there is no indexed record at or below it.
    if (data == snapshot.quorums.end() || data->rotation_enabled != params.useRotation) return records;
    for (const auto* commitments : {&data->active_commitments, &data->safety_commitments}) {
        for (const auto& entry : *commitments) {
            const size_t sequence{params.useRotation ? static_cast<size_t>(entry.commitment.quorumIndex) : 0};
            const CBlockIndex* mined{chainman.m_blockman.LookupBlockIndex(entry.mined_block_hash)};
            const CBlockIndex* quorum{chainman.m_blockman.LookupBlockIndex(entry.quorum_base_block_hash)};
            if (sequence < records.size() && mined != nullptr && quorum != nullptr) {
                records[sequence].emplace_back(mined->nHeight, quorum);
            }
        }
    }
    for (auto& sequence : records) {
        std::sort(sequence.begin(), sequence.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    }
    return records;
}

//! Prove the scans the snapshot chainstate must answer from carried history
//! alone: ChainLock verification for every height B + 1 may certify, and the
//! asset-unlock quorum check of B + 1. With `full_index`, the exporter's
//! complete commitment index, the proven vectors must also equal it.
bool CheckScanCoverage(const EvoSnapshot& snapshot, const ChainstateManager& chainman, const CBlockIndex* base_index,
                       const llmq::CQuorumBlockProcessor* full_index, std::string& error)
    EXCLUSIVE_LOCKS_REQUIRED(::cs_main)
{
    const auto& consensus{chainman.GetConsensus()};
    struct Request {
        Consensus::LLMQType type{Consensus::LLMQType::LLMQ_NONE};
        const CBlockIndex* start{nullptr};
        size_t count{0};
    };
    std::vector<Request> requests;
    // Without v20 at B + 1 its CbTx carries no ChainLock to verify.
    const auto chainlocks{chainman.GetParams().GetLLMQ(consensus.llmqTypeChainLocks)};
    if (chainlocks && DeploymentActiveAfter(base_index, consensus, Consensus::DEPLOYMENT_V20)) {
        // B + 1 may certify any height from the base's certified one on; with
        // no certificate at the base, any height at all.
        int first_height{0};
        const bool legacy{!DeploymentActiveAt(*base_index, consensus, Consensus::DEPLOYMENT_V19)};
        if (const auto cbtx{snapshot.base_coinbase ? DecodeCbTxPayload(*snapshot.base_coinbase, legacy) : std::nullopt};
            cbtx && cbtx->nVersion >= CCbTx::Version::CLSIG_AND_BALANCE && cbtx->bestCLSignature.Get().IsValid() &&
            cbtx->bestCLHeightDiff < static_cast<uint32_t>(base_index->nHeight)) {
            first_height = base_index->nHeight - static_cast<int>(cbtx->bestCLHeightDiff) - 1;
        }
        // With a fixed type and count, coverage only grows with the cutoff,
        // and the cutoff with the start. So only the first start of each
        // context (enablement and whether the scan is empty by genesis) needs
        // proving.
        std::optional<std::pair<bool, bool>> context;
        for (int height{std::max(first_height, llmq::SIGN_HEIGHT_OFFSET)}; height <= base_index->nHeight; ++height) {
            const CBlockIndex* start{base_index->GetAncestor(height - llmq::SIGN_HEIGHT_OFFSET)};
            const bool enabled{chainman.IsQuorumTypeEnabled(chainlocks->type, start)};
            const std::pair<bool, bool> current{enabled, enabled && llmq::NormalizeScanStart(*chainlocks, start)};
            if (context == current) continue;
            context = current;
            if (current.second) {
                requests.push_back({chainlocks->type, start, static_cast<size_t>(chainlocks->signingActiveQuorumCount)});
            }
        }
    }
    if (const auto platform{chainman.GetParams().GetLLMQ(consensus.llmqTypePlatform)}) {
        requests.push_back({platform->type, base_index, static_cast<size_t>(platform->signingActiveQuorumCount) + 1});
    }

    std::map<Consensus::LLMQType, std::vector<std::vector<std::pair<int, const CBlockIndex*>>>> carried;
    std::set<std::tuple<Consensus::LLMQType, int, size_t>> checked;
    for (const auto& request : requests) {
        const auto params{*Assert(chainman.GetParams().GetLLMQ(request.type))};
        if (!chainman.IsQuorumTypeEnabled(request.type, request.start)) continue;
        const auto cutoff{llmq::NormalizeScanStart(params, request.start)};
        if (!cutoff || !checked.emplace(request.type, (*cutoff)->nHeight, request.count).second) continue;
        auto [it, inserted]{carried.try_emplace(request.type)};
        if (inserted) it->second = CarriedScanRecords(snapshot, chainman, params);
        const auto coverage{EvoSnapshotScanCoverage(snapshot, params)};
        std::vector<llmq::ScanSequence> sequences;
        for (size_t i{0}; i < it->second.size(); ++i) {
            llmq::ScanSequence sequence{{}, coverage.End(i)};
            for (const auto& [mined_height, quorum] : it->second[i]) {
                if (mined_height <= (*cutoff)->nHeight) sequence.records.push_back(quorum);
            }
            sequences.push_back(std::move(sequence));
        }
        const auto proven{llmq::TraverseScanSequences(params.useRotation, sequences, request.count)};
        if (!proven) {
            error = strprintf("evo snapshot cannot prove the llmqType=%d scan of %d from height %d",
                              static_cast<int>(request.type), request.count, (*cutoff)->nHeight);
            return false;
        }
        if (full_index == nullptr) continue;
        std::vector<llmq::ScanSequence> complete;
        if (params.useRotation) {
            for (int index{0}; index < params.signingActiveQuorumCount; ++index) {
                complete.push_back(
                    {full_index->GetMinedCommitmentsByQuorumIndexUntilBlock(request.type, *cutoff, index, request.count),
                     llmq::ScanSequenceEnd::END});
            }
        } else {
            complete.push_back({full_index->GetMinedCommitmentsUntilBlock(request.type, *cutoff, request.count),
                                llmq::ScanSequenceEnd::END});
        }
        if (llmq::TraverseScanSequences(params.useRotation, complete, request.count) != proven) {
            error = strprintf("evo snapshot llmqType=%d scan from height %d differs from the full index",
                              static_cast<int>(request.type), (*cutoff)->nHeight);
            return false;
        }
    }
    return true;
}

} // namespace

bool CheckEvoSnapshotScanCoverage(const EvoSnapshot& snapshot, const ChainstateManager& chainman,
                                  const CBlockIndex* base_index, std::string& error)
{
    AssertLockHeld(::cs_main);
    return CheckScanCoverage(snapshot, chainman, base_index, /*full_index=*/nullptr, error);
}

llmq::ScanCoverageRecord EvoSnapshotScanCoverage(const EvoSnapshot& snapshot, const Consensus::LLMQParams& params)
{
    llmq::ScanCoverageRecord coverage;
    const auto data{std::ranges::find_if(snapshot.quorums, [&](const auto& q) { return q.llmq_type == params.type; })};
    const auto end_of = [](size_t carried, size_t selected) {
        return static_cast<uint8_t>(carried < selected ? llmq::ScanSequenceEnd::END : llmq::ScanSequenceEnd::UNKNOWN);
    };
    if (params.useRotation) {
        // The canonical selection carries the last two of every quorum index;
        // fewer means none older exists. Without rotation at the base the
        // indexed family is empty there.
        std::vector<size_t> counts(static_cast<size_t>(params.signingActiveQuorumCount));
        if (data != snapshot.quorums.end() && data->rotation_enabled) {
            for (const auto* commitments : {&data->active_commitments, &data->safety_commitments}) {
                for (const auto& entry : *commitments) {
                    if (entry.commitment.quorumIndex >= 0 &&
                        static_cast<size_t>(entry.commitment.quorumIndex) < counts.size()) {
                        ++counts[entry.commitment.quorumIndex];
                    }
                }
            }
        }
        for (const size_t count : counts)
            coverage.ends.push_back(end_of(count, 2));
    } else {
        const size_t carried{data == snapshot.quorums.end()
                                 ? size_t{0}
                                 : data->active_commitments.size() + data->safety_commitments.size()};
        coverage.ends.push_back(end_of(carried, SnapshotCommitmentCount(params, /*rotation_enabled=*/false)));
    }
    return coverage;
}

int EvoSnapshotWorkHeight(const Consensus::Params& consensus, const Consensus::LLMQParams& params,
                          bool rotation_enabled, int quorum_height)
{
    if (rotation_enabled) return quorum_height - quorum_height % params.dkgInterval - llmq::WORK_DIFF_DEPTH;
    return V20ActiveAfterHeight(consensus, quorum_height) ? quorum_height - llmq::WORK_DIFF_DEPTH : quorum_height;
}

bool PlanEvoSnapshotDependencies(const ChainstateManager& chainman, const CBlockIndex* base_index,
                                 Consensus::LLMQType llmq_type, const std::vector<const CBlockIndex*>& commitments,
                                 EvoSnapshotDependencyPlan& plan, std::string& error)
{
    AssertLockHeld(::cs_main);
    plan = EvoSnapshotDependencyPlan{};
    plan.llmq_type = llmq_type;
    const auto params_opt{chainman.GetParams().GetLLMQ(llmq_type)};
    if (base_index == nullptr || !params_opt) {
        error = "evo snapshot dependency plan without a base or LLMQ parameters";
        return false;
    }
    const auto& params{*params_opt};
    const auto& consensus{chainman.GetConsensus()};
    const int base_height{base_index->nHeight};
    const int interval{params.dkgInterval};
    const int h{base_height - base_height % interval};
    const int next{h + interval};
    plan.rotation_enabled = llmq::IsQuorumRotationEnabled(params, base_index);

    // A_T for a rotating type (cycle heights), or the quorum heights K_T ∪ {h, N}.
    std::set<int> cycles;
    for (const CBlockIndex* quorum : commitments) {
        if (quorum == nullptr || base_index->GetAncestor(quorum->nHeight) != quorum) {
            error = "evo snapshot commitment is not an ancestor of the base";
            return false;
        }
        cycles.insert(plan.rotation_enabled ? quorum->nHeight - quorum->nHeight % interval : quorum->nHeight);
    }
    // A type that forms no quorum at the base is carried only for its retained
    // commitments, so it needs no members for h or N.
    const bool enabled{chainman.IsQuorumTypeEnabled(llmq_type, base_index)};
    if (enabled) {
        cycles.insert(h);
        cycles.insert(next);
    }

    std::map<int, int> work_cycles; // work height -> the cycle whose modifier it carries
    const auto add_work = [&](int work_height, int cycle_height) {
        if (work_height < 0 || work_height > base_height) return;
        work_cycles.emplace(work_height, cycle_height);
    };
    std::set<int> referenced_work;
    for (const CBlockIndex* quorum : commitments) {
        referenced_work.insert(EvoSnapshotWorkHeight(consensus, params, plan.rotation_enabled, quorum->nHeight));
    }

    if (plan.rotation_enabled) {
        std::set<int> quarters;
        for (const int cycle : cycles) {
            for (int k{1}; k <= 3; ++k) {
                if (IsApplicableRotationCycle(chainman, params, base_index, cycle - k * interval)) {
                    quarters.insert(cycle - k * interval);
                }
            }
        }
        for (const int quarter : quarters) {
            plan.rotation_cycles.push_back(base_index->GetAncestor(quarter));
            add_work(quarter - llmq::WORK_DIFF_DEPTH, quarter);
            referenced_work.insert(quarter - llmq::WORK_DIFF_DEPTH);
        }
        for (const int cycle : cycles)
            add_work(cycle - llmq::WORK_DIFF_DEPTH, cycle);
    } else {
        for (const int quorum : cycles) {
            add_work(EvoSnapshotWorkHeight(consensus, params, /*rotation_enabled=*/false, quorum), quorum);
        }
    }

    for (const auto& [work_height, cycle_height] : work_cycles) {
        plan.work_blocks.push_back({base_index->GetAncestor(work_height), cycle_height});
    }
    if (enabled) {
        for (const int cycle : {h, next}) {
            const int work_height{EvoSnapshotWorkHeight(consensus, params, plan.rotation_enabled, cycle)};
            if (!work_cycles.contains(work_height) || referenced_work.contains(work_height)) continue;
            plan.bootstrap_work.push_back({cycle, base_index->GetAncestor(work_height)->GetBlockHash()});
        }
    }
    return true;
}

bool BuildEvoSnapshot(const CChainParams& chainparams, const ChainstateManager& chainman, CDeterministicMNManager& dmnman,
                      const llmq::CQuorumBlockProcessor& qblockman, llmq::CQuorumSnapshotManager& qsnapman,
                      CCreditPoolManager& cpoolman, CMNHFManager& mnhfman, const CBlockIndex* base_index,
                      EvoSnapshot& snapshot, std::string& error, const EvoSnapshotCaptures* captures)
{
    AssertLockHeld(::cs_main);
    error.clear();
    if (base_index == nullptr) {
        error = "evo snapshot base block is null";
        return false;
    }
    const auto& consensus{chainparams.GetConsensus()};

    EvoSnapshot result;
    result.base_block_hash = base_index->GetBlockHash();
    if (!DeploymentActiveAt(*base_index, consensus, Consensus::DEPLOYMENT_DIP0003)) {
        result.mn_list = CDeterministicMNList{base_index->GetBlockHash(), base_index->nHeight, 0};
        try {
            result.Validate();
        } catch (const std::exception& e) {
            error = e.what();
            return false;
        }
        snapshot = std::move(result);
        return true;
    }
    if (!CheckNextCycleContext(chainman, base_index, error)) return false;
    result.mn_list = dmnman.GetListForBlock(base_index);
    if (captures && captures->base_coinbase) {
        result.base_coinbase = captures->base_coinbase;
        result.base_coinbase_branch = captures->base_coinbase_branch;
    } else {
        CBlock base_block;
        if (!node::ReadBlockFromDisk(base_block, base_index, consensus) || base_block.vtx.empty()) {
            error = "evo snapshot base block is unreadable";
            return false;
        }
        result.base_coinbase = base_block.vtx[0];
        result.base_coinbase_branch = BlockMerkleBranch(base_block, 0);
    }
    std::map<uint256, const CBlockIndex*> historical;

    for (const auto& params : consensus.llmqs) {
        QuorumSnapshotData data;
        data.llmq_type = params.type;
        data.rotation_enabled = llmq::IsQuorumRotationEnabled(params, base_index);

        const size_t active_count{static_cast<size_t>(params.signingActiveQuorumCount)};
        const size_t total_count{SnapshotCommitmentCount(params, data.rotation_enabled)};
        std::vector<const CBlockIndex*> active;
        std::vector<const CBlockIndex*> safety;
        if (data.rotation_enabled) {
            active = qblockman.GetLastMinedCommitmentsPerQuorumIndexUntilBlock(params.type, base_index, 0);
            safety = qblockman.GetLastMinedCommitmentsPerQuorumIndexUntilBlock(params.type, base_index, 1);
        } else {
            active = qblockman.GetMinedCommitmentsUntilBlock(params.type, base_index, total_count);
            const size_t emit_active{std::min(active.size(), active_count)};
            safety.assign(active.begin() + emit_active, active.end());
            active.resize(emit_active);
        }
        if (active.size() > active_count) active.resize(active_count);
        if (safety.size() > total_count - active_count) safety.resize(total_count - active_count);
        // A type that can no longer form quorums (e.g. LLMQ_50_60 after DIP24
        // on mainnet) still contributes its retained commitments to the CbTx
        // quorum merkle root, so it is carried whenever it has any.
        if (active.empty() && safety.empty() && !chainman.IsQuorumTypeEnabled(params.type, base_index)) continue;

        std::vector<const CBlockIndex*> carried{active};
        carried.insert(carried.end(), safety.begin(), safety.end());
        EvoSnapshotDependencyPlan plan;
        if (!PlanEvoSnapshotDependencies(chainman, base_index, params.type, carried, plan, error)) return false;

        const auto read_commitment = [&](const CBlockIndex* quorum) -> std::optional<MinedQuorumCommitment> {
            auto [commitment, mined_hash] = qblockman.GetMinedCommitment(params.type, quorum->GetBlockHash());
            if (mined_hash.IsNull()) {
                error = "mined quorum commitment not found for " + quorum->GetBlockHash().ToString();
                return std::nullopt;
            }
            const int work_height{EvoSnapshotWorkHeight(consensus, params, data.rotation_enabled, quorum->nHeight)};
            if (work_height < 0) {
                error = "mined quorum commitment below its work depth";
                return std::nullopt;
            }
            return MinedQuorumCommitment{quorum->GetBlockHash(), base_index->GetAncestor(work_height)->GetBlockHash(),
                                         std::move(commitment), mined_hash};
        };
        for (const CBlockIndex* quorum : active) {
            auto entry{read_commitment(quorum)};
            if (!entry) return false;
            data.active_commitments.emplace_back(std::move(*entry));
        }
        for (const CBlockIndex* quorum : safety) {
            auto entry{read_commitment(quorum)};
            if (!entry) return false;
            data.safety_commitments.emplace_back(std::move(*entry));
        }

        for (const CBlockIndex* cycle : plan.rotation_cycles) {
            auto stored{qsnapman.GetSnapshotForBlock(params.type, cycle)};
            const auto inputs_complete = [&]() EXCLUSIVE_LOCKS_REQUIRED(::cs_main) {
                for (int k{1}; k <= 3; ++k) {
                    const int previous{cycle->nHeight - k * params.dkgInterval};
                    if (IsApplicableRotationCycle(chainman, params, base_index, previous) &&
                        !qsnapman.GetSnapshotForBlock(params.type, base_index->GetAncestor(previous))) {
                        return false;
                    }
                }
                return true;
            };
            if (!stored && inputs_complete()) {
                // Block connection stores every applicable quarter when its
                // cycle connects (PreComputeQuorumMembers), so this only
                // restores a lost one, from complete inputs, the same way.
                (void)llmq::utils::GetAllQuorumMembers(params.type, {dmnman, qsnapman, chainman, cycle});
                stored = qsnapman.GetSnapshotForBlock(params.type, cycle);
            }
            if (!stored) {
                error = strprintf("rotation quarter for applicable cycle %d is unavailable", cycle->nHeight);
                return false;
            }
            const CBlockIndex* work{cycle->GetAncestor(cycle->nHeight - llmq::WORK_DIFF_DEPTH)};
            data.rotation_snapshots.push_back({cycle->GetBlockHash(), work->GetBlockHash(), *stored});
        }
        for (const auto& work : plan.work_blocks) {
            std::optional<uint256> modifier;
            if (captures) {
                const auto captured{captures->modifiers.find({params.type, work.work_index->GetBlockHash()})};
                if (captured != captures->modifiers.end()) modifier = captured->second;
            }
            if (!modifier) {
                modifier = ExactWorkModifier(params, consensus, base_index, work,
                                             qsnapman.GetSeededQuorumModifier(params.type, work.work_index->GetBlockHash()),
                                             error);
            }
            if (!modifier) return false;
            result.quorum_modifiers.push_back({params.type, work.work_index->GetBlockHash(), *modifier});
            if (work.work_index != base_index) historical.emplace(work.work_index->GetBlockHash(), work.work_index);
        }
        data.bootstrap_work = plan.bootstrap_work;
        data.active_commitments = CanonicallySortedCopy(std::move(data.active_commitments));
        data.safety_commitments = CanonicallySortedCopy(std::move(data.safety_commitments));
        data.rotation_snapshots = CanonicallySortedCopy(std::move(data.rotation_snapshots));
        result.quorums.emplace_back(std::move(data));
    }

    std::vector<const CBlockIndex*> ordered_history;
    ordered_history.reserve(historical.size());
    for (const auto& [_, index] : historical)
        ordered_history.push_back(index);
    std::sort(ordered_history.begin(), ordered_history.end(), [](const auto* a, const auto* b) {
        return std::make_tuple(a->nHeight, a->GetBlockHash()) > std::make_tuple(b->nHeight, b->GetBlockHash());
    });
    CDeterministicMNList previous_list{result.mn_list};
    uint256 previous_hash{result.base_block_hash};
    for (const CBlockIndex* index : ordered_history) {
        const CDeterministicMNList list{BoundList(dmnman.GetListForBlock(index), index)};
        result.historical_mn_list_diffs.push_back({previous_hash, index->GetBlockHash(), index->nHeight,
                                                   list.GetTotalRegisteredCount(), CanonicalMNListHash(list),
                                                   previous_list.BuildDiff(list)});
        previous_hash = index->GetBlockHash();
        previous_list = list;
    }
    result.credit_pool = captures && captures->base_credit_pool ? *captures->base_credit_pool
                                                                : cpoolman.GetCreditPool(base_index);
    const int window{chainparams.CreditPoolPeriodBlocks()};
    for (int height{std::max(0, base_index->nHeight - window + 1)}; height <= base_index->nHeight; ++height) {
        const CBlockIndex* index{base_index->GetAncestor(height)};
        if (captures) {
            if (const auto captured{captures->credit_history.find(index->GetBlockHash())};
                captured != captures->credit_history.end()) {
                result.credit_history.push_back(captured->second);
                continue;
            }
        }
        const auto entry{CCreditPoolManager::ReadHistoryEntry(index, consensus, error)};
        if (!entry) return false;
        result.credit_history.push_back(*entry);
    }
    // The raw record: GetSignalsStage() would filter it for the next block,
    // which the snapshot chainstate does itself.
    result.mnhf_signals = mnhfman.GetForBlock(base_index);
    result.quorums = CanonicallySortedCopy(std::move(result.quorums));
    result.historical_mn_list_diffs = CanonicallySortedCopy(std::move(result.historical_mn_list_diffs));
    result.quorum_modifiers = CanonicallySortedCopy(std::move(result.quorum_modifiers));
    try {
        result.Validate();
    } catch (const std::exception& e) {
        error = e.what();
        return false;
    }
    // Export and load apply the same coverage gate; the exporter also checks
    // the proven scans against its complete index.
    if (!CheckScanCoverage(result, chainman, base_index, &qblockman, error)) return false;
    snapshot = std::move(result);
    return true;
}

bool ValidateEvoSnapshotAgainstChain(const EvoSnapshot& snapshot, const ChainstateManager& chainman,
                                     const CBlockIndex* base_index, std::string& error)
{
    AssertLockHeld(::cs_main);
    error.clear();
    const auto fail = [&](const std::string& message) {
        error = message;
        return false;
    };
    if (base_index == nullptr || snapshot.base_block_hash != base_index->GetBlockHash() ||
        snapshot.mn_list.GetBlockHash() != base_index->GetBlockHash() ||
        snapshot.mn_list.GetHeightForSnapshotCodec() != base_index->nHeight) {
        return fail("evo snapshot base block/height mismatch");
    }
    try {
        snapshot.Validate(/*require_canonical_order=*/true);
    } catch (const std::exception& e) {
        return fail(e.what());
    }

    const auto& consensus{chainman.GetConsensus()};
    if (!DeploymentActiveAt(*base_index, consensus, Consensus::DEPLOYMENT_DIP0003)) {
        if (!snapshot.quorums.empty() || !snapshot.historical_mn_list_diffs.empty() ||
            !snapshot.quorum_modifiers.empty() || snapshot.mn_list.GetCounts().total() != 0 ||
            snapshot.mn_list.GetTotalRegisteredCount() != 0 || snapshot.credit_pool.locked != 0 ||
            snapshot.credit_pool.currentLimit != 0 || snapshot.credit_pool.latelyUnlocked != 0 ||
            !snapshot.credit_pool.indexes.IsEmpty() || !snapshot.credit_history.empty() ||
            !snapshot.mnhf_signals.empty() || snapshot.base_coinbase || !snapshot.base_coinbase_branch.empty()) {
            return fail("nonempty pre-DIP3 evo snapshot");
        }
        return true;
    }
    if (!CheckNextCycleContext(chainman, base_index, error)) return false;
    // Anchor the section to the base header.
    if (!snapshot.base_coinbase ||
        ComputeMerkleRootFromBranch(snapshot.base_coinbase->GetHash(), snapshot.base_coinbase_branch, 0) !=
            base_index->hashMerkleRoot) {
        return fail("evo snapshot base coinbase is not proven by the base header");
    }

    // The credit-pool window: one entry per block of the window ending at the
    // base, zero wherever the pool did not exist yet.
    const int window{chainman.GetParams().CreditPoolPeriodBlocks()};
    if (window <= 0 || static_cast<size_t>(window) > EVO_SNAPSHOT_MAX_CREDIT_HISTORY ||
        snapshot.credit_history.size() != static_cast<size_t>(std::min(window, base_index->nHeight + 1))) {
        return fail("evo credit history does not cover the credit-pool window");
    }
    const int first_height{base_index->nHeight - static_cast<int>(snapshot.credit_history.size()) + 1};
    for (size_t i{0}; i < snapshot.credit_history.size(); ++i) {
        if (first_height + static_cast<int>(i) < consensus.DeploymentHeight(Consensus::DEPLOYMENT_V20) &&
            snapshot.credit_history[i] != CreditHistoryEntry{}) {
            return fail("evo credit history before v20 is not empty");
        }
    }
    if (!DeploymentActiveAt(*base_index, consensus, Consensus::DEPLOYMENT_V20) &&
        (snapshot.credit_pool.locked != 0 || snapshot.credit_pool.currentLimit != 0 ||
         snapshot.credit_pool.latelyUnlocked != 0 || !snapshot.credit_pool.indexes.IsEmpty())) {
        return fail("evo credit pool before v20 is not empty");
    }

    std::map<uint256, CDeterministicMNList> historical_lists;
    if (!ReconstructHistoricalMNLists(snapshot, historical_lists, error)) return false;
    historical_lists.emplace(snapshot.base_block_hash, snapshot.mn_list);
    for (const auto& entry : snapshot.historical_mn_list_diffs) {
        const CBlockIndex* index{chainman.m_blockman.LookupBlockIndex(entry.block_hash)};
        if (index == nullptr || base_index->GetAncestor(index->nHeight) != index || entry.height != index->nHeight) {
            return fail("invalid historical evo MN list chain data");
        }
    }

    std::map<std::pair<Consensus::LLMQType, uint256>, uint256> carried_modifiers;
    for (const auto& entry : snapshot.quorum_modifiers) {
        carried_modifiers.emplace(std::make_pair(entry.llmq_type, entry.work_block_hash), entry.modifier);
    }
    std::map<Consensus::LLMQType, const QuorumSnapshotData*> actual;
    for (const auto& data : snapshot.quorums)
        actual.emplace(data.llmq_type, &data);
    size_t known_count{0};
    std::set<uint256> required_work_hashes;
    std::set<std::pair<Consensus::LLMQType, uint256>> required_modifiers;
    for (const auto& params : consensus.llmqs) {
        const bool enabled{chainman.IsQuorumTypeEnabled(params.type, base_index)};
        const auto it{actual.find(params.type)};
        if (it == actual.end()) {
            if (enabled) return fail("missing enabled evo quorum type");
            continue;
        }
        ++known_count;
        const auto& data{*it->second};
        // A disabled type is carried only for the retained commitments it
        // still contributes to the CbTx quorum merkle root.
        if (!enabled && data.active_commitments.empty()) return fail("empty disabled evo quorum type");
        const bool rotation_enabled{llmq::IsQuorumRotationEnabled(params, base_index)};
        const size_t active_count{static_cast<size_t>(params.signingActiveQuorumCount)};
        const size_t total_count{SnapshotCommitmentCount(params, rotation_enabled)};
        if (data.rotation_enabled != rotation_enabled || data.active_commitments.size() > active_count ||
            data.safety_commitments.size() > total_count - active_count) {
            return fail("evo quorum params/count mismatch");
        }

        std::set<int16_t> active_indexes;
        std::vector<const CBlockIndex*> carried;
        const auto check_commitment = [&](const MinedQuorumCommitment& entry) EXCLUSIVE_LOCKS_REQUIRED(::cs_main) {
            if (!ValidateCommitmentAgainstChain(entry, chainman, base_index, params, rotation_enabled)) return false;
            const CBlockIndex* quorum{chainman.m_blockman.LookupBlockIndex(entry.quorum_base_block_hash)};
            const CBlockIndex* work{
                base_index->GetAncestor(EvoSnapshotWorkHeight(consensus, params, rotation_enabled, quorum->nHeight))};
            if (work == nullptr || entry.work_block_hash != work->GetBlockHash()) return false;
            carried.push_back(quorum);
            return true;
        };
        for (const auto& entry : data.active_commitments) {
            if (!check_commitment(entry) ||
                (rotation_enabled && !active_indexes.insert(entry.commitment.quorumIndex).second)) {
                return fail("invalid active evo quorum commitment chain data");
            }
        }
        for (const auto& entry : data.safety_commitments) {
            if (!check_commitment(entry)) return fail("invalid safety evo quorum commitment chain data");
        }

        EvoSnapshotDependencyPlan plan;
        if (!PlanEvoSnapshotDependencies(chainman, base_index, params.type, carried, plan, error)) return false;

        std::set<uint256> expected_cycles;
        for (const CBlockIndex* cycle : plan.rotation_cycles)
            expected_cycles.insert(cycle->GetBlockHash());
        std::set<uint256> actual_cycles;
        for (const auto& entry : data.rotation_snapshots)
            actual_cycles.insert(entry.cycle_base_block_hash);
        if (actual_cycles != expected_cycles) return fail("evo rotation cycles differ from the planned set");
        for (const auto& entry : data.rotation_snapshots) {
            // A planned cycle: a known ancestor of the base, above the work depth.
            const CBlockIndex* cycle{Assert(chainman.m_blockman.LookupBlockIndex(entry.cycle_base_block_hash))};
            const CBlockIndex* work{Assert(cycle->GetAncestor(cycle->nHeight - llmq::WORK_DIFF_DEPTH))};
            if (entry.work_block_hash != work->GetBlockHash()) return fail("evo rotation cycle/work ancestor mismatch");
            const auto historical{historical_lists.find(work->GetBlockHash())};
            if (historical == historical_lists.end() ||
                entry.snapshot.activeQuorumMembers.size() != historical->second.GetCounts().total()) {
                return fail("evo rotation bitset/work-block MN count mismatch");
            }
            // The ordinary computation stores no quarter for a cycle with too
            // few masternodes, so later cycles read without one, and a base
            // that needs such a cycle is unsupported. This catches the first
            // no-store path only; the pin covers the other (no member added).
            if (historical->second.GetCounts().enabled() < static_cast<size_t>(params.size / 4)) {
                return fail("evo rotation quarter carried for a cycle the ordinary computation leaves without one");
            }
        }
        if (data.bootstrap_work != plan.bootstrap_work) return fail("evo bootstrap work differs from the planned set");

        for (const auto& work : plan.work_blocks) {
            const uint256& work_hash{work.work_index->GetBlockHash()};
            required_work_hashes.insert(work_hash);
            required_modifiers.emplace(params.type, work_hash);
            const auto carried_modifier{carried_modifiers.find(std::make_pair(params.type, work_hash))};
            if (carried_modifier == carried_modifiers.end()) return fail("missing planned evo quorum modifier");
            // Recompute wherever the chain allows; only an unreadable v20 work
            // block leaves the carried value to the pin.
            std::string modifier_error;
            const auto expected{
                ExactWorkModifier(params, consensus, base_index, work, carried_modifier->second, modifier_error)};
            if (!expected) return fail(modifier_error);
            if (*expected != carried_modifier->second) return fail("evo carried quorum modifier mismatch");
        }
    }
    if (actual.size() != known_count) return fail("unknown evo quorum type");

    std::set<uint256> historical_hashes;
    for (const auto& entry : snapshot.historical_mn_list_diffs)
        historical_hashes.insert(entry.block_hash);
    required_work_hashes.erase(base_index->GetBlockHash());
    if (historical_hashes != required_work_hashes) return fail("historical evo MN lists differ from the planned set");
    std::set<std::pair<Consensus::LLMQType, uint256>> actual_modifiers;
    for (const auto& [key, _] : carried_modifiers)
        actual_modifiers.insert(key);
    if (actual_modifiers != required_modifiers) return fail("evo quorum modifiers differ from the planned set");

    if (!MoneyRange(snapshot.credit_pool.locked) || !MoneyRange(snapshot.credit_pool.currentLimit) ||
        !MoneyRange(snapshot.credit_pool.latelyUnlocked))
        return fail("invalid evo credit pool monetary value");
    for (const auto& [bit, height] : snapshot.mnhf_signals) {
        if (bit >= VERSIONBITS_NUM_BITS || height < 0 || height > base_index->nHeight) {
            return fail("invalid evo MNHF signal bit/height");
        }
    }
    return CheckScanCoverage(snapshot, chainman, base_index, /*full_index=*/nullptr, error);
}

bool VerifyEvoSnapshotBaseBlock(const EvoSnapshot& snapshot, const CBlockIndex& base_index,
                                const Consensus::Params& consensus, std::string& error)
{
    if (!snapshot.base_coinbase) {
        error = "evo snapshot without a base coinbase";
        return false;
    }
    if (ComputeMerkleRootFromBranch(snapshot.base_coinbase->GetHash(), snapshot.base_coinbase_branch, 0) !=
        base_index.hashMerkleRoot) {
        error = "evo snapshot base coinbase is not proven by the base header";
        return false;
    }
    const bool legacy{!DeploymentActiveAt(base_index, consensus, Consensus::DEPLOYMENT_V19)};
    const auto cbtx{DecodeCbTxPayload(*snapshot.base_coinbase, legacy)};
    if (!cbtx) {
        error = "evo snapshot base coinbase has no valid CbTx payload";
        return false;
    }
    return VerifyEvoSnapshotCbTx(snapshot, *cbtx, error);
}

bool SeedEvoSnapshotState(const EvoViewToken& view, const EvoSnapshot& snapshot, CSpecialTxProcessor& special_tx,
                          const node::BlockManager& blockman, const CBlockIndex* base_index, std::string& error)
{
    auto& dmnman{special_tx.DeterministicMNManager()};
    auto& qblockman{special_tx.QuorumBlockProcessor()};
    auto& qsnapman{special_tx.QuorumSnapshotManager()};
    auto& cpoolman{*special_tx.m_cpoolman};
    auto& mnhfman{*special_tx.m_mnhfman};
    AssertLockHeld(::cs_main);
    const auto fail = [&](const std::string& message) {
        error = message;
        return false;
    };
    if (base_index == nullptr || base_index->GetBlockHash() != snapshot.base_block_hash) {
        return fail("evo snapshot seeding base mismatch");
    }
    std::map<uint256, CDeterministicMNList> historical_lists;
    if (!ReconstructHistoricalMNLists(snapshot, historical_lists, error)) return false;
    if (!dmnman.SeedListForBlock(view, snapshot.mn_list)) return fail("failed to seed the base MN list");
    for (const auto& [_, list] : historical_lists) {
        if (!dmnman.SeedListForBlock(view, list)) return fail("failed to seed a historical MN list");
    }
    for (const auto& modifier : snapshot.quorum_modifiers) {
        if (!qsnapman.SeedQuorumModifier(view, modifier.llmq_type, modifier.work_block_hash, modifier.modifier)) {
            return fail("failed to seed a quorum score modifier");
        }
    }
    for (const auto& data : snapshot.quorums) {
        for (const auto* commitments : {&data.active_commitments, &data.safety_commitments}) {
            for (const auto& entry : *commitments) {
                if (!qblockman.SeedMinedCommitment(view, data.llmq_type, entry.quorum_base_block_hash, entry.commitment,
                                                   entry.mined_block_hash)) {
                    return fail("failed to seed a mined quorum commitment");
                }
            }
        }
        for (const auto& rotation : data.rotation_snapshots) {
            const CBlockIndex* cycle{blockman.LookupBlockIndex(rotation.cycle_base_block_hash)};
            if (cycle == nullptr || !qsnapman.SeedSnapshotForBlock(view, data.llmq_type, cycle, rotation.snapshot)) {
                return fail("failed to seed a rotation quarter");
            }
        }
    }
    if (!cpoolman.SeedSnapshot(view, base_index, snapshot.credit_pool) ||
        !cpoolman.SeedHistory(view, base_index, snapshot.credit_history)) {
        return fail("failed to seed the credit pool");
    }
    if (!mnhfman.SeedSignals(view, base_index, snapshot.mnhf_signals)) return fail("failed to seed the MNHF signals");
    if (snapshot.base_coinbase && !special_tx.SeedBaseCoinbase(view, base_index, *snapshot.base_coinbase)) {
        return fail("failed to seed the base coinbase");
    }
    for (const auto& params : special_tx.ConsensusParams().llmqs) {
        if (!qblockman.SeedScanCoverage(view, params.type, EvoSnapshotScanCoverage(snapshot, params))) {
            return fail("failed to seed quorum scan coverage");
        }
    }
    return true;
}

bool DryRunEvoSnapshot(const EvoSnapshot& snapshot, const ChainstateManager& chainman, CSpecialTxProcessor& special_tx,
                       const CBlockIndex* base_index, std::string& error)
{
    AssertLockHeld(::cs_main);
    const auto& consensus{chainman.GetConsensus()};
    const llmq::UtilParameters util{special_tx.DeterministicMNManager(), special_tx.QuorumSnapshotManager(), chainman,
                                    Assert(base_index)};
    try {
        for (const auto& data : snapshot.quorums) {
            const auto& params{SnapshotLLMQParams(data.llmq_type)};
            for (const auto* commitments : {&data.active_commitments, &data.safety_commitments}) {
                for (const auto& entry : *commitments) {
                    const CBlockIndex* quorum{chainman.m_blockman.LookupBlockIndex(entry.quorum_base_block_hash)};
                    if (quorum == nullptr) {
                        error = "evo snapshot dry run: unknown carried quorum";
                        return false;
                    }
                    (void)llmq::utils::GetAllQuorumMembers(data.llmq_type, util.replace_index(quorum),
                                                           /*reset_cache=*/true);
                }
            }
            if (!chainman.IsQuorumTypeEnabled(data.llmq_type, base_index)) continue;
            const int h{base_index->nHeight - base_index->nHeight % params.dkgInterval};
            if (h > 0) {
                (void)llmq::utils::GetAllQuorumMembers(data.llmq_type, util.replace_index(base_index->GetAncestor(h)),
                                                       /*reset_cache=*/true);
            }
            // The next cycle has no header yet; its members are computable
            // from the section only when its work block is at or below the base.
            const int next{h + params.dkgInterval};
            const int work_height{EvoSnapshotWorkHeight(consensus, params, data.rotation_enabled, next)};
            if (work_height >= 0 && work_height <= base_index->nHeight) {
                const CBlockIndex* work{base_index->GetAncestor(work_height)};
                // Before v20 the modifier needs the next cycle's own block hash.
                if (!llmq::utils::ComputeQuorumMembersFromWorkBlock(data.llmq_type, util.replace_index(work), work, next) &&
                    DeploymentActiveAfter(work, consensus, Consensus::DEPLOYMENT_V20)) {
                    error = "evo snapshot dry run: the members of the next cycle are not computable";
                    return false;
                }
            }
        }
    } catch (const LocalStateError& e) {
        error = strprintf("evo snapshot dry run: %s", e.what());
        return false;
    }
    return true;
}

namespace {
// Captures are NORMAL records that exist only while a snapshot is being
// validated: (prefix, kind, block hash, LLMQ type).
const std::string EVODB_SNAPSHOT_CAPTURE{"b_evo_cap"};
enum class CaptureKind : uint8_t {
    MODIFIER,
    CREDIT_HISTORY,
    BASE_COINBASE,
    BASE_CREDIT_POOL
};
using CaptureKey = std::tuple<std::string, uint8_t, uint256, uint8_t>;

CaptureKey MakeCaptureKey(CaptureKind kind, const uint256& block_hash,
                          Consensus::LLMQType llmq_type = Consensus::LLMQType::LLMQ_NONE)
{
    return {EVODB_SNAPSHOT_CAPTURE, static_cast<uint8_t>(kind), block_hash, static_cast<uint8_t>(llmq_type)};
}
} // namespace

std::optional<EvoSnapshotCapturePlan> PlanEvoSnapshotCaptures(const EvoSnapshot& snapshot, const ChainstateManager& chainman)
{
    AssertLockHeld(::cs_main);
    const CBlockIndex* base{chainman.m_blockman.LookupBlockIndex(snapshot.base_block_hash)};
    if (base == nullptr) return std::nullopt;
    EvoSnapshotCapturePlan plan;
    plan.base = base->GetBlockHash();
    plan.base_height = base->nHeight;
    if (!snapshot.base_coinbase) return plan;
    plan.heights.insert(base->nHeight);
    plan.window_first = base->nHeight - static_cast<int>(snapshot.credit_history.size()) + 1;
    for (int height{plan.window_first}; height <= base->nHeight; ++height)
        plan.heights.insert(height);
    for (const auto& entry : snapshot.quorum_modifiers) {
        const CBlockIndex* work{chainman.m_blockman.LookupBlockIndex(entry.work_block_hash)};
        if (work == nullptr || base->GetAncestor(work->nHeight) != work) return std::nullopt;
        // Only a v20 work block's modifier comes from its body.
        if (!DeploymentActiveAfter(work, chainman.GetConsensus(), Consensus::DEPLOYMENT_V20)) continue;
        plan.modifier_blocks[work->GetBlockHash()].push_back(entry.llmq_type);
        plan.heights.insert(work->nHeight);
    }
    return plan;
}

bool WriteEvoSnapshotCaptures(CEvoDB& evodb, CCreditPoolManager& cpoolman, const EvoSnapshotCapturePlan& plan,
                              const CBlock& block, const CBlockIndex& index, const Consensus::Params& consensus,
                              std::string& error)
{
    if (!plan.heights.contains(index.nHeight) || index.nHeight > plan.base_height) return true;
    const uint256 hash{index.GetBlockHash()};
    if (const auto it{plan.modifier_blocks.find(hash)}; it != plan.modifier_blocks.end()) {
        for (const auto llmq_type : it->second) {
            const uint256 modifier{
                llmq::utils::GetWorkBlockModifier(SnapshotLLMQParams(llmq_type), consensus, block, index)};
            if (!evodb.WriteDerived(MakeCaptureKey(CaptureKind::MODIFIER, hash, llmq_type), modifier)) {
                error = "conflicting quorum modifier capture";
                return false;
            }
        }
    }
    if (index.nHeight >= plan.window_first) {
        const auto entry{CCreditPoolManager::ReadHistoryEntry(&index, consensus, error)};
        if (!entry || !evodb.WriteDerived(MakeCaptureKey(CaptureKind::CREDIT_HISTORY, hash), *entry)) {
            if (error.empty()) error = "conflicting credit-pool window capture";
            return false;
        }
    }
    if (hash == plan.base) {
        if (block.vtx.empty() || !evodb.WriteDerived(MakeCaptureKey(CaptureKind::BASE_COINBASE, hash),
                                                     std::make_pair(block.vtx[0], BlockMerkleBranch(block, 0)))) {
            error = "conflicting base coinbase capture";
            return false;
        }
        // The pool at the base folds in every block back to the last pool on
        // disk, which may lie below the window.
        std::optional<CCreditPool> pool;
        try {
            pool = cpoolman.GetCreditPool(&index);
        } catch (const LocalStateError&) {
            // This node's own data, e.g. a pruned body: the caller stalls.
            throw;
        } catch (const std::exception& e) {
            error = strprintf("the credit pool at the base is not computable: %s", e.what());
            return false;
        }
        if (!evodb.WriteDerived(MakeCaptureKey(CaptureKind::BASE_CREDIT_POOL, hash), *pool)) {
            error = "conflicting base credit pool capture";
            return false;
        }
    }
    return true;
}

EvoSnapshotCaptures ReadEvoSnapshotCaptures(CEvoDB& evodb, const EvoSnapshotCapturePlan& plan,
                                            const ChainstateManager& chainman)
{
    AssertLockHeld(::cs_main);
    EvoSnapshotCaptures captures;
    for (const auto& [hash, types] : plan.modifier_blocks) {
        for (const auto llmq_type : types) {
            uint256 modifier;
            if (evodb.Read(MakeCaptureKey(CaptureKind::MODIFIER, hash, llmq_type), modifier)) {
                captures.modifiers.emplace(std::make_pair(llmq_type, hash), modifier);
            }
        }
    }
    const CBlockIndex* base{chainman.m_blockman.LookupBlockIndex(plan.base)};
    if (base == nullptr || plan.window_first < 0) return captures;
    for (int height{plan.window_first}; height <= plan.base_height; ++height) {
        const uint256 hash{base->GetAncestor(height)->GetBlockHash()};
        CreditHistoryEntry entry;
        if (evodb.Read(MakeCaptureKey(CaptureKind::CREDIT_HISTORY, hash), entry)) {
            captures.credit_history.emplace(hash, entry);
        }
    }
    std::pair<CTransactionRef, std::vector<uint256>> coinbase;
    if (evodb.Read(MakeCaptureKey(CaptureKind::BASE_COINBASE, plan.base), coinbase)) {
        std::tie(captures.base_coinbase, captures.base_coinbase_branch) = std::move(coinbase);
    }
    if (CCreditPool pool; evodb.Read(MakeCaptureKey(CaptureKind::BASE_CREDIT_POOL, plan.base), pool)) {
        captures.base_credit_pool = std::move(pool);
    }
    return captures;
}

bool EraseAllEvoSnapshotCaptures(CEvoDB& evodb)
{
    CDBWrapper& db{evodb.GetRawDB()};
    DataStream prefix;
    prefix << EVODB_SNAPSHOT_CAPTURE;
    std::unique_ptr<CDBIterator> it{db.NewIterator()};
    CDBBatch batch{db};
    for (it->Seek(prefix); it->Valid(); it->Next()) {
        const auto key{it->GetKey()};
        if (key.size() < prefix.size() || !std::equal(prefix.begin(), prefix.end(), key.begin())) break;
        batch.Erase(key);
    }
    // Most startups have nothing to erase; spare them the synced write.
    return batch.SizeEstimate() == 0 || db.WriteBatch(batch, /*fSync=*/true);
}

void EraseEvoSnapshotCaptures(CEvoDB& evodb, const EvoSnapshotCapturePlan& plan, const ChainstateManager& chainman)
{
    AssertLockHeld(::cs_main);
    for (const auto& [hash, types] : plan.modifier_blocks) {
        for (const auto llmq_type : types)
            evodb.Erase(MakeCaptureKey(CaptureKind::MODIFIER, hash, llmq_type));
    }
    const CBlockIndex* base{chainman.m_blockman.LookupBlockIndex(plan.base)};
    if (base == nullptr || plan.window_first < 0) return;
    for (int height{plan.window_first}; height <= plan.base_height; ++height) {
        evodb.Erase(MakeCaptureKey(CaptureKind::CREDIT_HISTORY, base->GetAncestor(height)->GetBlockHash()));
    }
    evodb.Erase(MakeCaptureKey(CaptureKind::BASE_COINBASE, plan.base));
    evodb.Erase(MakeCaptureKey(CaptureKind::BASE_CREDIT_POOL, plan.base));
}

} // namespace evo
