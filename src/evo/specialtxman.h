// Copyright (c) 2018-2025 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_EVO_SPECIALTXMAN_H
#define BITCOIN_EVO_SPECIALTXMAN_H

#include <gsl/pointers.h>
#include <sync.h>
#include <threadsafety.h>

#include <optional>

class BlockValidationState;
class CBlock;
class CBlockIndex;
class CCbTx;
class CChain;
class CCoinsViewCache;
class CCreditPoolManager;
class CDeterministicMNList;
class CDeterministicMNManager;
class CProDisTx;
class CTransaction;
class ChainstateManager;
class Chainstate;
class CMNHFManager;
class TxValidationState;
struct MNListUpdates;

/** Where a special transaction is being validated. Some checks (currently only ProDisTx) must
 *  defer to RebuildListFromBlock in block context because the referenced masternode may have been
 *  registered earlier in the same block. */
enum class SpecialTxContext : uint8_t {
    Mempool,
    Block,
};

namespace chainlock {
class Chainlocks;
}
namespace Consensus { struct Params; }
namespace llmq {
class CQuorumBlockProcessor;
class CQuorumManager;
class CQuorumSnapshotManager;
} // namespace llmq

extern RecursiveMutex cs_main; // NOLINT(readability-redundant-declaration)

class CSpecialTxProcessor
{
private:
    CCreditPoolManager& m_cpoolman;
    CDeterministicMNManager& m_dmnman;
    CMNHFManager& m_mnhfman;
    llmq::CQuorumBlockProcessor& m_qblockman;
    llmq::CQuorumSnapshotManager& m_qsnapman;
    const ChainstateManager& m_chainman;
    const Consensus::Params& m_consensus_params;
    const chainlock::Chainlocks& m_chainlocks;
    const llmq::CQuorumManager& m_qman;

public:
    explicit CSpecialTxProcessor(CCreditPoolManager& cpoolman, CDeterministicMNManager& dmnman, CMNHFManager& mnhfman,
                                 llmq::CQuorumBlockProcessor& qblockman, llmq::CQuorumSnapshotManager& qsnapman,
                                 const ChainstateManager& chainman, const Consensus::Params& consensus_params,
                                 const chainlock::Chainlocks& chainlocks, const llmq::CQuorumManager& qman) :
        m_cpoolman(cpoolman),
        m_dmnman{dmnman},
        m_mnhfman{mnhfman},
        m_qblockman{qblockman},
        m_qsnapman{qsnapman},
        m_chainman(chainman),
        m_consensus_params{consensus_params},
        m_chainlocks{chainlocks},
        m_qman{qman}
    {
    }

    bool CheckSpecialTx(const CTransaction& tx, const CBlockIndex* pindexPrev, const CCoinsViewCache& view, bool check_sigs, TxValidationState& state)
        EXCLUSIVE_LOCKS_REQUIRED(::cs_main);
    bool ProcessSpecialTxsInBlock(Chainstate& chainstate, const CBlock& block, const CBlockIndex* pindex, const CCoinsViewCache& view, bool fJustCheck,
                                  bool fCheckCbTxMerkleRoots, BlockValidationState& state, std::optional<MNListUpdates>& updatesRet)
        EXCLUSIVE_LOCKS_REQUIRED(::cs_main);
    bool UndoSpecialTxsInBlock(const Chainstate& chainstate, const CBlock& block, const CBlockIndex* pindex, std::optional<MNListUpdates>& updatesRet)
        EXCLUSIVE_LOCKS_REQUIRED(::cs_main);


    // the returned list will not contain the correct block hash (we can't know it yet as the coinbase TX is not updated yet)
    bool BuildNewListFromBlock(const CBlock& block, gsl::not_null<const CBlockIndex*> pindexPrev,
                               const CCoinsViewCache& view, bool debugLogs, BlockValidationState& state,
                               CDeterministicMNList& mnListRet) EXCLUSIVE_LOCKS_REQUIRED(cs_main);

    // Variant that takes an explicit starting list instead of loading from GetListForBlock
    // Used for rebuilding diffs from trusted snapshots
    bool RebuildListFromBlock(const CBlock& block, gsl::not_null<const CBlockIndex*> pindexPrev,
                              const CDeterministicMNList& prevList, const CCoinsViewCache& view, bool debugLogs,
                              BlockValidationState& state, CDeterministicMNList& mnListRet);

private:
    bool CheckCreditPoolDiffForBlock(const CBlock& block, const CBlockIndex* pindex, const CCbTx& cbTx,
                                     BlockValidationState& state) EXCLUSIVE_LOCKS_REQUIRED(::cs_main);
};

