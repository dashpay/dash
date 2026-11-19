// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <test/util/bls.h>

#include <crypto/common.h>
#include <crypto/sha256.h>
#include <dashbls/legacy.hpp>
#include <deploymentstatus.h>
#include <util/check.h>
#include <validation.h>

#include <algorithm>
#include <array>
#include <utility>
#include <vector>

CBLSSecretKey SecretFromScalar(uint8_t scalar)
{
    std::vector<uint8_t> bytes(32, 0);
    bytes.back() = scalar;
    CBLSSecretKey sk;
    sk.SetBytes(bytes);
    Assert(sk.IsValid());
    return sk;
}

BLSScheme SchemeForNextBlock(const ChainstateManager& chainman)
{
    LOCK(::cs_main);
    return DeploymentActiveAfter(chainman.ActiveChain().Tip(), chainman.GetConsensus(), Consensus::DEPLOYMENT_V19)
               ? BLSScheme::Basic
               : BLSScheme::Legacy;
}

CBLSSignature SignBLS(const CBLSSecretKey& sk, const uint256& hash, const BLSScheme scheme)
{
    if (scheme == BLSScheme::Basic || !sk.IsValid()) {
        return sk.Sign(hash);
    }
    // A signature is the secret key times the point of the message, which the legacy scheme maps in its own way.
    blst_p2 point;
    bls::ep2_map_legacy(&point, hash.begin(), hash.size());
    std::array<uint8_t, BLS_CURVE_SIG_SIZE> bytes;
    blst_p2_compress(bytes.data(), &point);
    CBLSSignature sig{bytes};
    sig.MulInsecure(sk);
    return sig;
}

CBLSSignature AggregateSecureBLS(Span<CBLSSignature> sigs, Span<CBLSPublicKey> pks, const uint256& hash,
                                 const BLSScheme scheme)
{
    if (scheme == BLSScheme::Basic || sigs.size() != pks.size() || sigs.empty()) {
        return CBLSSignature::AggregateSecure(sigs, pks, hash);
    }
    // Every signature is multiplied by a number derived from the encodings of all the keys, in their order.
    // The legacy scheme differs in that encoding.
    std::vector<std::pair<std::array<uint8_t, BLS_CURVE_PUBKEY_SIZE>, CBLSSignature>> signers;
    signers.reserve(sigs.size());
    for (size_t i = 0; i < sigs.size(); i++) {
        signers.emplace_back(pks[i].ToBytes(scheme), sigs[i]);
    }
    std::sort(signers.begin(), signers.end(), [](const auto& a, const auto& b) { return a.first < b.first; });

    CSHA256 hasher;
    for (const auto& [pk, _] : signers) {
        hasher.Write(pk.data(), pk.size());
    }
    std::array<uint8_t, 4 + CSHA256::OUTPUT_SIZE> preimage;
    hasher.Finalize(preimage.data() + 4);

    CBLSSignature ret;
    for (size_t i = 0; i < signers.size(); i++) {
        WriteBE32(preimage.data(), i);
        std::array<uint8_t, CSHA256::OUTPUT_SIZE> factor;
        CSHA256().Write(preimage.data(), preimage.size()).Finalize(factor.data());
        // A secret key holds a number below the group order
        blst_scalar reduced;
        blst_scalar_from_be_bytes(&reduced, factor.data(), factor.size());
        blst_bendian_from_scalar(factor.data(), &reduced);

        CBLSSignature& term{signers[i].second};
        term.MulInsecure(CBLSSecretKey{factor});
        if (i == 0) {
            ret = term;
        } else {
            ret.AggregateInsecure(term);
        }
    }
    return ret;
}
