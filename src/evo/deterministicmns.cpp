// Copyright (c) 2017 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "deterministicmns.h"
#include "specialtx.h"

#include "validation.h"
#include "validationinterface.h"
#include "chainparams.h"
#include "script/standard.h"
#include "base58.h"
#include "core_io.h"

#include <univalue.h>

static const std::string DB_SPORK15 = "dmn_s15";
static const std::string DB_LIST_SNAPSHOT = "dmn_S";
static const std::string DB_LIST_DIFF = "dmn_D";

CDeterministicMNManager* deterministicMNManager;

std::string CDeterministicMNState::ToString() const
{
    CTxDestination dest;
    std::string payoutAddress = "unknown";
    std::string operatorRewardAddress = "none";
    if (ExtractDestination(scriptPayout, dest)) {
        payoutAddress = CBitcoinAddress(dest).ToString();
    }
    if (ExtractDestination(scriptOperatorPayout, dest)) {
        operatorRewardAddress = CBitcoinAddress(dest).ToString();
    }

    return strprintf("CDeterministicMNState(registeredHeight=%d, lastPaidHeight=%d, PoSePenality=%d, PoSeRevivedHeight=%d, PoSeBanHeight=%d, revocationReason=%d, "
                     "keyIDOwner=%s, keyIDOperator=%s, keyIDVoting=%s, addr=%s, nProtocolVersion=%d, payoutAddress=%s, operatorRewardAddress=%s)",
                     registeredHeight, lastPaidHeight, PoSePenality, PoSeRevivedHeight, PoSeBanHeight, revocationReason,
                     keyIDOwner.ToString(), keyIDOperator.ToString(), keyIDVoting.ToString(), addr.ToStringIPPort(false), nProtocolVersion, payoutAddress, operatorRewardAddress);
}

void CDeterministicMNState::ToJson(UniValue& obj) const
{
    obj.clear();
    obj.setObject();
    obj.push_back(Pair("registeredHeight", registeredHeight));
    obj.push_back(Pair("lastPaidHeight", lastPaidHeight));
    obj.push_back(Pair("PoSePenality", PoSePenality));
    obj.push_back(Pair("PoSeRevivedHeight", PoSeRevivedHeight));
    obj.push_back(Pair("PoSeBanHeight", PoSeBanHeight));
    obj.push_back(Pair("revocationReason", revocationReason));
    obj.push_back(Pair("keyIDOwner", keyIDOwner.ToString()));
    obj.push_back(Pair("keyIDOperator", keyIDOperator.ToString()));
    obj.push_back(Pair("keyIDVoting", keyIDVoting.ToString()));
    obj.push_back(Pair("addr", addr.ToStringIPPort(false)));
    obj.push_back(Pair("nProtocolVersion", nProtocolVersion));

    CTxDestination dest;
    if (ExtractDestination(scriptPayout, dest)) {
        CBitcoinAddress bitcoinAddress(dest);
        obj.push_back(Pair("payoutAddress", bitcoinAddress.ToString()));
    }
    if (ExtractDestination(scriptOperatorPayout, dest)) {
        CBitcoinAddress bitcoinAddress(dest);
        obj.push_back(Pair("operatorRewardAddress", bitcoinAddress.ToString()));
    }
}

std::string CDeterministicMN::ToString() const
{
    return strprintf("CDeterministicMN(proTxHash=%s, nCollateralIndex=%d, operatorReward=%f, state=%s", proTxHash.ToString(), nCollateralIndex, (double)operatorReward / 100, state->ToString());
}

void CDeterministicMN::ToJson(UniValue& obj) const
{
    obj.clear();
    obj.setObject();

    UniValue stateObj;
    state->ToJson(stateObj);

    obj.push_back(Pair("proTxHash", proTxHash.ToString()));
    obj.push_back(Pair("collateralIndex", (int)nCollateralIndex));
    obj.push_back(Pair("operatorReward", (double)operatorReward / 100));
    obj.push_back(Pair("state", stateObj));
}

bool CDeterministicMNList::IsMNValid(const uint256& proTxHash) const
{
    auto p = mnMap.find(proTxHash);
    if (p == nullptr) {
        return false;
    }
    return IsMNValid(*p);
}

bool CDeterministicMNList::IsMNPoSeBanned(const uint256& proTxHash) const
{
    auto p = mnMap.find(proTxHash);
    if (p == nullptr) {
        return false;
    }
    return IsMNPoSeBanned(*p);
}

