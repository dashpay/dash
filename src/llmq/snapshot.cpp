// Copyright (c) 2021-2025 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <llmq/snapshot.h>

#include <chainparams.h>
#include <evo/deterministicmns.h>
#include <evo/evodb.h>
#include <evo/simplifiedmns.h>
#include <evo/smldiff.h>
#include <evo/snapshot_types.h>
#include <llmq/blockprocessor.h>
#include <llmq/commitment.h>
#include <node/blockstorage.h>
#include <validation.h>

#include <univalue.h>

#include <algorithm>

using node::CBlockIndexHeightOnlyComparator;

namespace {
constexpr std::string_view DB_QUORUM_SNAPSHOT{"llmq_S"};

bool CheckBlockDataAvailable(gsl::not_null<const CBlockIndex*> pindex, std::string& error)
    EXCLUSIVE_LOCKS_REQUIRED(::cs_main)
{
    if (pindex->nStatus & BLOCK_HAVE_DATA) return true;
    error = strprintf("block data for block %s %s", pindex->GetBlockHash().ToString(), BLOCK_DATA_UNAVAILABLE_SUFFIX);
    return false;
}

//! Constructs a llmq::CycleData and populate it with metadata
std::optional<llmq::CycleData> ConstructCycle(llmq::CQuorumSnapshotManager& qsnapman,
                                              const Consensus::LLMQType& llmq_type, bool skip_snap,
                                              bool need_block_data, int32_t height,
                                              gsl::not_null<const CBlockIndex*> index_tip, std::string& error)
    EXCLUSIVE_LOCKS_REQUIRED(::cs_main)
{
    llmq::CycleData ret;
    // The cycle block itself only keys EvoDB-backed snapshot metadata, so it
    // needs no block data. The work block becomes the target of an inner
    // mnlistdiff (read from disk) only on the legacy construction path.
    ret.m_cycle_index = index_tip->GetAncestor(height);
    if (!ret.m_cycle_index) {
        error = "Cannot find block";
        return std::nullopt;
    }
    ret.m_work_index = ret.m_cycle_index->GetAncestor(ret.m_cycle_index->nHeight - llmq::WORK_DIFF_DEPTH);
    if (!ret.m_work_index) {
        error = "Cannot find work block";
        return std::nullopt;
    }
    if (need_block_data && !CheckBlockDataAvailable(ret.m_work_index, error)) return std::nullopt;
    if (!skip_snap) {
        if (auto opt_snap = qsnapman.GetSnapshotForBlock(llmq_type, ret.m_cycle_index); opt_snap.has_value()) {
            ret.m_snap = opt_snap.value();
        } else {
            error = "Cannot find quorum snapshot";
            // Locally missing below a snapshot base, not the requester's fault.
            if (qsnapman.IsCarriedOnly(ret.m_cycle_index)) {
                error = strprintf("Quorum snapshot for block %s %s", ret.m_cycle_index->GetBlockHash().ToString(),
                                  BLOCK_DATA_UNAVAILABLE_SUFFIX);
            }
            return std::nullopt;
        }
    }
    return ret;
}

//! Add a base block, keeping the by-height ordering GetLastBaseBlockHash() requires.
void InsertBaseBlockSorted(std::vector<const CBlockIndex*>& baseBlockIndexes, const CBlockIndex* blockIndex)
{
    const auto pos{std::upper_bound(baseBlockIndexes.begin(), baseBlockIndexes.end(), blockIndex,
                                    CBlockIndexHeightOnlyComparator())};
    baseBlockIndexes.insert(pos, blockIndex);
}
} // anonymous namespace

