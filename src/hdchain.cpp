// Copyright (c) 2014-2017 The Dash Core developers
// Distributed under the MIT software license, see the accompanying

#include "chainparams.h"
#include "hdchain.h"
#include "tinyformat.h"

bool CHDChain::SetNull()
{
    nVersion = CHDChain::CURRENT_VERSION;
    nExternalChainCounter = 0;
    vchSeed.clear();
    id = uint256();
    return IsNull();
}

bool CHDChain::IsNull() const
{
    return vchSeed.empty() || id == uint256();
}

bool CHDChain::SetSeed(const std::vector<unsigned char>& vchSeedIn, bool fUpdateID)
{
    vchSeed = vchSeedIn;
    if (fUpdateID)
        id = GetSeedHash();
    return !IsNull();
}

std::vector<unsigned char> CHDChain::GetSeed() const
{
    return vchSeed;
}

uint256 CHDChain::GetSeedHash()
{
    return Hash(vchSeed.begin(), vchSeed.end());
}

void CHDChain::DeriveChildExtKey(uint32_t childIndex, CExtKey& extKeyRet)
{
    // Use BIP44 keypath scheme i.e. m / purpose' / coin_type' / account' / change / address_index
    CExtKey masterKey;              //hd master key
    CExtKey purposeKey;             //key at m/purpose'
    CExtKey cointypeKey;            //key at m/purpose'/coin_type'
    CExtKey accountKey;             //key at m/purpose'/coin_type'/account'
    CExtKey changeKey;              //key at m/purpose'/coin_type'/account'/change
    CExtKey childKey;               //key at m/purpose'/coin_type'/account'/change/address_index

    masterKey.SetMaster(&vchSeed[0], vchSeed.size());

    // Use hardened derivation for purpose, coin_type and account
    // (keys >= 0x80000000 are hardened after bip32)
    // TODO: support multiple accounts, external/internal addresses, and multiple index per each

    // derive m/purpose'
    masterKey.Derive(purposeKey, 44 | 0x80000000);
    // derive m/purpose'/coin_type'
    purposeKey.Derive(cointypeKey, Params().ExtCoinType() | 0x80000000);
    // derive m/purpose'/coin_type'/account'
    cointypeKey.Derive(accountKey, 0x80000000);
    // derive m/purpose'/coin_type'/account/change
    accountKey.Derive(changeKey, 0);
    // derive m/purpose'/coin_type'/account/change/address_index
    changeKey.Derive(extKeyRet, childIndex);
}

std::string CHDPubKey::GetKeyPath() const
{
    return strprintf("m/44'/%d'/%d'/%d/%d", Params().ExtCoinType(), nAccount, nChange, extPubKey.nChild);
}
