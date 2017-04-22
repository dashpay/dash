// Copyright (c) 2014-2017 The Owncoin Core developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef ACTIVEMASTERNODE_H
#define ACTIVEMASTERNODE_H

#include "net.h"
#include "key.h"
#include "wallet/wallet.h"

class CActiveMasternode;

static const int ACTIVE_MASTERNODE_INITIAL          = 0; // initial state
static const int ACTIVE_MASTERNODE_SYNC_IN_PROCESS  = 1;
static const int ACTIVE_MASTERNODE_INPUT_TOO_NEW    = 2;
static const int ACTIVE_MASTERNODE_NOT_CAPABLE      = 3;
static const int ACTIVE_MASTERNODE_STARTED          = 4;

extern CActiveMasternode activeMasternode;

// Responsible for activating the Masternode and pinging the network
class CActiveMasternode
{
public:
    enum masternode_type_enum_t {
        MASTERNODE_UNKNOWN = 0,
        MASTERNODE_REMOTE  = 1,
        MASTERNODE_LOCAL   = 2
    };

private:
    // critical section to protect the inner data structures
    mutable CCriticalSection cs;

    masternode_type_enum_t eType;

<<<<<<< HEAD
    bool fPingerEnabled;
=======
    /// Create Masternode broadcast, needs to be relayed manually after that
    bool CreateBroadcast(CTxIn vin, CService service, CKey key, CPubKey pubKey, CKey keyMasternode, CPubKey pubKeyMasternode, std::string &errorMessage, CMasternodeBroadcast &mnb);
>>>>>>> refs/remotes/dmrtsvetkov/v0.12.0.x

    /// Ping Masternode
    bool SendMasternodePing();

public:
    // Keys for the active Masternode
    CPubKey pubKeyMasternode;
    CKey keyMasternode;

    // Initialized while registering Masternode
    CTxIn vin;
    CService service;

    int nState; // should be one of ACTIVE_MASTERNODE_XXXX
    std::string strNotCapableReason;

    CActiveMasternode()
        : eType(MASTERNODE_UNKNOWN),
          fPingerEnabled(false),
          pubKeyMasternode(),
          keyMasternode(),
          vin(),
          service(),
          nState(ACTIVE_MASTERNODE_INITIAL)
    {}

    /// Manage state of active Masternode
    void ManageState();

<<<<<<< HEAD
    std::string GetStateString() const;
    std::string GetStatus() const;
    std::string GetTypeString() const;
=======
    /// Create Masternode broadcast, needs to be relayed manually after that
    bool CreateBroadcast(std::string strService, std::string strKey, std::string strTxHash, std::string strOutputIndex, std::string& errorMessage, CMasternodeBroadcast &mnb, bool fOffline = false);
>>>>>>> refs/remotes/dmrtsvetkov/v0.12.0.x

private:
    void ManageStateInitial();
    void ManageStateRemote();
    void ManageStateLocal();
};

#endif
