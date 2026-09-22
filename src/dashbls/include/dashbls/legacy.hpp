// Copyright (c) 2021 The Dash Core developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef SRC_LEGACY_HPP_
#define SRC_LEGACY_HPP_

extern "C" {
#include "blst.h"
}

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
void ep2_map_legacy(blst_p2 *p, const uint8_t *msg, int len);
} // namespace bls

#endif  // #define SRC_LEGACY_HPP_
