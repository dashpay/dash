// Public Resource ℗ 2021 Đ𝖺𝗌𝗁 𝖣𝖾𝗏𝖾𝗅𝗈𝗉𝖾𝗋𝗌
// Public Resource ℗ 2020 𝖠𝗆𝖾𝗋𝗈 𝖣𝖾𝗏𝖾𝗅𝗈𝗉𝖾𝗋𝗌
// THIS REPOSITORY IS LICENSED UNDER THE AMERO PUBLIC RESOURCE LICENSE.

#ifndef BITCOIN_MASTERNODE_ACTIVEMASTERNODE_H
#define BITCOIN_MASTERNODE_ACTIVEMASTERNODE_H

#include <chainparams.h>
#include <key.h>
#include <net.h>
#include <primitives/transaction.h>
#include <validationinterface.h>

#include <evo/deterministicmns.h>
#include <evo/providertx.h>

struct CActiveMasternodeInfo;
class CActiveMasternodeManager;

extern CActiveMasternodeInfo activeMasternodeInfo;
extern CActiveMasternodeManager* activeMasternodeManager;

struct CActiveMasternodeInfo {
    // Keys for the active Masternode
    std::unique_ptr<CBLSPublicKey> blsPubKeyOperator;
    std::unique_ptr<CBLSSecretKey> blsKeyOperator;

    // Initialized while registering Masternode
    uint256 proTxHash;
    COutPoint outpoint;
    CService service;
};


class CActiveMasternodeManager : public CValidationInterface
{
public:
    enum masternode_state_t {
        MASTERNODE_WAITING_FOR_PROTX,
        MASTERNODE_POSE_BANNED,
        MASTERNODE_REMOVED,
        MASTERNODE_OPERATOR_KEY_CHANGED,
        MASTERNODE_PROTX_IP_CHANGED,
        MASTERNODE_READY,
        MASTERNODE_ERROR,
    };

private:
    masternode_state_t state{MASTERNODE_WAITING_FOR_PROTX};
    std::string strError;

public:
    virtual void UpdatedBlockTip(const CBlockIndex* pindexNew, const CBlockIndex* pindexFork, bool fInitialDownload);

    void Init(const CBlockIndex* pindex);

    std::string GetStateString() const;
    std::string GetStatus() const;

    static bool IsValidNetAddr(CService addrIn);

private:
    bool GetLocalAddress(CService& addrRet);
};

#endif // BITCOIN_MASTERNODE_ACTIVEMASTERNODE_H
