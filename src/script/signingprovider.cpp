// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2021 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <script/keyorigin.h>
#include <script/signingprovider.h>
#include <script/standard.h>

#include <util/system.h>

const SigningProvider& DUMMY_SIGNING_PROVIDER = SigningProvider();

template<typename M, typename K, typename V>
bool LookupHelper(const M& map, const K& key, V& value)
{
    auto it = map.find(key);
    if (it != map.end()) {
        value = it->second;
        return true;
    }
    return false;
}

bool HidingSigningProvider::GetCScript(const CScriptID& scriptid, CScript& script) const
{
    return m_provider->GetCScript(scriptid, script);
}

bool HidingSigningProvider::GetPubKey(const CKeyID& keyid, CPubKey& pubkey) const
{
    return m_provider->GetPubKey(keyid, pubkey);
}

bool HidingSigningProvider::GetKey(const CKeyID& keyid, CKey& key) const
{
    if (m_hide_secret) return false;
    return m_provider->GetKey(keyid, key);
}

bool HidingSigningProvider::GetKeyOrigin(const CKeyID& keyid, KeyOriginInfo& info) const
{
    if (m_hide_origin) return false;
    return m_provider->GetKeyOrigin(keyid, info);
}

bool FlatSigningProvider::GetCScript(const CScriptID& scriptid, CScript& script) const { return LookupHelper(scripts, scriptid, script); }
bool FlatSigningProvider::GetPubKey(const CKeyID& keyid, CPubKey& pubkey) const { return LookupHelper(pubkeys, keyid, pubkey); }
bool FlatSigningProvider::GetKeyOrigin(const CKeyID& keyid, KeyOriginInfo& info) const
{
    std::pair<CPubKey, KeyOriginInfo> out;
    bool ret = LookupHelper(origins, keyid, out);
    if (ret) info = std::move(out.second);
    return ret;
}
bool FlatSigningProvider::GetKey(const CKeyID& keyid, CKey& key) const { return LookupHelper(keys, keyid, key); }

FlatSigningProvider& FlatSigningProvider::Merge(FlatSigningProvider&& b)
{
    scripts.merge(b.scripts);
    pubkeys.merge(b.pubkeys);
    keys.merge(b.keys);
    origins.merge(b.origins);
    return *this;
}

bool FillableSigningProvider::GetPubKey(const CKeyID &address, CPubKey &vchPubKeyOut) const
{
    CKey key;
    if (!GetKey(address, key)) {
        return false;
    }
    vchPubKeyOut = key.GetPubKey();
    return true;
}

bool FillableSigningProvider::AddKeyPubKey(const CKey& key, const CPubKey &pubkey)
{
    LOCK(cs_KeyStore);
    mapKeys[pubkey.GetID()] = key;
    return true;
}

bool FillableSigningProvider::HaveKey(const CKeyID &address) const
{
    LOCK(cs_KeyStore);
    return mapKeys.count(address) > 0;
}

std::set<CKeyID> FillableSigningProvider::GetKeys() const
{
    LOCK(cs_KeyStore);
    std::set<CKeyID> set_address;
    for (const auto& mi : mapKeys) {
        set_address.insert(mi.first);
    }
    return set_address;
}

bool FillableSigningProvider::GetKey(const CKeyID &address, CKey &keyOut) const
{
    LOCK(cs_KeyStore);
    KeyMap::const_iterator mi = mapKeys.find(address);
    if (mi != mapKeys.end()) {
        keyOut = mi->second;
        return true;
    }
    return false;
}

bool FillableSigningProvider::AddCScript(const CScript& redeemScript)
{
    if (redeemScript.size() > MAX_SCRIPT_ELEMENT_SIZE)
        return error("FillableSigningProvider::AddCScript(): redeemScripts > %i bytes are invalid", MAX_SCRIPT_ELEMENT_SIZE);

    LOCK(cs_KeyStore);
    mapScripts[CScriptID(redeemScript)] = redeemScript;
    return true;
}

bool FillableSigningProvider::HaveCScript(const CScriptID& hash) const
{
    LOCK(cs_KeyStore);
    return mapScripts.count(hash) > 0;
}

std::set<CScriptID> FillableSigningProvider::GetCScripts() const
{
    LOCK(cs_KeyStore);
    std::set<CScriptID> set_script;
    for (const auto& mi : mapScripts) {
        set_script.insert(mi.first);
    }
    return set_script;
}

bool FillableSigningProvider::GetCScript(const CScriptID &hash, CScript& redeemScriptOut) const
{
    LOCK(cs_KeyStore);
    ScriptMap::const_iterator mi = mapScripts.find(hash);
    if (mi != mapScripts.end())
    {
        redeemScriptOut = (*mi).second;
        return true;
    }
    return false;
}

CKeyID GetKeyForDestination(const SigningProvider& store, const CTxDestination& dest)
{
    // Only supports destinations which map to single public keys, i.e. P2PKH
    const PKHash *pkhash = std::get_if<PKHash>(&dest);

    if (pkhash != nullptr) return ToKeyID(*pkhash);

    return CKeyID();
}
