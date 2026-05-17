// Copyright (c) 2014-2025 The Dash Core developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <governance/superblock.h>

#include <evo/deterministicmns.h>
#include <governance/classes.h>
#include <governance/object.h>
#include <governance/vote.h>
#include <key_io.h>
#include <logging.h>
#include <primitives/transaction.h>
#include <util/std23.h>
#include <util/time.h>

namespace governance {

bool SuperblockManager::AddTrigger(std::shared_ptr<CGovernanceObject> obj, int cachedHeight)
{
    AssertLockNotHeld(cs_sb);
    if (!obj) return false;

    uint256 nHash = obj->GetHash();

    LOCK(cs_sb);
    if (m_triggers.count(nHash)) {
        LogPrint(BCLog::GOBJECT, "SuperblockManager::%s -- Already have hash, nHash = %s, size = %s\n",
                 __func__, nHash.GetHex(), m_triggers.size());
        return false;
    }

    CSuperblock_sptr pSuperblock;
    try {
        pSuperblock = std::make_shared<CSuperblock>(*obj, nHash);
    } catch (std::exception& e) {
        LogPrintf("SuperblockManager::%s -- Error creating superblock: %s\n", __func__, e.what());
        return false;
    } catch (...) {
        LogPrintf("SuperblockManager::%s -- Unknown Error creating superblock\n", __func__);
        return false;
    }

    pSuperblock->SetStatus(SeenObjectStatus::Valid);
    m_triggers.emplace(nHash, TriggerEntry{pSuperblock, std::move(obj)});

    return !pSuperblock->IsExpired(cachedHeight);
}

void SuperblockManager::RemoveTrigger(const uint256& hash)
{
    LOCK(cs_sb);
    m_triggers.erase(hash);
}

void SuperblockManager::Clean(int cachedHeight)
{
    AssertLockNotHeld(cs_sb);
    LOCK(cs_sb);

    LogPrint(BCLog::GOBJECT, "SuperblockManager::%s -- m_triggers.size() = %d\n", __func__, m_triggers.size());

    for (auto it = m_triggers.begin(); it != m_triggers.end();) {
        bool remove = false;
        const auto& [sb, obj] = it->second;

        if (!sb) {
            LogPrint(BCLog::GOBJECT, "SuperblockManager::%s -- nullptr superblock\n", __func__);
            remove = true;
        } else {
            if (!obj || obj->GetObjectType() != GovernanceObject::TRIGGER) {
                LogPrint(BCLog::GOBJECT, "SuperblockManager::%s -- Unknown or non-trigger superblock\n", __func__);
                sb->SetStatus(SeenObjectStatus::ErrorInvalid);
            }
            LogPrint(BCLog::GOBJECT, "SuperblockManager::%s -- superblock status = %d\n", __func__,
                     std23::to_underlying(sb->GetStatus()));
            switch (sb->GetStatus()) {
            case SeenObjectStatus::ErrorInvalid:
            case SeenObjectStatus::Unknown:
                LogPrint(BCLog::GOBJECT, "SuperblockManager::%s -- Unknown or invalid trigger found\n", __func__);
                remove = true;
                break;
            case SeenObjectStatus::Valid:
            case SeenObjectStatus::Executed:
                LogPrint(BCLog::GOBJECT, "SuperblockManager::%s -- Valid trigger found\n", __func__);
                if (sb->IsExpired(cachedHeight)) {
                    if (obj) obj->SetExpired();
                    remove = true;
                }
                break;
            default:
                break;
            }
        }
        LogPrint(BCLog::GOBJECT, "SuperblockManager::%s -- %smarked for removal\n", __func__,
                 remove ? "" : "NOT ");

        if (remove) {
            std::string strDataAsPlainString = "nullptr";
            if (obj) {
                strDataAsPlainString = obj->GetDataAsPlainString();
                obj->PrepareDeletion(GetTime<std::chrono::seconds>().count());
            }
            LogPrint(BCLog::GOBJECT, "SuperblockManager::%s -- Removing trigger object %s\n", __func__,
                     strDataAsPlainString);
            it = m_triggers.erase(it);
        } else {
            ++it;
        }
    }
}

void SuperblockManager::Clear()
{
    LOCK(cs_sb);
    m_triggers.clear();
}

std::vector<CSuperblock_sptr> SuperblockManager::GetActiveTriggers() const
{
    LOCK(cs_sb);
    std::vector<CSuperblock_sptr> vecResults;
    vecResults.reserve(m_triggers.size());
    for (const auto& [_, entry] : m_triggers) {
        if (entry.sb && entry.obj) {
            vecResults.push_back(entry.sb);
        }
    }
    return vecResults;
}

bool SuperblockManager::GetBestSuperblock(const CDeterministicMNList& tip_mn_list,
                                          CSuperblock_sptr& sbRet, int nBlockHeight) const
{
    LOCK(cs_sb);
    return GetBestSuperblockInternal(tip_mn_list, sbRet, nBlockHeight);
}

bool SuperblockManager::GetBestSuperblockInternal(const CDeterministicMNList& tip_mn_list,
                                                  CSuperblock_sptr& sbRet, int nBlockHeight) const
{
    AssertLockHeld(cs_sb);
    if (!CSuperblock::IsValidBlockHeight(nBlockHeight)) {
        return false;
    }

    int nYesCount = 0;
    for (const auto& [_, entry] : m_triggers) {
        if (!entry.sb || !entry.obj || nBlockHeight != entry.sb->GetBlockHeight()) {
            continue;
        }
        int nTempYesCount = entry.obj->GetAbsoluteYesCount(tip_mn_list, VOTE_SIGNAL_FUNDING);
        if (nTempYesCount > nYesCount) {
            nYesCount = nTempYesCount;
            sbRet = entry.sb;
        }
    }
    return nYesCount > 0;
}

bool SuperblockManager::IsSuperblockTriggered(const CDeterministicMNList& tip_mn_list, int nBlockHeight)
{
    LogPrint(BCLog::GOBJECT, "IsSuperblockTriggered -- Start nBlockHeight = %d\n", nBlockHeight);
    if (!CSuperblock::IsValidBlockHeight(nBlockHeight)) {
        return false;
    }

    LOCK(cs_sb);

    LogPrint(BCLog::GOBJECT, "IsSuperblockTriggered -- m_triggers.size() = %d\n", m_triggers.size());

    for (const auto& [_, entry] : m_triggers) {
        if (!entry.sb) {
            LogPrintf("IsSuperblockTriggered -- Non-superblock found, continuing\n");
            continue;
        }
        if (!entry.obj) {
            LogPrintf("IsSuperblockTriggered -- pObj == nullptr, continuing\n");
            continue;
        }

        LogPrint(BCLog::GOBJECT, "IsSuperblockTriggered -- data = %s\n", entry.obj->GetDataAsPlainString());

        if (nBlockHeight != entry.sb->GetBlockHeight()) {
            LogPrint(BCLog::GOBJECT, /* Continued */
                     "IsSuperblockTriggered -- block height doesn't match nBlockHeight = %d, blockStart = %d, "
                     "continuing\n",
                     nBlockHeight, entry.sb->GetBlockHeight());
            continue;
        }

        entry.obj->UpdateSentinelVariables(tip_mn_list);

        if (entry.obj->IsSetCachedFunding()) {
            LogPrint(BCLog::GOBJECT, "IsSuperblockTriggered -- fCacheFunding = true, returning true\n");
            return true;
        } else {
            LogPrint(BCLog::GOBJECT, "IsSuperblockTriggered -- fCacheFunding = false, continuing\n");
        }
    }

    return false;
}

bool SuperblockManager::IsValidSuperblock(const CChain& active_chain, const CDeterministicMNList& tip_mn_list,
                                          const CTransaction& txNew, int nBlockHeight, CAmount blockReward) const
{
    LOCK(cs_sb);
    CSuperblock_sptr pSuperblock;
    if (GetBestSuperblockInternal(tip_mn_list, pSuperblock, nBlockHeight)) {
        return pSuperblock->IsValid(active_chain, txNew, nBlockHeight, blockReward);
    }
    return false;
}

bool SuperblockManager::GetSuperblockPayments(const CDeterministicMNList& tip_mn_list, int nBlockHeight,
                                              std::vector<CTxOut>& voutSuperblockRet) const
{
    LOCK(cs_sb);

    CSuperblock_sptr pSuperblock;
    if (!GetBestSuperblockInternal(tip_mn_list, pSuperblock, nBlockHeight)) {
        LogPrint(BCLog::GOBJECT, "GetSuperblockPayments -- Can't find superblock for height %d\n", nBlockHeight);
        return false;
    }

    voutSuperblockRet.clear();

    // TODO: How many payments can we add before things blow up?
    //       Consider at least following limits:
    //          - max coinbase tx size
    //          - max "budget" available
    for (int i = 0; i < pSuperblock->CountPayments(); i++) {
        CGovernancePayment payment;
        if (pSuperblock->GetPayment(i, payment)) {
            voutSuperblockRet.emplace_back(payment.nAmount, payment.script);

            CTxDestination dest;
            ExtractDestination(payment.script, dest);

            LogPrint(BCLog::GOBJECT, "GetSuperblockPayments -- NEW Superblock: output %d (addr %s, amount %d.%08d)\n",
                     i, EncodeDestination(dest), payment.nAmount / COIN, payment.nAmount % COIN);
        } else {
            LogPrint(BCLog::GOBJECT, "GetSuperblockPayments -- Payment not found\n");
        }
    }

    return true;
}

void SuperblockManager::ExecuteBestSuperblock(const CDeterministicMNList& tip_mn_list, int nBlockHeight)
{
    LOCK(cs_sb);
    CSuperblock_sptr pSuperblock;
    if (GetBestSuperblockInternal(tip_mn_list, pSuperblock, nBlockHeight)) {
        // All checks are done in CSuperblock::IsValid via IsBlockValueValid and IsBlockPayeeValid,
        // tip wouldn't be updated if anything was wrong. Mark this trigger as executed.
        pSuperblock->SetExecuted();
    }
}

} // namespace governance
