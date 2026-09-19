// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_BLS_BLS_LEGACY_H
#define BITCOIN_BLS_BLS_LEGACY_H

#include <span.h>
#include <uint256.h>

#include <blst.h>

#include <cstdint>

/**
 * Point codec and hash-to-curve of the legacy (pre-v19) BLS scheme.
 *
 * Both reproduce, bit for bit, what the relic-based implementation did before
 * the v19 hard fork, including its treatment of malformed input: relic was
 * built without its CHECK option, so decoding never failed but left degenerate
 * coordinates behind (an out-of-range coordinate reads as zero, a missing
 * square root leaves y at zero or at a raw marker value). Every such point is
 * consensus-visible through re-serialization of on-chain data, so all of it
 * is kept.
 */
namespace bls_legacy {

/**
 * Deserializes a G1 point from its 48-byte legacy encoding. Returns false only
 * for a non-canonical infinity encoding (top two bits set with anything but
 * 0xc0 00..00). Any other input yields a point, possibly off the curve and
 * outside the subgroup; the caller decides whether that is acceptable.
 */
bool DecodeG1(blst_p1& out, Span<const uint8_t> in);

/**
 * Deserializes a G2 point from its 96-byte legacy encoding (x.c0 || x.c1).
 * Returns false for any input with the top two bits set: the legacy G2 codec
 * rejected the infinity encoding along with everything else in that range.
 */
bool DecodeG2(blst_p2& out, Span<const uint8_t> in);

/** Serializes a G1 point: 0xc0 00..00 for infinity, else x with bit 0x80 set iff y > (p-1)/2. */
void EncodeG1(Span<uint8_t> out, const blst_p1& p);

/** Serializes a G2 point: 0xc0 00..00 for infinity, else x.c0 || x.c1 with bit 0x80 set iff y.c1 > (p-1)/2. */
void EncodeG2(Span<uint8_t> out, const blst_p2& p);

/**
 * Legacy hash-to-G2 of a 32-byte message hash: two Fouque-Tibouchi
 * (Shallue-van de Woestijne) encodings of SHA-256 derived field elements,
 * added and multiplied by the effective cofactor.
 */
void HashToG2(blst_p2& out, const uint256& hash);

} // namespace bls_legacy

#endif // BITCOIN_BLS_BLS_LEGACY_H