/**
 * This helper does some trivial validations that doesn't depends on collateral and
 * masternode list. Use CheckPro*Tx for the full validation of transaction,
 * including bls signatures, list of masternodes and collateral
 */
template <typename ProTx>
std::optional<ProTx> GetValidatedPayload(const CTransaction& tx, gsl::not_null<const CBlockIndex*> pindexPrev,
                                         const ChainstateManager& chainman, TxValidationState& state);

/** Validates the bestCLSignature / bestCLHeightDiff fields embedded in a CbTx payload. */
bool CheckCbTxBestChainlock(const CCbTx& cbTx, const CBlockIndex* pindex, const Consensus::Params& consensus_params,
                            const CChain& chain, const llmq::CQuorumManager& qman,
                            const chainlock::Chainlocks& chainlocks, BlockValidationState& state);

bool CheckProRegTx(const CTransaction& tx, gsl::not_null<const CBlockIndex*> pindexPrev,
                   CDeterministicMNManager& dmnman, const CCoinsViewCache& view, const ChainstateManager& chainman,
                   TxValidationState& state, bool check_sigs);
bool CheckProUpServTx(const CTransaction& tx, gsl::not_null<const CBlockIndex*> pindexPrev, CDeterministicMNManager& dmnman,
                      const ChainstateManager& chainman, TxValidationState& state, bool check_sigs);
bool CheckProUpRegTx(const CTransaction& tx, gsl::not_null<const CBlockIndex*> pindexPrev,
                     CDeterministicMNManager& dmnman, const CCoinsViewCache& view, const ChainstateManager& chainman,
                     TxValidationState& state, bool check_sigs);
bool CheckProUpRevTx(const CTransaction& tx, gsl::not_null<const CBlockIndex*> pindexPrev, CDeterministicMNManager& dmnman,
                     const ChainstateManager& chainman, TxValidationState& state, bool check_sigs);
bool CheckProDisTx(const CTransaction& tx, gsl::not_null<const CBlockIndex*> pindexPrev, CDeterministicMNManager& dmnman,
                   const ChainstateManager& chainman, TxValidationState& state, bool check_sigs,
                   SpecialTxContext context);
bool CheckProUpShareTx(const CTransaction& tx, gsl::not_null<const CBlockIndex*> pindexPrev,
                       CDeterministicMNManager& dmnman, const ChainstateManager& chainman, TxValidationState& state,
                       bool check_sigs);
bool CheckProUpSharedRegTx(const CTransaction& tx, gsl::not_null<const CBlockIndex*> pindexPrev,
                           CDeterministicMNManager& dmnman, const ChainstateManager& chainman,
                           TxValidationState& state, bool check_sigs);
/** Full ProDisTx validation against a given masternode list and spend height. Used by mempool
 *  validation (tip list, tip height + 1) and by RebuildListFromBlock (the evolving list, which
 *  includes masternodes registered earlier in the same block, per the DIP). */
bool CheckProDisTxForList(const CTransaction& tx, const CProDisTx& ptx, const CDeterministicMNList& mnList,
                          int nSpendHeight, TxValidationState& state, bool check_sigs);

/** Consensus rule (v24): an input whose prevout pays the shared-collateral template script may
 *  only be spent by a ProDisTx. Applies to every transaction; callers gate on v24 activation. */
bool CheckSharedCollateralSpends(const CTransaction& tx, const CCoinsViewCache& view, TxValidationState& state);
/** Consensus rule (v24): an output paying the shared-collateral template script is only valid as
 *  the collateral output of a shared registration. Applies to every transaction, including the
 *  coinbase; callers gate on v24 activation. */
bool CheckSharedCollateralTemplateOutputs(const CTransaction& tx, TxValidationState& state);


/**
 * Asset lock transactions with more than 100 inputs (and so over ~20 kB) can not
 * be processed by Platform, so Dash Core nodes should not relay them: they are
 * marked non-standard, which keeps the network from propagating them over p2p.
 *
 * Asset lock v2 is enabled by the v24 fork, but Platform can not process it yet.
 * It is kept non-standard so it can be enabled later without another hard fork.
 *
 * These are relay/mempool checks only: a rejected transaction stays valid inside
 * a block.
 *
 * Returns false (with `reason` set) for a non-standard asset lock, and
 * true for any transaction that is not an asset lock or not special-tx.
 */
bool IsStandardSpecialTx(const CTransaction& tx, std::string& reason);
#endif // BITCOIN_EVO_SPECIALTXMAN_H
