// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_PLATFORM_TRANSPORT_CBOR_H
#define BITCOIN_PLATFORM_TRANSPORT_CBOR_H

#include <cstdint>
#include <span.h>
#include <string>
#include <vector>

/**
 * Minimal CBOR writer — just enough to encode the `where` and `order_by`
 * operands of a DAPI getDocuments v0 request, which are CBOR-encoded byte
 * fields (arrays of [field, operator, value] / [field, direction] tuples).
 * See dashpay/platform packages/dapi getDocuments handler (v0 CBOR path).
 */
namespace platform::transport::cbor {

class Writer
{
public:
    void Array(size_t n) { WriteHead(4, n); }
    void Uint(uint64_t v) { WriteHead(0, v); }
    void Text(const std::string& s)
    {
        WriteHead(3, s.size());
        m_out.insert(m_out.end(), s.begin(), s.end());
    }
    void Bytes(Span<const uint8_t> b)
    {
        WriteHead(2, b.size());
        m_out.insert(m_out.end(), b.begin(), b.end());
    }
    void Bool(bool b) { m_out.push_back(b ? 0xf5 : 0xf4); }

    const std::vector<uint8_t>& data() const { return m_out; }
    std::vector<uint8_t> take() { return std::move(m_out); }

private:
    void WriteHead(uint8_t major, uint64_t value)
    {
        const uint8_t mt = static_cast<uint8_t>(major << 5);
        if (value < 24) {
            m_out.push_back(mt | static_cast<uint8_t>(value));
        } else if (value <= 0xff) {
            m_out.push_back(mt | 24);
            m_out.push_back(static_cast<uint8_t>(value));
        } else if (value <= 0xffff) {
            m_out.push_back(mt | 25);
            m_out.push_back(static_cast<uint8_t>(value >> 8));
            m_out.push_back(static_cast<uint8_t>(value));
        } else if (value <= 0xffffffff) {
            m_out.push_back(mt | 26);
            for (int i = 3; i >= 0; --i) m_out.push_back(static_cast<uint8_t>(value >> (8 * i)));
        } else {
            m_out.push_back(mt | 27);
            for (int i = 7; i >= 0; --i) m_out.push_back(static_cast<uint8_t>(value >> (8 * i)));
        }
    }
    std::vector<uint8_t> m_out;
};

} // namespace platform::transport::cbor

#endif // BITCOIN_PLATFORM_TRANSPORT_CBOR_H