bool CDeterministicMNList::IsMNValid(const CDeterministicMNCPtr& dmn) const
{
    return !IsMNPoSeBanned(dmn);
}

bool CDeterministicMNList::IsMNPoSeBanned(const CDeterministicMNCPtr& dmn) const
{
    assert(dmn);
    const CDeterministicMNState& state = *dmn->state;
    return state.PoSeBanHeight != -1;
}

CDeterministicMNCPtr CDeterministicMNList::GetMN(const uint256& proTxHash) const
{
    auto p = mnMap.find(proTxHash);
    if (p == nullptr) {
        return nullptr;
    }
    return *p;
}

CDeterministicMNCPtr CDeterministicMNList::GetValidMN(const uint256& proTxHash) const
{
    auto dmn = GetMN(proTxHash);
    if (dmn && !IsMNValid(dmn)) {
        return nullptr;
    }
    return dmn;
}

CDeterministicMNCPtr CDeterministicMNList::GetMNByOperatorKey(const CKeyID& keyID)
{
    for (const auto& p : mnMap) {
        if (p.second->state->keyIDOperator == keyID) {
            return p.second;
        }
    }
    return nullptr;
}

static int CompareByLastPaid_GetHeight(const CDeterministicMN &dmn)
{
    int h = dmn.state->lastPaidHeight;
    if (dmn.state->PoSeRevivedHeight != -1 && dmn.state->PoSeRevivedHeight > h) {
        h = dmn.state->PoSeRevivedHeight;
    } else if (h == 0) {
        h = dmn.state->registeredHeight;
    }
    return h;
}

static bool CompareByLastPaid(const CDeterministicMN &_a, const CDeterministicMN &_b)
{
    int ah = CompareByLastPaid_GetHeight(_a);
    int bh = CompareByLastPaid_GetHeight(_b);
    if (ah == bh) {
        return _a.proTxHash < _b.proTxHash;
    } else {
        return ah < bh;
    }
}
static bool CompareByLastPaid(const CDeterministicMNCPtr &_a, const CDeterministicMNCPtr &_b)
{
    return CompareByLastPaid(*_a, *_b);
}

CDeterministicMNCPtr CDeterministicMNList::GetMNPayee() const
{
    if (mnMap.size() == 0)
        return nullptr;

    CDeterministicMNCPtr best;
    for (const auto& dmn : valid_range()) {
        if (!best || CompareByLastPaid(dmn, best))
            best = dmn;
    }

    return best;
}

std::vector<CDeterministicMNCPtr> CDeterministicMNList::GetProjectedMNPayees(int count) const
{
    std::vector<CDeterministicMNCPtr> result;
    result.reserve(count);

    CDeterministicMNList tmpMNList = *this;
    for (int h = height; h < height + count; h++) {
        tmpMNList.SetHeight(h);

        CDeterministicMNCPtr payee = tmpMNList.GetMNPayee();
        // push the original MN object instead of the one from the temporary list
        result.push_back(GetMN(payee->proTxHash));

        CDeterministicMNStatePtr newState = std::make_shared<CDeterministicMNState>(*payee->state);
        newState->lastPaidHeight = h;
        tmpMNList.UpdateMN(payee->proTxHash, newState);
    }

    return result;
}

CDeterministicMNListDiff CDeterministicMNList::BuildDiff(const CDeterministicMNList& to) const
{
    CDeterministicMNListDiff diffRet;
    diffRet.prevBlockHash = blockHash;
    diffRet.blockHash = to.blockHash;
    diffRet.height = to.height;

    for (const auto& p : to.mnMap) {
        const auto fromPtr = mnMap.find(p.first);
        if (fromPtr == nullptr) {
            diffRet.addedMNs.emplace(p.first, p.second);
        } else if (*p.second->state != *(*fromPtr)->state) {
            diffRet.updatedMNs.emplace(p.first, p.second->state);
        }
    }
    for (const auto& p : mnMap) {
        const auto toPtr = to.mnMap.find(p.first);
        if (toPtr == nullptr) {
            diffRet.removedMns.insert(p.first);
        }
    }

    return diffRet;
}

