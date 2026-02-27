// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <bls/bls.h>
#include <bls/bls_ies.h>
#include <streams.h>
#include <test/fuzz/FuzzedDataProvider.h>
#include <test/fuzz/fuzz.h>
#include <test/fuzz/util.h>
#include <uint256.h>

#include <array>
#include <cstdint>
#include <vector>

namespace {

//! Build a CBLSSecretKey from fuzzed bytes. Returns invalid key on bad input.
CBLSSecretKey MakeSecretKey(FuzzedDataProvider& fuzzed_data_provider)
{
    auto bytes = fuzzed_data_provider.ConsumeBytes<uint8_t>(CBLSSecretKey::SerSize);
    bytes.resize(CBLSSecretKey::SerSize);
    return CBLSSecretKey(bytes);
}

//! Build a CBLSPublicKey from fuzzed bytes via deserialization.
CBLSPublicKey MakePublicKey(FuzzedDataProvider& fuzzed_data_provider)
{
    auto bytes = fuzzed_data_provider.ConsumeBytes<uint8_t>(CBLSPublicKey::SerSize);
    bytes.resize(CBLSPublicKey::SerSize);
    CBLSPublicKey pk;
    pk.SetBytes(bytes, fuzzed_data_provider.ConsumeBool());
    return pk;
}

//! Build a CBLSSignature from fuzzed bytes via deserialization.
CBLSSignature MakeSignature(FuzzedDataProvider& fuzzed_data_provider)
{
    auto bytes = fuzzed_data_provider.ConsumeBytes<uint8_t>(CBLSSignature::SerSize);
    bytes.resize(CBLSSignature::SerSize);
    CBLSSignature sig;
    sig.SetBytes(bytes, fuzzed_data_provider.ConsumeBool());
    return sig;
}

uint256 MakeHash(FuzzedDataProvider& fuzzed_data_provider)
{
    auto bytes = fuzzed_data_provider.ConsumeBytes<uint8_t>(32);
    bytes.resize(32);
    uint256 h;
    memcpy(h.begin(), bytes.data(), 32);
    return h;
}

} // namespace

void initialize_bls_operations() { BLSInit(); }

