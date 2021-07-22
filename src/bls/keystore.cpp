// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2015 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <keystore.h>

#include <hash.h>
#include <util/system.h>

bool CBasicKeyStore::GetPubKey(const CKeyID &address, CBLSPublicKey &vchPubKeyOut) const
{
    CBLSSecretKey key;
    if (!GetKey(address, key)) {
        LOCK(cs_KeyStore);
        bKeyMap::const_iterator it = mapBKeys.find(address);
        if (it != mapBKeys.end()) {
            vchPubKeyOut = it->second.GetPublicKey();
            return true;
        }
        return false;
    }
    vchPubKeyOut = key.GetPublicKey();
    return true;
}

bool CBasicKeyStore::AddKeyPubKey(const CBLSSecretKey& key, const CBLSPublicKey &pubkey)
{
    LOCK(cs_KeyStore);
    mapBKeys[CKeyID(Hash160(pubkey.ToByteVector()))] = key;
    return true;
}

bool CBasicKeyStore::GetKey(const CKeyID &address, CBLSSecretKey &keyOut) const
{
    LOCK(cs_KeyStore);
    bKeyMap::const_iterator mi = mapBKeys.find(address);
    if (mi != mapBKeys.end()) {
        keyOut = mi->second;
        return true;
    }
    return false;
}

