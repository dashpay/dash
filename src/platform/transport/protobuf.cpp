// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <platform/transport/protobuf.h>

namespace platform::pb {

void Writer::WriteVarint(uint64_t value)
{
    while (value >= 0x80) {
        m_out.push_back(static_cast<uint8_t>(value) | 0x80);
        value >>= 7;
    }
    m_out.push_back(static_cast<uint8_t>(value));
}

void Writer::WriteTag(uint32_t field, WireType wt)
{
    WriteVarint((static_cast<uint64_t>(field) << 3) | static_cast<uint64_t>(wt));
}

void Writer::Varint(uint32_t field, uint64_t value)
{
    WriteTag(field, WireType::Varint);
    WriteVarint(value);
}

void Writer::Bytes(uint32_t field, Span<const uint8_t> value)
{
    WriteTag(field, WireType::Len);
    WriteVarint(value.size());
    m_out.insert(m_out.end(), value.begin(), value.end());
}

void Writer::Str(uint32_t field, const std::string& value)
{
    WriteTag(field, WireType::Len);
    WriteVarint(value.size());
    m_out.insert(m_out.end(), value.begin(), value.end());
}

} // namespace platform::pb
