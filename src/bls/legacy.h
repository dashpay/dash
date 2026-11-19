// Copyright (c) 2021 The Dash Core developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef DASH_CRYPTO_BLS_LEGACY_H
#define DASH_CRYPTO_BLS_LEGACY_H

#include <blst.h>

#include <cstddef>
#include <cstdint>

namespace bls {
/**
 * Maps a byte array to a point in an elliptic curve over a quadratic extension.
 *
 * Called ep2_map() in old relic version. Now reimplemented on blst and must stay
 * bit-for-bit compatible: pre-v19 (legacy scheme) consensus depends on it.
 *
 * @param[out] p    - the result.
 * @param[in] msg   - the byte array to map (must be 32 bytes).
 * @param[in] len   - the array length in bytes (must be 32).
 */
void ep2_map_legacy(blst_p2 *p, const uint8_t *msg, size_t len);

/**
 * The encodings of points from before v19: 48 bytes of a public key, 96 of a signature.
 *
 * Decoding returns false for what the relic-based implementation refused. What it accepts is any
 * point of the curve, in the subgroup or not, and some values that are no point at all.
 */
bool DecodeLegacy(blst_p1& point, const uint8_t* bytes);
bool DecodeLegacy(blst_p2& point, const uint8_t* bytes);
void EncodeLegacy(uint8_t* bytes, const blst_p1& point);
void EncodeLegacy(uint8_t* bytes, const blst_p2& point);
} // namespace bls

#endif // DASH_CRYPTO_BLS_LEGACY_H
