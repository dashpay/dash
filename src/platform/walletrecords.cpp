// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <platform/walletrecords.h>

#include <clientversion.h>
#include <streams.h>

#include <algorithm>

namespace platform {

std::vector<unsigned char> SerializeIdentityRecord(const IdentityRecord& r)
{
    CDataStream s(SER_DISK, CLIENT_VERSION);
    s << r.version << static_cast<uint8_t>(r.state) << r.funding_txid << r.funding_vout
      << r.funding_key_index << r.funding_amount
      << std::vector<uint8_t>(r.identity_id.begin(), r.identity_id.end())
      << r.label << r.normalized_label
      << std::vector<uint8_t>(r.preorder_salt.begin(), r.preorder_salt.end())
      << r.contested << r.last_error << r.started_at;
    const auto span = MakeUCharSpan(s);
    return {span.begin(), span.end()};
}

bool DeserializeIdentityRecord(const std::vector<unsigned char>& data, IdentityRecord& r)
{
    try {
        CDataStream s(data, SER_DISK, CLIENT_VERSION);
        uint8_t state{0};
        std::vector<uint8_t> identity_id, salt;
        s >> r.version;
        if (r.version == 0 || r.version > IdentityRecord::CURRENT_VERSION) return false;
        s >> state >> r.funding_txid >> r.funding_vout >> r.funding_key_index >> r.funding_amount >>
            identity_id >> r.label >> r.normalized_label >> salt >> r.contested >> r.last_error >>
            r.started_at;
        if (identity_id.size() != r.identity_id.size() || salt.size() != r.preorder_salt.size()) return false;
        std::copy(identity_id.begin(), identity_id.end(), r.identity_id.begin());
        std::copy(salt.begin(), salt.end(), r.preorder_salt.begin());
        r.state = static_cast<IdentityRecord::State>(state);
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

std::vector<unsigned char> EncodePaymentCursor(uint32_t next_index)
{
    return {static_cast<unsigned char>(next_index), static_cast<unsigned char>(next_index >> 8),
            static_cast<unsigned char>(next_index >> 16), static_cast<unsigned char>(next_index >> 24)};
}

uint32_t DecodePaymentCursor(const std::vector<unsigned char>& data)
{
    if (data.size() != 4) return 0;
    return uint32_t{data[0]} | (uint32_t{data[1]} << 8) | (uint32_t{data[2]} << 16) |
           (uint32_t{data[3]} << 24);
}

uint32_t ComputePaymentCursor(uint32_t window,
                              const std::function<std::optional<CScript>(uint32_t)>& derive,
                              const std::set<CScript>& wallet_output_scripts)
{
    uint32_t cursor{0};
    for (uint32_t index = 0; index < window; ++index) {
        const auto script{derive(index)};
        if (!script) break;
        if (wallet_output_scripts.count(*script)) cursor = index + 1;
    }
    return cursor;
}

} // namespace platform