CDeterministicMNList CDeterministicMNList::ApplyDiff(const CDeterministicMNListDiff& diff) const
{
    assert(diff.prevBlockHash == blockHash && diff.height == height + 1);

    CDeterministicMNList result = *this;
    result.blockHash = diff.blockHash;
    result.height = diff.height;

    for (const auto& hash : diff.removedMns) {
        result.RemoveMN(hash);
    }
    for (const auto& p : diff.addedMNs) {
        result.AddMN(p.second);
    }
    for (const auto& p : diff.updatedMNs) {
        result.UpdateMN(p.first, p.second);
    }

    return result;
}


CDeterministicMNManager::CDeterministicMNManager(CEvoDB& _evoDb) :
    evoDb(_evoDb)
{
}

bool CDeterministicMNManager::ProcessBlock(const CBlock& block, const CBlockIndex* pindexPrev, CValidationState& _state)
{
    int height = pindexPrev->nHeight + 1;

    CDeterministicMNList newList;
    if (!BuildNewListFromBlock(block, pindexPrev, _state, newList)) {
        return false;
    }

    if (newList.GetHeight() == -1) {
        newList.SetHeight(height);
    }

    newList.SetBlockHash(block.GetHash());

    CDeterministicMNList oldList = GetListForBlock(pindexPrev->GetBlockHash());
    CDeterministicMNListDiff diff = oldList.BuildDiff(newList);

    evoDb.Write(std::make_pair(DB_LIST_DIFF, diff.blockHash), diff);
    if ((height % SNAPSHOT_LIST_PERIOD) == 0) {
        evoDb.Write(std::make_pair(DB_LIST_SNAPSHOT, diff.blockHash), newList);
        LogPrintf("CDeterministicMNManager::%s -- Wrote snapshot. height=%d, mapCurMNs.size=%d\n",
                  __func__, height, newList.size());
    }

    UpdateSpork15Value();
    if (height == GetSpork15Value()) {
        LogPrintf("CDeterministicMNManager::%s -- spork15 is active now. height=%d\n", __func__, height);
    }

    CleanupCache(height);

    return true;
}

bool CDeterministicMNManager::UndoBlock(const CBlock& block, const CBlockIndex* pindex)
{
    LOCK(cs);

    int height = pindex->nHeight;

    evoDb.Erase(std::make_pair(DB_LIST_DIFF, block.GetHash()));
    evoDb.Erase(std::make_pair(DB_LIST_SNAPSHOT, block.GetHash()));

    if (height == GetSpork15Value()) {
        LogPrintf("CDeterministicMNManager::%s -- spork15 is not active anymore. height=%d\n", __func__, height);
    }

    return true;
}

void CDeterministicMNManager::UpdatedBlockTip(const CBlockIndex* pindex)
{
    LOCK(cs);

    tipHeight = pindex->nHeight;
    tipBlockHash = pindex->GetBlockHash();
}

