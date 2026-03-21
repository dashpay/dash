// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <index/addressindex_util.h>

#include <hash.h>
#include <script/script.h>

#include <vector>

bool AddressBytesFromScript(const CScript& script, AddressType& address_type, uint160& address_bytes)
{
    if (script.IsPayToScriptHash()) {
        address_type = AddressType::P2SH;
        address_bytes = uint160(std::vector<uint8_t>(script.begin() + 2, script.begin() + 22));
    } else if (script.IsPayToPublicKeyHash()) {
        address_type = AddressType::P2PK_OR_P2PKH;
        address_bytes = uint160(std::vector<uint8_t>(script.begin() + 3, script.begin() + 23));
    } else if (script.IsPayToPublicKey()) {
        address_type = AddressType::P2PK_OR_P2PKH;
        address_bytes = Hash160(std::vector<uint8_t>(script.begin() + 1, script.end() - 1));
    } else {
        address_type = AddressType::UNKNOWN;
        address_bytes.SetNull();
        return false;
    }
    return true;
}
