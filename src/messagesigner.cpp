// Copyright (c) 2014-2022 The Dash Core developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <key_io.h>
#include <hash.h>
#include <util/message.h> // For MESSAGE_MAGIC
#include <messagesigner.h>
#include <tinyformat.h>
#include <util/strencodings.h>

bool CMessageSigner::GetKeysFromSecret(const std::string& strSecret, CKey& keyRet, CPubKey& pubkeyRet)
{
    keyRet = DecodeSecret(strSecret);
    if (!keyRet.IsValid()) {
        return false;
    }
    pubkeyRet = keyRet.GetPubKey();

    return true;
}

std::optional<std::array<unsigned char, CPubKey::COMPACT_SIGNATURE_SIZE>> CMessageSigner::SignMessage(const std::string& strMessage, const CKey& key)
{
    CHashWriter ss(SER_GETHASH, 0);
    ss << MESSAGE_MAGIC;
    ss << strMessage;

    return CHashSigner::SignHash(ss.GetHash(), key);
}

bool CMessageSigner::VerifyMessage(const CPubKey& pubkey, Span<unsigned char> vchSig, const std::string& strMessage, std::string& strErrorRet)
{
    return VerifyMessage(pubkey.GetID(), vchSig, strMessage, strErrorRet);
}

bool CMessageSigner::VerifyMessage(const CKeyID& keyID, Span<unsigned char> vchSig, const std::string& strMessage, std::string& strErrorRet)
{
    CHashWriter ss(SER_GETHASH, 0);
    ss << MESSAGE_MAGIC;
    ss << strMessage;

    return CHashSigner::VerifyHash(ss.GetHash(), keyID, vchSig, strErrorRet);
}

std::optional<std::array<unsigned char, CPubKey::COMPACT_SIGNATURE_SIZE>> CHashSigner::SignHash(const uint256& hash, const CKey& key)
{
    return key.SignCompact(hash);
}

bool CHashSigner::VerifyHash(const uint256& hash, const CPubKey& pubkey, Span<unsigned char> vchSig, std::string& strErrorRet)
{
    return VerifyHash(hash, pubkey.GetID(), vchSig, strErrorRet);
}

bool CHashSigner::VerifyHash(const uint256& hash, const CKeyID& keyID, Span<unsigned char> vchSig, std::string& strErrorRet)
{
    CPubKey pubkeyFromSig;
    if(!pubkeyFromSig.RecoverCompact(hash, vchSig)) {
        strErrorRet = "Error recovering public key.";
        return false;
    }

    if(pubkeyFromSig.GetID() != keyID) {
        strErrorRet = strprintf("Keys don't match: pubkey=%s, pubkeyFromSig=%s, hash=%s, vchSig=%s",
                    keyID.ToString(), pubkeyFromSig.GetID().ToString(), hash.ToString(),
                    EncodeBase64(vchSig));
        return false;
    }

    return true;
}