FUZZ_TARGET(bls_operations, .init = initialize_bls_operations)
{
    FuzzedDataProvider fuzzed_data_provider(buffer.data(), buffer.size());

    // Test both legacy and basic BLS schemes.
    // Intentionally mutating this global: each fuzz run is an isolated process,
    // so there is no cross-run state leakage.
    const bool use_legacy = fuzzed_data_provider.ConsumeBool();
    bls::bls_legacy_scheme.store(use_legacy);

    LIMITED_WHILE(fuzzed_data_provider.remaining_bytes() > 0, 32)
    {
        switch (fuzzed_data_provider.ConsumeIntegralInRange<int>(0, 10)) {
        case 0: {
            // Key generation from fuzzed bytes + public key derivation
            CBLSSecretKey sk = MakeSecretKey(fuzzed_data_provider);
            if (sk.IsValid()) {
                CBLSPublicKey pk = sk.GetPublicKey();
                (void)pk.IsValid();
                (void)pk.GetHash();
                (void)pk.ToString();
            }
            break;
        }
        case 1: {
            // Signing + verification with fuzzed data
            CBLSSecretKey sk = MakeSecretKey(fuzzed_data_provider);
            uint256 hash = MakeHash(fuzzed_data_provider);
            const bool legacy_sign = fuzzed_data_provider.ConsumeBool();

            if (sk.IsValid()) {
                CBLSSignature sig = sk.Sign(hash, legacy_sign);
                if (sig.IsValid()) {
                    CBLSPublicKey pk = sk.GetPublicKey();
                    // Should verify with matching scheme
                    (void)sig.VerifyInsecure(pk, hash, legacy_sign);
                    // May or may not verify with opposite scheme
                    (void)sig.VerifyInsecure(pk, hash, !legacy_sign);
                }
            }
            break;
        }
        case 2: {
            // Verification with completely fuzzed key/sig/hash
            CBLSPublicKey pk = MakePublicKey(fuzzed_data_provider);
            CBLSSignature sig = MakeSignature(fuzzed_data_provider);
            uint256 hash = MakeHash(fuzzed_data_provider);
            const bool legacy_verify = fuzzed_data_provider.ConsumeBool();

            (void)sig.VerifyInsecure(pk, hash, legacy_verify);
            break;
        }
        case 3: {
            // Public key aggregation (filter to valid keys — impl access on invalid is UB)
            const size_t count = fuzzed_data_provider.ConsumeIntegralInRange<size_t>(0, 5);
            std::vector<CBLSPublicKey> pks;
            for (size_t i = 0; i < count; i++) {
                auto pk = MakePublicKey(fuzzed_data_provider);
                if (pk.IsValid()) pks.push_back(pk);
            }
            (void)CBLSPublicKey::AggregateInsecure(pks);
            break;
        }
        case 4: {
            // Signature aggregation (filter to valid sigs)
            const size_t count = fuzzed_data_provider.ConsumeIntegralInRange<size_t>(0, 5);
            std::vector<CBLSSignature> sigs;
            for (size_t i = 0; i < count; i++) {
                auto sig = MakeSignature(fuzzed_data_provider);
                if (sig.IsValid()) sigs.push_back(sig);
            }
            (void)CBLSSignature::AggregateInsecure(sigs);
            break;
        }
        case 5: {
            // Secret key aggregation (filter to valid keys)
            const size_t count = fuzzed_data_provider.ConsumeIntegralInRange<size_t>(0, 5);
            std::vector<CBLSSecretKey> sks;
            for (size_t i = 0; i < count; i++) {
                auto sk = MakeSecretKey(fuzzed_data_provider);
                if (sk.IsValid()) sks.push_back(sk);
            }
            (void)CBLSSecretKey::AggregateInsecure(sks);
            break;
        }
        case 6: {
            // Threshold secret key share — SecretKeyShare validates inputs internally
            const size_t threshold = fuzzed_data_provider.ConsumeIntegralInRange<size_t>(1, 4);
            std::vector<CBLSSecretKey> msk;
            for (size_t i = 0; i < threshold; i++) {
                msk.push_back(MakeSecretKey(fuzzed_data_provider));
            }
            CBLSId id(MakeHash(fuzzed_data_provider));
            CBLSSecretKey share;
            (void)share.SecretKeyShare(msk, id);
            break;
        }
        case 7: {
            // Threshold public key share — PublicKeyShare validates inputs internally
            const size_t threshold = fuzzed_data_provider.ConsumeIntegralInRange<size_t>(1, 4);
            std::vector<CBLSPublicKey> mpk;
            for (size_t i = 0; i < threshold; i++) {
                mpk.push_back(MakePublicKey(fuzzed_data_provider));
            }
            CBLSId id(MakeHash(fuzzed_data_provider));
            CBLSPublicKey share;
            (void)share.PublicKeyShare(mpk, id);
            break;
        }
        case 8: {
            // Signature recovery from shares — Recover validates inputs internally
            const size_t count = fuzzed_data_provider.ConsumeIntegralInRange<size_t>(0, 4);
            std::vector<CBLSSignature> sigs;
            std::vector<CBLSId> ids;
            for (size_t i = 0; i < count; i++) {
                sigs.push_back(MakeSignature(fuzzed_data_provider));
                ids.emplace_back(MakeHash(fuzzed_data_provider));
            }
            CBLSSignature recovered;
            (void)recovered.Recover(sigs, ids);
            break;
        }
        case 9: {
            // DH key exchange
            CBLSSecretKey sk = MakeSecretKey(fuzzed_data_provider);
            CBLSPublicKey pk = MakePublicKey(fuzzed_data_provider);
            CBLSPublicKey result;
            (void)result.DHKeyExchange(sk, pk);
            break;
        }
        case 10: {
            // Aggregated signature verification
            const size_t count = fuzzed_data_provider.ConsumeIntegralInRange<size_t>(1, 4);
            std::vector<CBLSPublicKey> pks;
            std::vector<uint256> hashes;
            for (size_t i = 0; i < count; i++) {
                auto pk = MakePublicKey(fuzzed_data_provider);
                uint256 h = MakeHash(fuzzed_data_provider);
                if (pk.IsValid()) {
                    pks.push_back(pk);
                    hashes.push_back(h);
                }
            }
            if (!pks.empty()) {
                CBLSSignature sig = MakeSignature(fuzzed_data_provider);
                // VerifyInsecureAggregated asserts non-empty + equal sizes
                (void)sig.VerifyInsecureAggregated(pks, hashes);
                // VerifySecureAggregated accesses pk.impl directly
                uint256 single_hash = MakeHash(fuzzed_data_provider);
                (void)sig.VerifySecureAggregated(pks, single_hash);
            }
            break;
        }
        } // switch
    } // while
}

