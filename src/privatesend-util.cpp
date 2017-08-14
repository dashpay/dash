// Copyright (c) 2014-2017 The Dash Core developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.
#include "privatesend-util.h"

KeyHolder::KeyHolder(CWallet* pwallet)
        :reserveKey(pwallet)
{
        reserveKey.GetReservedKey(pubKey, false);
}

void KeyHolder::KeepKey()
{
    reserveKey.KeepKey();
}

void KeyHolder::ReturnKey()
{
    reserveKey.ReturnKey();
}

CScript KeyHolder::GetScriptForDestination()
{
    return ::GetScriptForDestination(pubKey.GetID());
}


KeyHolderPtr KeyHolderStorage::AddKey(CWallet* pwallet)
{
    LogPrintf("PrivateSend - KeyHolderStorage -- AddKey\n");
    auto key = std::make_shared<KeyHolder>(pwallet);
    storage.push_back(key);
    return key;
}

void KeyHolderStorage::KeepAll(){
    if (storage.size() > 0) {
        LogPrintf("PrivateSend - KeyHolderStorage -- KeepAll\n");
        for (auto key : storage) {
            key->KeepKey();
        }
        storage.clear();
    }
}

void KeyHolderStorage::ReturnAll()
{
    if (storage.size() > 0) {
        LogPrintf("PrivateSend - KeyHolderStorage -- ReturnAll\n");
        for (auto key : storage) {
            key->ReturnKey();
        }
        storage.clear();
    }
}