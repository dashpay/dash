// Copyright (c) 2017 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef DASH_DETERMINISTICMNS_H
#define DASH_DETERMINISTICMNS_H

#include "evodb.h"
#include "providertx.h"
#include "dbwrapper.h"
#include "sync.h"
#include "spork.h"

#include <map>

#include <boost/range/adaptors.hpp>
#include <boost/range/any_range.hpp>

class CBlock;
class CBlockIndex;
class CValidationState;

class CDeterministicMNState
{
public:
    int registeredHeight{-1};
    int lastPaidHeight{0};
    int PoSePenality{0};
    int PoSeRevivedHeight{-1};
    int PoSeBanHeight{-1};
    uint16_t revocationReason{CProUpRevTx::REASON_NOT_SPECIFIED};

    CKeyID keyIDOwner;
    CKeyID keyIDOperator;
    CKeyID keyIDVoting;
    CService addr;
    int32_t nProtocolVersion;
    CScript scriptPayout;
    CScript scriptOperatorPayout;

public:
    CDeterministicMNState() {}
    CDeterministicMNState(const CProRegTx& proTx)
    {
        keyIDOwner = proTx.keyIDOwner;
        keyIDOperator = proTx.keyIDOperator;
        keyIDVoting = proTx.keyIDVoting;
        addr = proTx.addr;
        nProtocolVersion = proTx.nProtocolVersion;
        scriptPayout = proTx.scriptPayout;
    }
    template<typename Stream>
    CDeterministicMNState(deserialize_type, Stream& s) { s >> *this;}

    ADD_SERIALIZE_METHODS;

    template <typename Stream, typename Operation>
    inline void SerializationOp(Stream& s, Operation ser_action)
    {
        READWRITE(registeredHeight);
        READWRITE(lastPaidHeight);
        READWRITE(PoSePenality);
        READWRITE(PoSeRevivedHeight);
        READWRITE(PoSeBanHeight);
        READWRITE(revocationReason);
        READWRITE(keyIDOwner);
        READWRITE(keyIDOperator);
        READWRITE(keyIDVoting);
        READWRITE(addr);
        READWRITE(nProtocolVersion);
        READWRITE(*(CScriptBase*)(&scriptPayout));
        READWRITE(*(CScriptBase*)(&scriptOperatorPayout));
    }

    void ResetOperatorFields()
    {
        keyIDOperator.SetNull();
        addr = CService();
        nProtocolVersion = 0;
        scriptOperatorPayout = CScript();
        revocationReason = CProUpRevTx::REASON_NOT_SPECIFIED;
    }
    void BanIfNotBanned(int height)
    {
        if (PoSeBanHeight == -1) {
            PoSeBanHeight = height;
        }
    }

    bool operator==(const CDeterministicMNState& rhs) const
    {
        return registeredHeight == rhs.registeredHeight &&
               lastPaidHeight == rhs.lastPaidHeight &&
               PoSePenality == rhs.PoSePenality &&
               PoSeRevivedHeight == rhs.PoSeRevivedHeight &&
               PoSeBanHeight == rhs.PoSeBanHeight &&
               keyIDOwner == rhs.keyIDOwner &&
               keyIDOperator == rhs.keyIDOperator &&
               keyIDVoting == rhs.keyIDVoting &&
               addr == rhs.addr &&
               nProtocolVersion == rhs.nProtocolVersion &&
               scriptPayout == rhs.scriptPayout &&
               scriptOperatorPayout == rhs.scriptOperatorPayout;
    }

    bool operator!=(const CDeterministicMNState& rhs) const
    {
        return !(rhs == *this);
    }

public:
    std::string ToString() const;
    void ToJson(UniValue& obj) const;
};
typedef std::shared_ptr<CDeterministicMNState> CDeterministicMNStatePtr;
typedef std::shared_ptr<const CDeterministicMNState> CDeterministicMNStateCPtr;

