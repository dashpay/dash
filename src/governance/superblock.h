// Copyright (c) 2014-2025 The Dash Core developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_GOVERNANCE_SUPERBLOCK_H
#define BITCOIN_GOVERNANCE_SUPERBLOCK_H

#include <consensus/amount.h>
#include <sync.h>
#include <uint256.h>

#include <atomic>
#include <map>
#include <memory>
#include <vector>

class CChain;
class CDeterministicMNList;
class CGovernanceObject;
class CSuperblock;
class CTransaction;
class CTxOut;

using CSuperblock_sptr = std::shared_ptr<CSuperblock>;

namespace governance {

/**
 * Owns the set of active superblock triggers. Trigger lifecycle is fully
 * owned here; CGovernanceManager pushes new TRIGGER objects in via
 * AddTrigger() and notifies us via RemoveTrigger() when it erases the
 * underlying object from its store. Each entry holds a strong reference
 * to the underlying CGovernanceObject so we never look it up by hash.
 */
class SuperblockManager
{
public:
    bool IsValid() const { return m_loaded; }
    void SetLoaded(bool loaded) { m_loaded = loaded; }

    /** Register a TRIGGER governance object. Returns true if the resulting
     *  trigger is live (not already height-expired). */
    bool AddTrigger(std::shared_ptr<CGovernanceObject> obj, int cachedHeight)
        EXCLUSIVE_LOCKS_REQUIRED(!cs_sb);

    /** Single-direction notification from CGovernanceManager when it erases
     *  a TRIGGER object from its store. */
    void RemoveTrigger(const uint256& hash) EXCLUSIVE_LOCKS_REQUIRED(!cs_sb);

    /** Prune triggers that are invalid or height-expired. Calls
     *  PrepareDeletion() on the underlying object so CGovernanceManager
     *  picks it up on its own sweep. */
    void Clean(int cachedHeight) EXCLUSIVE_LOCKS_REQUIRED(!cs_sb);

    void Clear() EXCLUSIVE_LOCKS_REQUIRED(!cs_sb);

    std::vector<CSuperblock_sptr> GetActiveTriggers() const EXCLUSIVE_LOCKS_REQUIRED(!cs_sb);

    bool GetBestSuperblock(const CDeterministicMNList& tip_mn_list,
                           CSuperblock_sptr& sbRet, int nBlockHeight) const
        EXCLUSIVE_LOCKS_REQUIRED(!cs_sb);

    bool IsSuperblockTriggered(const CDeterministicMNList& tip_mn_list, int nBlockHeight)
        EXCLUSIVE_LOCKS_REQUIRED(!cs_sb);

    bool IsValidSuperblock(const CChain& active_chain, const CDeterministicMNList& tip_mn_list,
                           const CTransaction& txNew, int nBlockHeight, CAmount blockReward) const
        EXCLUSIVE_LOCKS_REQUIRED(!cs_sb);

    bool GetSuperblockPayments(const CDeterministicMNList& tip_mn_list, int nBlockHeight,
                               std::vector<CTxOut>& voutSuperblockRet) const
        EXCLUSIVE_LOCKS_REQUIRED(!cs_sb);

    void ExecuteBestSuperblock(const CDeterministicMNList& tip_mn_list, int nBlockHeight)
        EXCLUSIVE_LOCKS_REQUIRED(!cs_sb);

private:
    struct TriggerEntry {
        CSuperblock_sptr sb;
        std::shared_ptr<CGovernanceObject> obj;
    };

    bool GetBestSuperblockInternal(const CDeterministicMNList& tip_mn_list,
                                   CSuperblock_sptr& sbRet, int nBlockHeight) const
        EXCLUSIVE_LOCKS_REQUIRED(cs_sb);

    mutable Mutex cs_sb;
    std::atomic<bool> m_loaded{false};
    std::map<uint256, TriggerEntry> m_triggers GUARDED_BY(cs_sb);
};

} // namespace governance

#endif // BITCOIN_GOVERNANCE_SUPERBLOCK_H
