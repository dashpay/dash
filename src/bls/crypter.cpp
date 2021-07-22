// Copyright (c) 2009-2015 The Bitcoin Core developers
// Copyright (c) 2021 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <wallet/crypter.h>

#include <crypto/aes.h>
#include <crypto/sha512.h>
#include <script/script.h>
#include <script/standard.h>
#include <util/system.h>

#include <string>
#include <vector>

bool EncryptSecret(const CKeyingMaterial& vMasterKey, const CKeyingMaterial &vchPlaintext, const uint256& nIV, std::vector<unsigned char> &vchCiphertext);
bool DecryptSecret(const CKeyingMaterial& vMasterKey, const std::vector<unsigned char>& vchCiphertext, const uint256& nIV, CKeyingMaterial& vchPlaintext);

static bool DecryptKey(const CKeyingMaterial& vMasterKey, const std::vector<unsigned char>& vchCryptedSecret, const CBLSPublicKey& vchPubKey, CBLSSecretKey& key)
{
    CKeyingMaterial vchSecret;
    if(!DecryptSecret(vMasterKey, vchCryptedSecret, vchPubKey.GetHash(), vchSecret))
        return false;

    if (vchSecret.size() != 32)
        return false;

    key.SetByteVector(std::vector<uint8_t>(vchSecret.begin(), vchSecret.end()));
    return true;
}

bool CCryptoKeyStore::AddKeyPubKey(const CBLSSecretKey& key, const CBLSPublicKey &pubkey)
{
    LOCK(cs_KeyStore);
    if (!IsCrypted()) {
        return CBasicKeyStore::AddKeyPubKey(key, pubkey);
    }

    if (IsLocked(true)) {
        return false;
    }

    std::vector<unsigned char> vchCryptedSecret;
    CKeyingMaterial vchSecret(key.ToByteVector().begin(), key.ToByteVector().end());
    if (!EncryptSecret(vMasterKey, vchSecret, pubkey.GetHash(), vchCryptedSecret)) {
        return false;
    }

    if (!AddCryptedKey(pubkey, vchCryptedSecret)) {
        return false;
    }
    return true;
}

bool CCryptoKeyStore::AddCryptedKey(const CBLSPublicKey &vchPubKey, const std::vector<unsigned char> &vchCryptedSecret)
{
    LOCK(cs_KeyStore);
    if (!SetCrypted()) {
        return false;
    }

    mapBCryptedKeys[CKeyID(Hash160(vchPubKey.ToByteVector()))] = make_pair(vchPubKey, vchCryptedSecret);
    return true;
}

bool CCryptoKeyStore::GetKey(const CKeyID &address, CBLSSecretKey& keyOut) const
{
    LOCK(cs_KeyStore);
    if (!IsCrypted()) {
        return CBasicKeyStore::GetKey(address, keyOut);
    }

    bCryptedKeyMap::const_iterator mi = mapBCryptedKeys.find(address);
    if (mi != mapBCryptedKeys.end())
    {
        const CBLSPublicKey &vchPubKey = (*mi).second.first;
        const std::vector<unsigned char> &vchCryptedSecret = (*mi).second.second;
        return DecryptKey(vMasterKey, vchCryptedSecret, vchPubKey, keyOut);
    }
    return false;
}

bool CCryptoKeyStore::GetPubKey(const CKeyID &address, CBLSPublicKey& vchPubKeyOut) const
{
    LOCK(cs_KeyStore);
    if (!IsCrypted())
        return CBasicKeyStore::GetPubKey(address, vchPubKeyOut);

    bCryptedKeyMap::const_iterator mi = mapBCryptedKeys.find(address);
    if (mi != mapBCryptedKeys.end())
    {
        vchPubKeyOut = (*mi).second.first;
        return true;
    }
    // Check for watch-only pubkeys
    return CBasicKeyStore::GetPubKey(address, vchPubKeyOut);
}
