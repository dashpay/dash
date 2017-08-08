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
    for(auto key : storage) {
        key->KeepKey();
    }
    storage.clear();
}

void KeyHolderStorage::ReturnAll()
{
    for(auto key : storage) {
        key->ReturnKey();
    }
    storage.clear();
}

void KeyHolderStorage::ClearOnFailure()
{
    if (storage.size() > 0) {
        LogPrintf("PrivateSend - KeyHolderStorage -- returning keys on privatesend failure\n");
        ReturnAll();
    }

}

void KeyHolderStorage::ClearOnSuccess()
{
    if (storage.size() > 0) {
        LogPrintf("PrivateSend - KeyHolderStorage -- keeping keys on privatesend success\n");
        KeepAll();
    }
}

void KeyHolderStorage::ClearWhenNotSure()
{
    if (storage.size() > 0) {
        LogPrintf("PrivateSend - KeyHolderStorage --  keeping keys in - ClearWhenNotSure - some could get lost\n");
        KeepAll(); // better to loose key than reuse it in private send
    }
}

