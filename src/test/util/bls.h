// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_TEST_UTIL_BLS_H
#define BITCOIN_TEST_UTIL_BLS_H

#include <bls/bls.h>

//! Sign under an explicit scheme. Production code can only sign basic.
CBLSSignature SignBLS(const CBLSSecretKey& sk, const uint256& hash, BLSScheme scheme);
//! Secure-aggregate under an explicit scheme. Production code can only aggregate basic.
CBLSSignature AggregateSecureBLS(Span<CBLSSignature> sigs, Span<CBLSPublicKey> pks, const uint256& hash, BLSScheme scheme);

#endif // BITCOIN_TEST_UTIL_BLS_H
