// Copyright (c) 2014-2017 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
#ifndef DASH_HDCHAIN_H
#define DASH_HDCHAIN_H

#include "key.h"

/* simple HD chain data model */
class CHDChain
{
private:
    std::vector<unsigned char> vchSeed;

public:
    static const int CURRENT_VERSION = 1;
    int nVersion;
    uint256 id;
    uint32_t nExternalChainCounter;

    CHDChain() : nVersion(CHDChain::CURRENT_VERSION), id(uint256()), nExternalChainCounter(0) { SetNull(); }

    ADD_SERIALIZE_METHODS;
    template <typename Stream, typename Operation>
    inline void SerializationOp(Stream& s, Operation ser_action, int nType, int nVersion)
    {
        READWRITE(this->nVersion);
        nVersion = this->nVersion;
        READWRITE(vchSeed);
        READWRITE(id);
        READWRITE(nExternalChainCounter);
    }

    bool SetNull();
    bool IsNull() const;

    bool SetSeed(const std::vector<unsigned char>& vchSeedIn, bool fUpdateID);
    std::vector<unsigned char> GetSeed() const;

    uint256 GetSeedHash();
    void DeriveChildExtKey(uint32_t childIndex, CExtKey& extKeyRet);
};

/* hd pubkey data model */
class CHDPubKey
{
public:
    static const int CURRENT_VERSION = 1;
    int nVersion;
    CExtPubKey extPubKey;
    uint256 hdchainID;
    unsigned int nAccount;
    unsigned int nChange;

    CHDPubKey() : nVersion(CHDPubKey::CURRENT_VERSION), nAccount(0), nChange(0) {}

    ADD_SERIALIZE_METHODS;
    template <typename Stream, typename Operation>
    inline void SerializationOp(Stream& s, Operation ser_action, int nType, int nVersion)
    {
        READWRITE(this->nVersion);
        nVersion = this->nVersion;
        READWRITE(extPubKey);
        READWRITE(hdchainID);
        READWRITE(nAccount);
        READWRITE(nChange);
    }

    std::string GetKeyPath() const;
};

#endif // DASH_HDCHAIN_H
