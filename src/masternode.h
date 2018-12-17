// Copyright (c) 2014-2017 The Dash Core developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef MASTERNODE_H
#define MASTERNODE_H

#include "key.h"
#include "validation.h"
#include "spork.h"
#include "bls/bls.h"

#include "evo/deterministicmns.h"

class CMasternode;
class CConnman;

static const int MASTERNODE_MAX_MIXING_TXES             = 5;

struct masternode_info_t
{
    // Note: all these constructors can be removed once C++14 is enabled.
    // (in C++11 the member initializers wrongly disqualify this as an aggregate)
    masternode_info_t() = default;
    masternode_info_t(masternode_info_t const&) = default;

    masternode_info_t(int activeState, int protoVer, int64_t sTime) :
        nActiveState{activeState}, nProtocolVersion{protoVer}, sigTime{sTime} {}

    // only called when the network is in legacy MN list mode
    masternode_info_t(int activeState, int protoVer, int64_t sTime,
                      COutPoint const& outpnt, CService const& addr,
                      CPubKey const& pkCollAddr, CPubKey const& pkMN) :
        nActiveState{activeState}, nProtocolVersion{protoVer}, sigTime{sTime},
        outpoint{outpnt}, addr{addr},
        pubKeyCollateralAddress{pkCollAddr}, pubKeyMasternode{pkMN}, keyIDCollateralAddress{pkCollAddr.GetID()}, keyIDOwner{pkMN.GetID()}, legacyKeyIDOperator{pkMN.GetID()}, keyIDVoting{pkMN.GetID()} {}

    // only called when the network is in deterministic MN list mode
    masternode_info_t(int activeState, int protoVer, int64_t sTime,
                      COutPoint const& outpnt, CService const& addr,
                      CKeyID const& pkCollAddr, CKeyID const& pkOwner, CBLSPublicKey const& pkOperator, CKeyID const& pkVoting) :
        nActiveState{activeState}, nProtocolVersion{protoVer}, sigTime{sTime},
        outpoint{outpnt}, addr{addr},
        pubKeyCollateralAddress{}, pubKeyMasternode{}, keyIDCollateralAddress{pkCollAddr}, keyIDOwner{pkOwner}, blsPubKeyOperator{pkOperator}, keyIDVoting{pkVoting} {}

    int nActiveState = 0;
    int nProtocolVersion = 0;
    int64_t sigTime = 0; //mnb message time

    COutPoint outpoint{};
    CService addr{};
    CPubKey pubKeyCollateralAddress{}; // this will be invalid/unset when the network switches to deterministic MNs (luckely it's only important for the broadcast hash)
    CPubKey pubKeyMasternode{}; // this will be invalid/unset when the network switches to deterministic MNs (luckely it's only important for the broadcast hash)
    CKeyID keyIDCollateralAddress{}; // this is only used in compatibility code and won't be used when spork15 gets activated
    CKeyID keyIDOwner{};
    CKeyID legacyKeyIDOperator{};
    CBLSPublicKey blsPubKeyOperator;
    CKeyID keyIDVoting{};

    int64_t nLastDsq = 0; //the dsq count from the last dsq broadcast of this node
    int64_t nTimeLastChecked = 0;
    int64_t nTimeLastPaid = 0;
    bool fInfoValid = false; //* not in CMN
};

//
// The Masternode Class. For managing the Darksend process. It contains the input of the 1000DRK, signature to prove
// it's the one who own that ip address and code for calculating the payment election.
//
class CMasternode : public masternode_info_t
{
private:
    // critical section to protect the inner data structures
    mutable CCriticalSection cs;

public:
    enum state {
        MASTERNODE_ENABLED,
        MASTERNODE_OUTPOINT_SPENT,
        MASTERNODE_POSE_BAN
    };

    enum CollateralStatus {
        COLLATERAL_OK,
        COLLATERAL_UTXO_NOT_FOUND,
        COLLATERAL_INVALID_AMOUNT,
        COLLATERAL_INVALID_PUBKEY,
    };


    std::vector<unsigned char> vchSig{};

    uint256 nCollateralMinConfBlockHash{};
    int nBlockLastPaid{};
    int nPoSeBanScore{};
    int nPoSeBanHeight{};
    int nMixingTxCount{};
    bool fUnitTest = false;