bool CDeterministicMNManager::BuildNewListFromBlock(const CBlock& block, const CBlockIndex* pindexPrev, CValidationState& _state, CDeterministicMNList& mnListRet)
{
    LOCK(cs);

    int height = pindexPrev->nHeight + 1;

    CDeterministicMNList oldList = GetListForBlock(pindexPrev->GetBlockHash());
    CDeterministicMNList newList = oldList;
    newList.SetBlockHash(uint256()); // we can't know the final block hash, so better not return a (invalid) block hash
    newList.SetHeight(height);

    auto payee = oldList.GetMNPayee();

    for (const auto& dmn : newList.all_range()) {
    }

    for (int i = 1; i < (int)block.vtx.size(); i++) {
        const CTransaction& tx = *block.vtx[i];

        // check if any existing MN collateral is spent by this transaction
        for (const auto& in : tx.vin) {
            const uint256& proTxHash = in.prevout.hash;
            auto dmn = newList.GetMN(proTxHash);
            if (dmn && dmn->nCollateralIndex == in.prevout.n) {
                newList.RemoveMN(proTxHash);

                LogPrintf("CDeterministicMNManager::%s -- MN %s removed from list because collateral was spent. height=%d, mapCurMNs.size=%d\n",
                          __func__, proTxHash.ToString(), height, newList.size());
            }
        }

        if (tx.nType == TRANSACTION_PROVIDER_REGISTER) {
            CProRegTx proTx;
            if (!GetTxPayload(tx, proTx)) {
                assert(false); // this should have been handled already
            }

            if (newList.HasUniqueProperty(proTx.addr))
                return _state.DoS(100, false, REJECT_CONFLICT, "bad-protx-dup-addr");
            if (newList.HasUniqueProperty(proTx.keyIDOwner) || newList.HasUniqueProperty(proTx.keyIDOperator))
                return _state.DoS(100, false, REJECT_CONFLICT, "bad-protx-dup-key");

            auto dmn = std::make_shared<CDeterministicMN>(tx.GetHash(), proTx);

            CDeterministicMNState dmnState = *dmn->state;
            dmnState.registeredHeight = height;

            if (proTx.addr == CService() || proTx.nProtocolVersion == 0) {
                // start in banned state as we need to wait for a ProUpServTx
                dmnState.PoSeBanHeight = height;
            }

            dmn->state = std::make_shared<CDeterministicMNState>(dmnState);

            newList.AddMN(dmn);

            LogPrintf("CDeterministicMNManager::%s -- MN %s added at height %d: %s\n",
                      __func__, tx.GetHash().ToString(), height, proTx.ToString());
        } else if (tx.nType == TRANSACTION_PROVIDER_UPDATE_SERVICE) {
            CProUpServTx proTx;
            if (!GetTxPayload(tx, proTx)) {
                assert(false); // this should have been handled already
            }

            if (newList.HasUniqueProperty(proTx.addr) && newList.GetUniquePropertyMN(proTx.addr)->proTxHash != proTx.proTxHash)
                return _state.DoS(100, false, REJECT_CONFLICT, "bad-protx-dup-addr");

            CDeterministicMNCPtr dmn = newList.GetMN(proTx.proTxHash);
            if (!dmn) {
                return _state.DoS(100, false, REJECT_INVALID, "bad-protx-hash");
            }
            auto newState = std::make_shared<CDeterministicMNState>(*dmn->state);
            newState->addr = proTx.addr;
            newState->nProtocolVersion = proTx.nProtocolVersion;
            newState->scriptOperatorPayout = proTx.scriptOperatorPayout;

            if (newState->PoSeBanHeight != -1) {
                newState->PoSeBanHeight = -1;
                newState->PoSeRevivedHeight = height;

                LogPrintf("CDeterministicMNManager::%s -- MN %s revived at height %d\n",
                          __func__, proTx.proTxHash.ToString(), height);
            }

            newList.UpdateMN(proTx.proTxHash, newState);

            LogPrintf("CDeterministicMNManager::%s -- MN %s updated at height %d: %s\n",
                      __func__, proTx.proTxHash.ToString(), height, proTx.ToString());
        } else if (tx.nType == TRANSACTION_PROVIDER_UPDATE_REGISTRAR) {
            CProUpRegTx proTx;
            if (!GetTxPayload(tx, proTx)) {
                assert(false); // this should have been handled already
            }

            CDeterministicMNCPtr dmn = newList.GetMN(proTx.proTxHash);
            if (!dmn) {
                return _state.DoS(100, false, REJECT_INVALID, "bad-protx-hash");
            }
            auto newState = std::make_shared<CDeterministicMNState>(*dmn->state);
            if (newState->keyIDOperator != proTx.keyIDOperator) {
                // reset all operator related fields and put MN into PoSe-banned state in case the operator key changes
                newState->ResetOperatorFields();
                newState->BanIfNotBanned(height);
            }
            newState->keyIDOperator = proTx.keyIDOperator;
            newState->keyIDVoting = proTx.keyIDVoting;
            newState->scriptPayout = proTx.scriptPayout;

            newList.UpdateMN(proTx.proTxHash, newState);

            LogPrintf("CDeterministicMNManager::%s -- MN %s updated at height %d: %s\n",
                      __func__, proTx.proTxHash.ToString(), height, proTx.ToString());
        } else if (tx.nType == TRANSACTION_PROVIDER_UPDATE_REVOKE) {
            CProUpRevTx proTx;
            if (!GetTxPayload(tx, proTx)) {
                assert(false); // this should have been handled already
            }

            CDeterministicMNCPtr dmn = newList.GetMN(proTx.proTxHash);
            if (!dmn) {
                return _state.DoS(100, false, REJECT_INVALID, "bad-protx-hash");
            }
            auto newState = std::make_shared<CDeterministicMNState>(*dmn->state);
            newState->ResetOperatorFields();
            newState->BanIfNotBanned(height);
            newState->revocationReason = proTx.reason;

            newList.UpdateMN(proTx.proTxHash, newState);

            LogPrintf("CDeterministicMNManager::%s -- MN %s revoked operator key at height %d: %s\n",
                      __func__, proTx.proTxHash.ToString(), height, proTx.ToString());
        }
    }

    // The payee for the current block was determined by the previous block's list but it might have disappeared in the
    // current block. We still pay that MN one last time however.
    if (payee && newList.HasMN(payee->proTxHash)) {
        auto newState = std::make_shared<CDeterministicMNState>(*newList.GetMN(payee->proTxHash)->state);
        newState->lastPaidHeight = height;
        newList.UpdateMN(payee->proTxHash, newState);
    }

    mnListRet = std::move(newList);

    return true;
}

