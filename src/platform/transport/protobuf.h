// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_PLATFORM_TRANSPORT_PROTOBUF_H
#define BITCOIN_PLATFORM_TRANSPORT_PROTOBUF_H

#include <cstdint>
#include <span.h>
#include <string>
#include <vector>

/**
 * Minimal protobuf wire-format writer for the handful of DAPI Platform
 * requests the GUI uses. Field numbers/types come from
 * dashpay/platform packages/dapi-grpc/protos/platform/v0/platform.proto.
 */
namespace platform::pb {

enum class WireType : uint8_t { Varint = 0, I64 = 1, Len = 2, I32 = 5 };

class Writer
{
public:
    void Varint(uint32_t field, uint64_t value);
    void Bytes(uint32_t field, Span<const uint8_t> value);
    void Bytes(uint32_t field, const std::vector<uint8_t>& value) { Bytes(field, Span<const uint8_t>{value}); }
    void Str(uint32_t field, const std::string& value);
    void Bool(uint32_t field, bool value) { if (value) Varint(field, 1); }
    //! Write a nested message (its already-serialized bytes) as a length-
    //! delimited field.
    void Message(uint32_t field, const std::vector<uint8_t>& msg) { Bytes(field, msg); }

    const std::vector<uint8_t>& data() const { return m_out; }
    std::vector<uint8_t> take() { return std::move(m_out); }

private:
    void WriteTag(uint32_t field, WireType wt);
    void WriteVarint(uint64_t value);
    std::vector<uint8_t> m_out;
};

} // namespace platform::pb

#endif // BITCOIN_PLATFORM_TRANSPORT_PROTOBUF_H