namespace llmq {
bool CQuorumSnapshotManager::IsCarriedOnly(const CBlockIndex* pindex) const
{
    const EvoViewToken view{m_evoDb.CurrentView()};
    return view.identity == EvoDbIdentity::SNAPSHOT && pindex->nHeight <= view.base_height;
}

bool BuildQuorumRotationInfo(CDeterministicMNManager& dmnman, CQuorumSnapshotManager& qsnapman,
                             const ChainstateManager& chainman, const CQuorumManager& qman,
                             const CQuorumBlockProcessor& qblockman, const CGetQuorumRotationInfo& request,
                             bool use_legacy_construction, CQuorumRotationInfo& response, std::string& errorRet)
{
    AssertLockHeld(::cs_main);

    // Peers are rejected at deserialization; this covers callers that build the request in memory (RPC).
    if (request.baseBlockHashes.size() > MAX_BASE_BLOCK_HASHES) {
        errorRet = strprintf("too many baseBlockHashes: %d > %d", request.baseBlockHashes.size(), MAX_BASE_BLOCK_HASHES);
        return false;
    }

    std::vector<const CBlockIndex*> baseBlockIndexes;
    if (request.baseBlockHashes.size() == 0) {
        const CBlockIndex* blockIndex = chainman.ActiveChain().Genesis();
        if (!blockIndex) {
            errorRet = strprintf("genesis block not found");
            return false;
        }
        baseBlockIndexes.push_back(blockIndex);
    } else {
        for (const auto& blockHash : request.baseBlockHashes) {
            const CBlockIndex* blockIndex = chainman.m_blockman.LookupBlockIndex(blockHash);
            if (!blockIndex) {
                errorRet = strprintf("block %s not found", blockHash.ToString());
                return false;
            }
            if (!chainman.ActiveChain().Contains(blockIndex)) {
                errorRet = strprintf("block %s is not in the active chain", blockHash.ToString());
                return false;
            }
            // No availability check: these are only diff bases, which need
            // EvoDB-backed state rather than block data; the inner
            // BuildSimplifiedMNListDiff calls surface the sentinel error if
            // a snapshot node cannot construct a base list yet.
            baseBlockIndexes.push_back(blockIndex);
        }
        // GetLastBaseBlockHash() requires the list sorted by height, and the legacy path (served to
        // peers < EFFICIENT_QRINFO_VERSION) also reads baseBlockIndexes.back(). Later inserts keep
        // the order via InsertBaseBlockSorted().
        std::sort(baseBlockIndexes.begin(), baseBlockIndexes.end(), CBlockIndexHeightOnlyComparator());
        if (!use_legacy_construction) {
            // Only deduplicate on the non-legacy path; leave the legacy path untouched so the
            // wire response to older peers stays bit-for-bit identical to the pre-fix behavior.
            baseBlockIndexes.erase(std::unique(baseBlockIndexes.begin(), baseBlockIndexes.end()),
                                   baseBlockIndexes.end());
        }
    }

    const CBlockIndex* tipBlockIndex = chainman.ActiveChain().Tip();
    if (!tipBlockIndex) {
        errorRet = strprintf("tip block not found");
        return false;
    }
    if (!CheckBlockDataAvailable(tipBlockIndex, errorRet)) return false;
    if (use_legacy_construction) {
        // Build MN list Diff always with highest baseblock
        if (!BuildSimplifiedMNListDiff(dmnman, chainman, qblockman, qman, baseBlockIndexes.back()->GetBlockHash(),
                                       tipBlockIndex->GetBlockHash(), response.mnListDiffTip, errorRet)) {
            return false;
        }
    }

    const CBlockIndex* blockIndex = chainman.m_blockman.LookupBlockIndex(request.blockRequestHash);
    if (!blockIndex) {
        errorRet = strprintf("block not found");
        return false;
    }
    if (!CheckBlockDataAvailable(blockIndex, errorRet)) return false;

    // Quorum rotation is enabled only for InstantSend atm.
    Consensus::LLMQType llmqType = Params().GetConsensus().llmqTypeDIP0024InstantSend;

    // Since the returned quorums are in reversed order, the most recent one is at index 0
    const auto& llmq_params_opt = Params().GetLLMQ(llmqType);
    assert(llmq_params_opt.has_value());

    const int cycleLength = llmq_params_opt->dkgInterval;

    auto cycle_base_opt = ConstructCycle(qsnapman, llmqType, /*skip_snap=*/true,
                                         /*need_block_data=*/use_legacy_construction,
                                         /*height=*/blockIndex->nHeight - (blockIndex->nHeight % cycleLength),
                                         blockIndex, errorRet);
    if (!cycle_base_opt.has_value()) {
        return false;
    }
    if (use_legacy_construction) {
        // Build MN list Diff always with highest baseblock
        if (!BuildSimplifiedMNListDiff(dmnman, chainman, qblockman, qman,
                                       GetLastBaseBlockHash(baseBlockIndexes, cycle_base_opt->m_work_index),
                                       cycle_base_opt->m_work_index->GetBlockHash(), response.mnListDiffH, errorRet)) {
            return false;
        }
    }

    response.extraShare = request.extraShare;

    auto target_cycles{response.GetCycles()};
    for (size_t idx{0}; idx < target_cycles.size(); idx++) {
        auto cycle_opt = ConstructCycle(qsnapman, llmqType, /*skip_snap=*/false,
                                        /*need_block_data=*/use_legacy_construction,
                                        /*height=*/cycle_base_opt->m_cycle_index->nHeight - (cycleLength * (idx + 1)),
                                        tipBlockIndex, errorRet);
        if (!cycle_opt.has_value()) {
            return false;
        }
        if (use_legacy_construction) {
            if (!BuildSimplifiedMNListDiff(dmnman, chainman, qblockman, qman,
                                           GetLastBaseBlockHash(baseBlockIndexes, cycle_opt->m_work_index),
                                           cycle_opt->m_work_index->GetBlockHash(), cycle_opt->m_diff, errorRet)) {
                return false;
            }
        }
        *target_cycles[idx] = cycle_opt.value();
    }

    std::set<int> snapshotHeightsNeeded;
    for (const auto& obj : qblockman.GetLastMinedCommitmentsPerQuorumIndexUntilBlock(llmqType, blockIndex, /*cycle=*/0)) {
        auto [qc, minedBlockHash] = qblockman.GetMinedCommitment(llmqType, obj->GetBlockHash());
        if (minedBlockHash == uint256::ZERO) {
            return false;
        }
        response.lastCommitmentPerIndex.emplace_back(std::move(qc));

        int quorumCycleStartHeight = obj->nHeight - (obj->nHeight % llmq_params_opt->dkgInterval);
        snapshotHeightsNeeded.insert(quorumCycleStartHeight - cycleLength);
        snapshotHeightsNeeded.insert(quorumCycleStartHeight - 2 * cycleLength);
        snapshotHeightsNeeded.insert(quorumCycleStartHeight - 3 * cycleLength);
    }

    for (auto* cycle : target_cycles) {
        snapshotHeightsNeeded.erase(cycle->m_cycle_index->nHeight);
    }

    for (const auto& h : snapshotHeightsNeeded) {
        auto cycle_opt = ConstructCycle(qsnapman, llmqType, /*skip_snap=*/false,
                                        /*need_block_data=*/use_legacy_construction, /*height=*/h, tipBlockIndex,
                                        errorRet);
        if (!cycle_opt.has_value()) {
            return false;
        }
        response.quorumSnapshotList.push_back(cycle_opt->m_snap);
        CSimplifiedMNListDiff mnhneeded;
        if (!BuildSimplifiedMNListDiff(dmnman, chainman, qblockman, qman,
                                       GetLastBaseBlockHash(baseBlockIndexes, cycle_opt->m_work_index),
                                       cycle_opt->m_work_index->GetBlockHash(), mnhneeded, errorRet)) {
            return false;
        }
        if (!use_legacy_construction) {
            InsertBaseBlockSorted(baseBlockIndexes, cycle_opt->m_work_index);
        }
        response.mnListDiffList.push_back(mnhneeded);
    }

    if (!use_legacy_construction) {
        for (size_t n = target_cycles.size(); n > 0; --n) {
            auto* cycle{target_cycles[n - 1]};
            if (!BuildSimplifiedMNListDiff(dmnman, chainman, qblockman, qman,
                                           GetLastBaseBlockHash(baseBlockIndexes, cycle->m_work_index),
                                           cycle->m_work_index->GetBlockHash(), cycle->m_diff, errorRet)) {
                return false;
            }
            InsertBaseBlockSorted(baseBlockIndexes, cycle->m_work_index);
        }

        if (!BuildSimplifiedMNListDiff(dmnman, chainman, qblockman, qman,
                                       GetLastBaseBlockHash(baseBlockIndexes, cycle_base_opt->m_work_index),
                                       cycle_base_opt->m_work_index->GetBlockHash(), response.mnListDiffH, errorRet)) {
            return false;
        }
        InsertBaseBlockSorted(baseBlockIndexes, cycle_base_opt->m_work_index);

        if (!BuildSimplifiedMNListDiff(dmnman, chainman, qblockman, qman,
                                       GetLastBaseBlockHash(baseBlockIndexes, tipBlockIndex),
                                       tipBlockIndex->GetBlockHash(), response.mnListDiffTip, errorRet)) {
            return false;
        }
    }
    return true;
}

uint256 GetLastBaseBlockHash(Span<const CBlockIndex* const> baseBlockIndexes, const CBlockIndex* blockIndex)
{
    const auto pos{std::upper_bound(baseBlockIndexes.begin(), baseBlockIndexes.end(), blockIndex,
                                    CBlockIndexHeightOnlyComparator())};
    if (pos == baseBlockIndexes.begin()) return Params().GetConsensus().hashGenesisBlock;
    return (*std::prev(pos))->GetBlockHash();
}

CQuorumSnapshot::CQuorumSnapshot() = default;

CQuorumSnapshot::CQuorumSnapshot(std::vector<bool> active_quorum_members, SnapshotSkipMode skip_mode,
                                 std::vector<int> skip_list) :
    activeQuorumMembers(std::move(active_quorum_members)),
    mnSkipListMode(skip_mode),
    mnSkipList(std::move(skip_list))
{
}

CQuorumSnapshot::~CQuorumSnapshot() = default;

CQuorumRotationInfo::CQuorumRotationInfo() = default;

CQuorumRotationInfo::~CQuorumRotationInfo() = default;

std::vector<CycleData*> CQuorumRotationInfo::GetCycles()
{
    std::vector<CycleData*> ret{&cycleHMinusC, &cycleHMinus2C, &cycleHMinus3C};
    if (extraShare) {
        if (!cycleHMinus4C.has_value()) { cycleHMinus4C = CycleData{}; }
        ret.emplace_back(&(cycleHMinus4C.value()));
    }
    return ret;
}

CQuorumSnapshotManager::CQuorumSnapshotManager(CEvoDB& evoDb) :
    m_evoDb{evoDb},
    quorumSnapshotCache{[] { return Uint256LruHashMap<CQuorumSnapshot>{32}; }}
{
}

CQuorumSnapshotManager::~CQuorumSnapshotManager() = default;

std::optional<CQuorumSnapshot> CQuorumSnapshotManager::GetSnapshotForBlock(const Consensus::LLMQType llmqType, const CBlockIndex* pindex)
{
    CQuorumSnapshot snapshot = {};

    auto snapshotHash = ::SerializeHash(std::make_pair(llmqType, pindex->GetBlockHash()));
    const EvoViewToken view{m_evoDb.CurrentView()};

    LOCK(snapshotCacheCs);
    auto& cache{quorumSnapshotCache.For(view)};
    // try using cache before reading from disk
    if (cache.get(snapshotHash, snapshot)) {
        return snapshot;
    }
    if (m_evoDb.Read(std::make_pair(DB_QUORUM_SNAPSHOT, snapshotHash), snapshot)) {
        cache.insert(snapshotHash, snapshot);
        return snapshot;
    }

    return std::nullopt;
}

void CQuorumSnapshotManager::StoreSnapshotForBlock(const Consensus::LLMQType llmqType, const CBlockIndex* pindex, const CQuorumSnapshot& snapshot)
{
    auto snapshotHash = ::SerializeHash(std::make_pair(llmqType, pindex->GetBlockHash()));
    const auto key{std::make_pair(DB_QUORUM_SNAPSHOT, snapshotHash)};

    AssertLockNotHeld(m_evoDb.cs);
    const EvoViewToken view{m_evoDb.CurrentView()};
    if (view.identity == EvoDbIdentity::SNAPSHOT && pindex->nHeight <= view.base_height) {
        // At or below the snapshot base the snapshot view holds only carried
        // state. A carried quarter must match the one just computed from
        // complete inputs. An uncarried one is a reconstruction output, never a
        // required input, so it is neither persisted nor cached.
        CQuorumSnapshot carried;
        if (m_evoDb.Read(key, carried) && ::SerializeHash(carried) != ::SerializeHash(snapshot)) {
            throw evo::SnapshotStateMismatchError(
                strprintf("carried quorum snapshot for %s at height %d differs from the computed one",
                          pindex->GetBlockHash().ToString(), pindex->nHeight));
        }
        return;
    }

    LOCK(snapshotCacheCs);
    m_evoDb.WriteDurable(key, snapshot);
    quorumSnapshotCache.For(view).insert(snapshotHash, snapshot);
}
} // namespace llmq