void CDeterministicMNManager::UpdateSpork15Value()
{
    AssertLockHeld(cs);

    if (!sporkManager.IsSporkSet(SPORK_15_DETERMINISTIC_MNS_ENABLED)) {
        return;
    }

    // only update cached spork15 value when it was not set before. This is needed because spork values are very unreliable when starting the node
    int64_t oldSpork15Value = GetSpork15Value();
    int64_t newSpork15Value = sporkManager.GetSporkValue(SPORK_15_DETERMINISTIC_MNS_ENABLED);
    if (newSpork15Value != oldSpork15Value) {
        evoDb.Write(DB_SPORK15, newSpork15Value);
        LogPrintf("CDeterministicMNManager::%s -- Updated spork15 value to %d\n", __func__, newSpork15Value);
    }
}

int64_t CDeterministicMNManager::GetSpork15Value()
{
    AssertLockHeld(cs);

    int64_t v;
    if (evoDb.Read(DB_SPORK15, v)) {
        return v;
    }
    return sporkManager.GetDefaultSporkValue(SPORK_15_DETERMINISTIC_MNS_ENABLED);
}

CDeterministicMNList CDeterministicMNManager::GetListForBlock(const uint256& blockHash)
{
    LOCK(cs);

    auto it = mnListsCache.find(blockHash);
    if (it != mnListsCache.end()) {
        return it->second;
    }

    CDeterministicMNList snapshot;
    if (evoDb.Read(std::make_pair(DB_LIST_SNAPSHOT, blockHash), snapshot)) {
        mnListsCache.emplace(blockHash, snapshot);
        return std::move(snapshot);
    }

    CDeterministicMNListDiff diff;
    if (!evoDb.Read(std::make_pair(DB_LIST_DIFF, blockHash), diff)) {
        return CDeterministicMNList(blockHash, -1);
    }

    snapshot = GetListForBlock(diff.prevBlockHash);
    if (diff.HasChanges()) {
        snapshot = snapshot.ApplyDiff(diff);
    } else {
        snapshot.SetBlockHash(blockHash);
        snapshot.SetHeight(diff.height);
    }

    mnListsCache.emplace(blockHash, snapshot);
    return std::move(snapshot);
}

CDeterministicMNList CDeterministicMNManager::GetListAtChainTip()
{
    LOCK(cs);
    return GetListForBlock(tipBlockHash);
}

CDeterministicMNCPtr CDeterministicMNManager::GetMN(const uint256& blockHash, const uint256& proTxHash)
{
    auto mnList = GetListForBlock(blockHash);
    return mnList.GetMN(proTxHash);
}

bool CDeterministicMNManager::HasValidMNAtBlock(const uint256& blockHash, const uint256& proTxHash)
{
    auto mnList = GetListForBlock(blockHash);
    return mnList.IsMNValid(proTxHash);
}

bool CDeterministicMNManager::HasValidMNAtChainTip(const uint256& proTxHash)
{
    return GetListAtChainTip().IsMNValid(proTxHash);
}

bool CDeterministicMNManager::IsDeterministicMNsSporkActive(int height)
{
    LOCK(cs);

    if (height == -1) {
        height = tipHeight;
    }

    int64_t spork15Value;
    if (sporkManager.IsSporkSet(SPORK_15_DETERMINISTIC_MNS_ENABLED)) {
        spork15Value = sporkManager.GetSporkValue(SPORK_15_DETERMINISTIC_MNS_ENABLED);
    } else {
        spork15Value = GetSpork15Value();
    }

    if (spork15Value < 0)
        return false;
    return height >= spork15Value;
}

void CDeterministicMNManager::CleanupCache(int height)
{
    AssertLockHeld(cs);

    std::vector<uint256> toDelete;
    for (const auto& p : mnListsCache) {
        if (p.second.GetHeight() + LISTS_CACHE_SIZE < height) {
            toDelete.emplace_back(p.first);
        }
    }
    for (const auto& h : toDelete) {
        mnListsCache.erase(h);
    }
}