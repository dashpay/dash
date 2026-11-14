// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_TEST_UTIL_BLS_H
#define BITCOIN_TEST_UTIL_BLS_H

#include <bls/bls.h>
#include <evo/types.h>

class ChainstateManager;

//! Scheme a block built on chainman's tip is validated under.
BLSScheme SchemeForNextBlock(const ChainstateManager& chainman);

//! Highest provider-transaction version a block built on chainman's tip may use.
inline uint16_t ProTxVersionForNextBlock(const ChainstateManager& chainman)
{
    return ProTxVersion::GetMax(SchemeForNextBlock(chainman) == BLSScheme::Basic, /*is_extended_addr=*/false);
}

inline const char* SchemeName(BLSScheme scheme) { return scheme == BLSScheme::Legacy ? "legacy" : "basic"; }

//! Sign under an explicit scheme. Production code can only sign basic.
CBLSSignature SignBLS(const CBLSSecretKey& sk, const uint256& hash, BLSScheme scheme);
//! Secure-aggregate under an explicit scheme. Production code can only aggregate basic.
CBLSSignature AggregateSecureBLS(Span<CBLSSignature> sigs, Span<CBLSPublicKey> pks, const uint256& hash, BLSScheme scheme);

//! Secret key whose scalar is `scalar` in the least significant byte.
CBLSSecretKey SecretFromScalar(uint8_t scalar);

//! Public key of secret scalar 1, legacy and basic encodings.
inline constexpr char PK1_LEG[]{
    "17f1d3a73197d7942695638c4fa9ac0fc3688c4f9774b905a14e3a3f171bac586c55e83ff97a1aeffb3af00adb22c6bb"};
inline constexpr char PK1_BAS[]{
    "97f1d3a73197d7942695638c4fa9ac0fc3688c4f9774b905a14e3a3f171bac586c55e83ff97a1aeffb3af00adb22c6bb"};

#endif // BITCOIN_TEST_UTIL_BLS_H
