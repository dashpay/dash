// Copyright (c) 2014-2023 The Dash Core developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <key_io.h>
#include <hash.h>
#include <util/message.h> // For MESSAGE_MAGIC
#include <messagesigner.h>
#include <tinyformat.h>
#include <util/strencodings.h>

#include <cstring>

bool CMessageSigner::GetKeysFromSecret(const std::string& strSecret, CKey& keyRet, CPubKey& pubkeyRet)
{
    keyRet = DecodeSecret(strSecret);
    if (!keyRet.IsValid()) {
        return false;
    }
    pubkeyRet = keyRet.GetPubKey();

    return true;
}

bool CMessageSigner::SignMessage(const std::string& strMessage, std::vector<unsigned char>& vchSigRet, const CKey& key)
{
    CHashWriter ss(SER_GETHASH, 0);
    ss << MESSAGE_MAGIC;
    ss << strMessage;

    return CHashSigner::SignHash(ss.GetHash(), key, vchSigRet);
}

bool CMessageSigner::VerifyMessage(const CPubKey& pubkey, const std::vector<unsigned char>& vchSig, const std::string& strMessage, std::string& strErrorRet)
{
    return VerifyMessage(pubkey.GetID(), vchSig, strMessage, strErrorRet);
}

bool CMessageSigner::VerifyMessage(const CKeyID& keyID, const std::vector<unsigned char>& vchSig, const std::string& strMessage, std::string& strErrorRet)
{
    CHashWriter ss(SER_GETHASH, 0);
    ss << MESSAGE_MAGIC;
    ss << strMessage;

    return CHashSigner::VerifyHash(ss.GetHash(), keyID, vchSig, strErrorRet);
}

bool CHashSigner::SignHash(const uint256& hash, const CKey& key, std::vector<unsigned char>& vchSigRet)
{
    return key.SignCompact(hash, vchSigRet);
}

bool CHashSigner::VerifyHash(const uint256& hash, const CPubKey& pubkey, const std::vector<unsigned char>& vchSig, std::string& strErrorRet)
{
    return VerifyHash(hash, pubkey.GetID(), vchSig, strErrorRet);
}

bool CHashSigner::VerifyHashCanonical(const uint256& hash, const CKeyID& keyID, const std::vector<unsigned char>& vchSig, std::string& strErrorRet)
{
    if (vchSig.size() != CPubKey::COMPACT_SIGNATURE_SIZE) {
        strErrorRet = "Signature is not 65 bytes.";
        return false;
    }
    // The header byte must be canonical. CPubKey::RecoverCompact only masks (header - 27) with 3
    // (recovery id) and 4 (compression flag) and never bounds-checks it, so header bytes differing
    // in any higher bit (e.g. 31 and 39) recover the same key. Restricting the header to the eight
    // values SignCompact can produce (27..34) removes those aliases, so together with low-S all 65
    // bytes are pinned and the signature is non-malleable by third parties.
    if (vchSig[0] < 27 || vchSig[0] > 34) {
        strErrorRet = "Signature has a non-canonical recovery header.";
        return false;
    }
    // A compact recoverable signature is <header byte> <32-byte R> <32-byte S>, with R and S
    // big-endian. Enforce low-S: S must not exceed half the secp256k1 group order. Note that
    // CKey::SignCompact always produces low-S signatures, so this only rejects third-party
    // malleated signatures.
    static constexpr unsigned char SECP256K1_HALF_ORDER[32] = {
        0x7f, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
        0x5d, 0x57, 0x6e, 0x73, 0x57, 0xa4, 0x50, 0x1d,
        0xdf, 0xe9, 0x2f, 0x46, 0x68, 0x1b, 0x20, 0xa0,
    };
    if (std::memcmp(vchSig.data() + 33, SECP256K1_HALF_ORDER, 32) > 0) {
        strErrorRet = "Signature is not low-S.";
        return false;
    }
    return VerifyHash(hash, keyID, vchSig, strErrorRet);
}

bool CHashSigner::VerifyHash(const uint256& hash, const CKeyID& keyID, const std::vector<unsigned char>& vchSig, std::string& strErrorRet)
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
