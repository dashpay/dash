// Copyright (c) 2014-2017 The Dash Core developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef PRIVATESENDUTIL_H
#define PRIVATESENDUTIL_H

#include "wallet/wallet.h"

class KeyHolder
{
private:
    CReserveKey reserveKey;
    CPubKey pubKey;
public:
    KeyHolder(CWallet* pwalletIn);
    void KeepKey();
    void ReturnKey();

    CScript GetScriptForDestination() const;

};

class KeyHolderStorage
{
private:
    std::vector<KeyHolder> storage;

public:
    const KeyHolder& AddKey(CWallet* pwalletIn);
    void KeepAll();
    void ReturnAll();

};
#endif //PRIVATESENDUTIL_H