class CDeterministicMN
{
public:
    CDeterministicMN() {}
    CDeterministicMN(const uint256& _proTxHash, const CProRegTx& _proTx)
    {
        proTxHash = _proTxHash;
        nCollateralIndex = _proTx.nCollateralIndex;
        operatorReward = _proTx.operatorReward;
        state = std::make_shared<CDeterministicMNState>(_proTx);
    }
    template<typename Stream>
    CDeterministicMN(deserialize_type, Stream& s) { s >> *this;}

    uint256 proTxHash;
    uint32_t nCollateralIndex;
    uint16_t operatorReward;
    CDeterministicMNStateCPtr state;

public:
    ADD_SERIALIZE_METHODS;

    template <typename Stream, typename Operation>
    inline void SerializationOp(Stream& s, Operation ser_action)
    {
        READWRITE(proTxHash);
        READWRITE(nCollateralIndex);
        READWRITE(operatorReward);
        READWRITE(state);
    }

public:
    std::string ToString() const;
    void ToJson(UniValue& obj) const;
};
typedef std::shared_ptr<CDeterministicMN> CDeterministicMNPtr;
typedef std::shared_ptr<const CDeterministicMN> CDeterministicMNCPtr;

class CDeterministicMNListDiff;

class CDeterministicMNList
{
public:
    typedef std::map<uint256, CDeterministicMNCPtr> MnMap;
    typedef std::shared_ptr<MnMap> MnMapPtr;

private:
    int height{-1};
    MnMapPtr mnMap;

public:
    CDeterministicMNList() : mnMap(std::make_shared<MnMap>()) {}
    explicit CDeterministicMNList(int _height, MnMapPtr _mnMap = std::make_shared<MnMap>()) :
            height(_height),
            mnMap(_mnMap)
    {}

    CDeterministicMNList Clone() const
    {
        MnMapPtr newMnMap = std::make_shared<MnMap>(mnMap->begin(), mnMap->end());
        return CDeterministicMNList(height, newMnMap);
    }

    ADD_SERIALIZE_METHODS;

    template <typename Stream, typename Operation>
    inline void SerializationOp(Stream& s, Operation ser_action)
    {
        READWRITE(height);
        READWRITE(*mnMap);
    }

public:

    size_t size() const
    {
        return mnMap->size();
    }

    typedef boost::any_range<const CDeterministicMNCPtr&, boost::forward_traversal_tag> range_type;

    range_type all_range() const
    {
        return boost::adaptors::transform(*mnMap, [] (const MnMap::value_type& p) -> const CDeterministicMNCPtr& {
            return p.second;
        });
    }

    range_type valid_range() const
    {
        return boost::adaptors::filter(all_range(), [&] (const CDeterministicMNCPtr& dmn) -> bool {
            return IsMNValid(dmn);
        });
    }

    size_t all_count() const
    {
        return mnMap->size();
    }

    size_t valid_count() const
    {
        size_t c = 0;
        for (const auto& p : *mnMap) {
            if (IsMNValid(p.second)) {
                c++;
            }
        }
        return c;
    }

public:
    int GetHeight() const
    {
        return height;
    }
    void SetHeight(int _height)
    {
        height = _height;
    }
    MnMapPtr GetMap()
    {
        return mnMap;
    }

    bool IsMNValid(const uint256& proTxHash) const;
    bool IsMNPoSeBanned(const uint256& proTxHash) const;

    bool HasMN(const uint256& proTxHash) const {
        return GetMN(proTxHash) != nullptr;
    }
    CDeterministicMNCPtr GetMN(const uint256& proTxHash) const;
    CDeterministicMNCPtr GetValidMN(const uint256& proTxHash) const;
    CDeterministicMNCPtr GetMNByOperatorKey(const CKeyID& keyID);
    CDeterministicMNCPtr GetMNPayee() const;

    /**
     * Calculates the projected MN payees for the next *count* blocks. The result is not guaranteed to be correct
     * as PoSe banning might occur later
     * @param count
     * @return
     */
    std::vector<CDeterministicMNCPtr> GetProjectedMNPayees(int count) const;

    CDeterministicMNListDiff BuildDiff(const CDeterministicMNList& to) const;
    CDeterministicMNList ApplyDiff(const CDeterministicMNListDiff& diff) const;

