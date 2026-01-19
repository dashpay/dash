// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2015 The Bitcoin Core developers
// Copyright (c) 2016 BitPay Inc.
// Copyright (c) 2023-2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_INDEX_SPENTINDEX_TYPES_H
#define BITCOIN_INDEX_SPENTINDEX_TYPES_H

#include <consensus/amount.h>
#include <addressindex.h>
#include <serialize.h>
#include <uint256.h>

#include <map>
#include <tuple>

struct CSpentIndexKey {
public:
    uint256 m_tx_hash;
    uint32_t m_tx_index{0};

public:
    CSpentIndexKey() { SetNull(); }

    CSpentIndexKey(uint256 txout_hash, uint32_t txout_index) :
        m_tx_hash{txout_hash},
        m_tx_index{txout_index} {};

    void SetNull()
    {
        m_tx_hash.SetNull();
        m_tx_index = 0;
    }

public:
    SERIALIZE_METHODS(CSpentIndexKey, obj) { READWRITE(obj.m_tx_hash, obj.m_tx_index); }
};

struct CSpentIndexValue {
public:
    uint256 m_tx_hash;
    uint32_t m_tx_index{0};
    int32_t m_block_height{0};
    CAmount m_amount{0};
    AddressType m_address_type{AddressType::UNKNOWN};
    uint160 m_address_bytes;

public:
    CSpentIndexValue() { SetNull(); }

    CSpentIndexValue(uint256 txin_hash, uint32_t txin_index, int32_t block_height, CAmount amount,
                     AddressType address_type, uint160 address_bytes) :
        m_tx_hash{txin_hash},
        m_tx_index{txin_index},
        m_block_height{block_height},
        m_amount{amount},
        m_address_type{address_type},
        m_address_bytes{address_bytes} {};

    void SetNull()
    {
        m_tx_hash.SetNull();
        m_tx_index = 0;
        m_block_height = 0;
        m_amount = 0;
        m_address_type = AddressType::UNKNOWN;
        m_address_bytes.SetNull();
    }

    bool IsNull() const { return m_tx_hash.IsNull(); }

public:
    SERIALIZE_METHODS(CSpentIndexValue, obj)
    {
        READWRITE(obj.m_tx_hash, obj.m_tx_index, obj.m_block_height, obj.m_amount, obj.m_address_type, obj.m_address_bytes);
    }
};

struct CSpentIndexKeyCompare {
    bool operator()(const CSpentIndexKey& a, const CSpentIndexKey& b) const
    {
        auto to_tuple = [](const CSpentIndexKey& obj) { return std::tie(obj.m_tx_hash, obj.m_tx_index); };
        return to_tuple(a) < to_tuple(b);
    }
};

using CSpentIndexEntry = std::pair<CSpentIndexKey, CSpentIndexValue>;

#endif // BITCOIN_INDEX_SPENTINDEX_TYPES_H