FUZZ_TARGET(bls_ies, .init = initialize_bls_operations)
{
    FuzzedDataProvider fuzzed_data_provider(buffer.data(), buffer.size());

    // Intentionally mutating global; see bls_operations target comment.
    bls::bls_legacy_scheme.store(fuzzed_data_provider.ConsumeBool());

    switch (fuzzed_data_provider.ConsumeIntegralInRange<int>(0, 2)) {
    case 0: {
        // CBLSIESEncryptedBlob deserialization + decrypt with fuzzed key
        CBLSIESEncryptedBlob blob;

        // Build blob from fuzzed data
        blob.ephemeralPubKey = MakePublicKey(fuzzed_data_provider);
        auto iv_bytes = fuzzed_data_provider.ConsumeBytes<uint8_t>(32);
        iv_bytes.resize(32);
        memcpy(blob.ivSeed.begin(), iv_bytes.data(), 32);
        blob.data = fuzzed_data_provider.ConsumeBytes<uint8_t>(fuzzed_data_provider.ConsumeIntegralInRange<size_t>(0, 256));

        (void)blob.IsValid();

        size_t idx = fuzzed_data_provider.ConsumeIntegralInRange<size_t>(0, 16);
        (void)blob.GetIV(idx);

        CBLSSecretKey sk = MakeSecretKey(fuzzed_data_provider);
        CDataStream ds(SER_NETWORK, PROTOCOL_VERSION);
        (void)blob.Decrypt(idx, sk, ds);
        break;
    }
    case 1: {
        // CBLSIESMultiRecipientBlobs decrypt with fuzzed data
        CBLSIESMultiRecipientBlobs multi;
        multi.ephemeralPubKey = MakePublicKey(fuzzed_data_provider);

        auto iv_bytes = fuzzed_data_provider.ConsumeBytes<uint8_t>(32);
        iv_bytes.resize(32);
        memcpy(multi.ivSeed.begin(), iv_bytes.data(), 32);

        const size_t blob_count = fuzzed_data_provider.ConsumeIntegralInRange<size_t>(0, 8);
        multi.blobs.resize(blob_count);
        for (size_t i = 0; i < blob_count; i++) {
            multi.blobs[i] = fuzzed_data_provider.ConsumeBytes<uint8_t>(
                fuzzed_data_provider.ConsumeIntegralInRange<size_t>(0, 128));
        }

        // Try to decrypt each blob
        CBLSSecretKey sk = MakeSecretKey(fuzzed_data_provider);
        for (size_t i = 0; i < blob_count; i++) {
            CBLSIESMultiRecipientBlobs::Blob result;
            (void)multi.Decrypt(i, sk, result);
        }
        break;
    }
    case 2: {
        // Roundtrip: encrypt then decrypt with matching keys
        CBLSSecretKey recipient_sk = MakeSecretKey(fuzzed_data_provider);
        if (!recipient_sk.IsValid()) break;

        CBLSPublicKey recipient_pk = recipient_sk.GetPublicKey();
        if (!recipient_pk.IsValid()) break;

        const size_t data_size = fuzzed_data_provider.ConsumeIntegralInRange<size_t>(0, 128);
        auto plaintext = fuzzed_data_provider.ConsumeBytes<uint8_t>(data_size);
        plaintext.resize(data_size);

        // Encrypt
        CBLSIESMultiRecipientBlobs multi;
        multi.InitEncrypt(1);
        bool encrypted = multi.Encrypt(0, recipient_pk, plaintext);
        if (encrypted) {
            // Decrypt
            CBLSIESMultiRecipientBlobs::Blob decrypted;
            bool decrypted_ok = multi.Decrypt(0, recipient_sk, decrypted);
            if (decrypted_ok) {
                assert(decrypted == plaintext);
            }
        }
        break;
    }
    } // switch
}