    void AddMN(const CDeterministicMNCPtr &dmn)
    {
        mnMap->emplace(dmn->proTxHash, dmn);
    }
    void UpdateMN(const uint256 &proTxHash, const CDeterministicMNStateCPtr &state)
    {
        auto it = mnMap->find(proTxHash);
        assert(it != mnMap->end());
        auto dmn = std::make_shared<CDeterministicMN>(*it->second);
        dmn->state = state;
        it->second = dmn;
    }
    void RemoveMN(const uint256& proTxHash)
    {
        mnMap->erase(proTxHash);
    }

private:
    bool IsMNValid(const CDeterministicMNCPtr& dmn) const;
    bool IsMNPoSeBanned(const CDeterministicMNCPtr& dmn) const;
};

class CDeterministicMNListDiff
{
public:
    int height;
    std::map<uint256, CDeterministicMNCPtr> addedMNs;
    std::map<uint256, CDeterministicMNStateCPtr> updatedMNs;
    std::set<uint256> removedMns;

public:
    ADD_SERIALIZE_METHODS;

    template <typename Stream, typename Operation>
    inline void SerializationOp(Stream& s, Operation ser_action)
    {
        READWRITE(height);
        READWRITE(addedMNs);
        READWRITE(updatedMNs);
        READWRITE(removedMns);
    }

public:
    bool HasChanges() const
    {
        return !addedMNs.empty() || !updatedMNs.empty() || !removedMns.empty();
    }
};

class CDeterministicMNManagerState
{
public:
    int firstMNHeight{-1};
    int curHeight{0};
    uint256 curBlockHash;
    int64_t spork15Value{SPORK_15_DETERMINISTIC_MNS_DEFAULT};

    ADD_SERIALIZE_METHODS;

    template <typename Stream, typename Operation>
    inline void SerializationOp(Stream& s, Operation ser_action)
    {
        READWRITE(firstMNHeight);
        READWRITE(curHeight);
        READWRITE(curBlockHash);
        READWRITE(spork15Value);
    }
};

class CDeterministicMNManager
{
    static const int SNAPSHOT_LIST_PERIOD = 576; // once per day
    static const int LISTS_CACHE_SIZE = 576;

public:
    CCriticalSection cs;

private:
    CEvoDB& evoDb;

    CDeterministicMNManagerState state;
    CDeterministicMNManagerState stateBackup; // for rollback

    // stores a list per block.
    // Mutliple consecutive entries might internally point to the same list in case nothing changed in a block
    std::map<int, CDeterministicMNList> lists;
    std::map<int, CDeterministicMNList> listsBackup; // for rollback

    std::map<uint256, std::weak_ptr<const CProRegTx>> proTxCache;

public:
    CDeterministicMNManager(size_t nCacheSize, bool fMemory=false, bool fWipe=false);

    std::unique_ptr<CScopedDBTransaction> BeginTransaction()
    {
        LOCK(cs);
        stateBackup = state;
        listsBackup = lists;
        auto t = CScopedDBTransaction::Begin(dbTransaction);
        t->SetRollbackHandler([&] {
            state = stateBackup;
            lists.swap(listsBackup);
            listsBackup.clear();
        });
        t->SetCommitHandler([&] {
            listsBackup.clear();
        });
        return t;
    }

    void Init();

    bool ProcessBlock(const CBlock& block, const CBlockIndex* pindexPrev, CValidationState& state);
    bool UndoBlock(const CBlock& block, const CBlockIndex* pindex);

    CDeterministicMNList GetListAtHeight(int height);
    CDeterministicMNList GetListAtChainTip();

    CDeterministicMNCPtr GetMN(int height, const uint256& proTxHash);
    bool HasValidMNAtHeight(int height, const uint256& proTxHash);
    bool HasValidMNAtChainTip(const uint256& proTxHash);

    bool IsDeterministicMNsSporkActive(int height = -1);

private:
    void UpdateSpork15Value();
    void RebuildLists(int startHeight, int endHeight);
};

extern CDeterministicMNManager* deterministicMNManager;

#endif//DASH_DETERMINISTICMNS_H
