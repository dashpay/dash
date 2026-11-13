// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <test/util/bls.h>

#include <dashbls/schemes.hpp>

namespace {

bls::CoreMPL& Scheme(const BLSScheme scheme)
{
    static bls::LegacySchemeMPL legacy;
    static bls::BasicSchemeMPL basic;
    return scheme == BLSScheme::Legacy ? static_cast<bls::CoreMPL&>(legacy)
                                       : static_cast<bls::CoreMPL&>(basic);
}

CBLSSignature SignatureFromElement(const bls::G2Element& element, const BLSScheme scheme)
{
    CBLSSignature sig;
    sig.SetBytes(element.SerializeToArray(scheme == BLSScheme::Legacy), scheme);
    return sig;
}

} // namespace

CBLSSignature SignBLS(const CBLSSecretKey& sk, const uint256& hash, const BLSScheme scheme)
{
    if (!sk.IsValid()) {
        return {};
    }
    const auto sk_bytes{sk.ToBytes(BLSScheme::Basic)};
    try {
        const bls::PrivateKey key{bls::PrivateKey::FromBytes(bls::Bytes(sk_bytes.data(), sk_bytes.size()), false)};
        const bls::G2Element element{Scheme(scheme).Sign(key, bls::Bytes(hash.begin(), hash.size()))};
        return SignatureFromElement(element, scheme);
    } catch (...) {
        return {};
    }
}

CBLSSignature AggregateSecureBLS(Span<CBLSSignature> sigs, Span<CBLSPublicKey> pks, const uint256& hash,
                                 const BLSScheme scheme)
{
    if (sigs.size() != pks.size() || sigs.empty()) {
        return {};
    }

    const bool legacy{scheme == BLSScheme::Legacy};
    std::vector<bls::G1Element> pubkeys;
    std::vector<bls::G2Element> signatures;
    pubkeys.reserve(pks.size());
    signatures.reserve(sigs.size());
    try {
        for (const CBLSPublicKey& pk : pks) {
            if (!pk.IsValid()) {
                pubkeys.emplace_back();
                continue;
            }
            const auto bytes{pk.ToBytes(scheme)};
            pubkeys.push_back(bls::G1Element::FromBytes(bls::Bytes(bytes.data(), bytes.size()), legacy));
        }
        for (const CBLSSignature& sig : sigs) {
            if (!sig.IsValid()) {
                signatures.emplace_back();
                continue;
            }
            const auto bytes{sig.ToBytes(scheme)};
            signatures.push_back(bls::G2Element::FromBytes(bls::Bytes(bytes.data(), bytes.size()), legacy));
        }
        const bls::G2Element element{
            Scheme(scheme).AggregateSecure(pubkeys, signatures, bls::Bytes(hash.begin(), hash.size()))};
        return SignatureFromElement(element, scheme);
    } catch (...) {
        return {};
    }
}
