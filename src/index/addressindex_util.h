// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_INDEX_ADDRESSINDEX_UTIL_H
#define BITCOIN_INDEX_ADDRESSINDEX_UTIL_H

#include <index/addressindex_types.h>

bool AddressBytesFromScript(const CScript& script, AddressType& address_type, uint160& address_bytes);

#endif // BITCOIN_INDEX_ADDRESSINDEX_UTIL_H