    // KEEP TRACK OF GOVERNANCE ITEMS EACH MASTERNODE HAS VOTE UPON FOR RECALCULATION
    std::map<uint256, int> mapGovernanceObjectsVotedOn;

    CMasternode();
    CMasternode(const CMasternode& other);
    CMasternode(CService addrNew, COutPoint outpointNew, CPubKey pubKeyCollateralAddressNew, CPubKey pubKeyMasternodeNew, int nProtocolVersionIn);
    CMasternode(const uint256 &proTxHash, const CDeterministicMNCPtr& dmn);

    ADD_SERIALIZE_METHODS;

    template <typename Stream, typename Operation>
    inline void SerializationOp(Stream& s, Operation ser_action) {
        LOCK(cs);
        READWRITE(outpoint);
        READWRITE(addr);
        READWRITE(pubKeyCollateralAddress);
        READWRITE(pubKeyMasternode);
        READWRITE(keyIDCollateralAddress);
        READWRITE(keyIDOwner);
        READWRITE(legacyKeyIDOperator);
        READWRITE(blsPubKeyOperator);
        READWRITE(keyIDVoting);
        READWRITE(vchSig);
        READWRITE(sigTime);
        READWRITE(nLastDsq);
        READWRITE(nTimeLastChecked);
        READWRITE(nTimeLastPaid);
        READWRITE(nActiveState);
        READWRITE(nCollateralMinConfBlockHash);
        READWRITE(nBlockLastPaid);
        READWRITE(nProtocolVersion);
        READWRITE(nPoSeBanScore);
        READWRITE(nPoSeBanHeight);
        READWRITE(nMixingTxCount);
        READWRITE(fUnitTest);
        READWRITE(mapGovernanceObjectsVotedOn);
    }

    // CALCULATE A RANK AGAINST OF GIVEN BLOCK
    arith_uint256 CalculateScore(const uint256& blockHash) const;

    static CollateralStatus CheckCollateral(const COutPoint& outpoint, const CKeyID& keyID);
    static CollateralStatus CheckCollateral(const COutPoint& outpoint, const CKeyID& keyID, int& nHeightRet);

    bool IsEnabled() const { return nActiveState == MASTERNODE_ENABLED; }
    bool IsPoSeBanned() const { return nActiveState == MASTERNODE_POSE_BAN; }
    bool IsOutpointSpent() const { return nActiveState == MASTERNODE_OUTPOINT_SPENT; }

    bool IsValidForMixingTxes() const
    {
        return nMixingTxCount <= MASTERNODE_MAX_MIXING_TXES;
    }

    bool IsValidNetAddr();
    static bool IsValidNetAddr(CService addrIn);

    masternode_info_t GetInfo() const;

    static std::string StateToString(int nStateIn);
    std::string GetStateString() const;
    std::string GetStatus() const;

    int GetLastPaidTime() const { return nTimeLastPaid; }
    int GetLastPaidBlock() const { return nBlockLastPaid; }
    void UpdateLastPaid(const CBlockIndex *pindex, int nMaxBlocksToScanBack);

    // KEEP TRACK OF EACH GOVERNANCE ITEM INCASE THIS NODE GOES OFFLINE, SO WE CAN RECALC THEIR STATUS
    void AddGovernanceVote(uint256 nGovernanceObjectHash);
    // RECALCULATE CACHED STATUS FLAGS FOR ALL AFFECTED OBJECTS
    void FlagGovernanceItemsAsDirty();

    void RemoveGovernanceObject(uint256 nGovernanceObjectHash);

    CMasternode& operator=(CMasternode const& from)
    {
        static_cast<masternode_info_t&>(*this)=from;
        vchSig = from.vchSig;
        nCollateralMinConfBlockHash = from.nCollateralMinConfBlockHash;
        nBlockLastPaid = from.nBlockLastPaid;
        nPoSeBanScore = from.nPoSeBanScore;
        nPoSeBanHeight = from.nPoSeBanHeight;
        nMixingTxCount = from.nMixingTxCount;
        fUnitTest = from.fUnitTest;
        mapGovernanceObjectsVotedOn = from.mapGovernanceObjectsVotedOn;
        return *this;
    }
};

inline bool operator==(const CMasternode& a, const CMasternode& b)
{
    return a.outpoint == b.outpoint;
}
inline bool operator!=(const CMasternode& a, const CMasternode& b)
{
    return !(a.outpoint == b.outpoint);
}

#endif
