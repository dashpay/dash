// Copyright 2020 Chia Network Inc

// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at

//    http://www.apache.org/licenses/LICENSE-2.0

// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "schemes.hpp"

#include <string.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <set>

#include "bls.hpp"
#include "elements.hpp"
#include "hdkeys.hpp"

using std::string;
using std::vector;

namespace bls {

template <typename GetBytesFn>
static void HashPubKeys(blst_scalar* computedTs, size_t nPubKeys, GetBytesFn getBytes)
{
    std::vector<uint8_t> vecBuffer(nPubKeys * G1Element::SIZE);

    for (size_t i = 0; i < nPubKeys; i++) {
        const uint8_t* pkBytes = getBytes(i);
        memcpy(vecBuffer.data() + i * G1Element::SIZE, pkBytes, G1Element::SIZE);
    }

    uint8_t pkHash[32];
    Util::Hash256(pkHash, vecBuffer.data(), nPubKeys * G1Element::SIZE);
    for (size_t i = 0; i < nPubKeys; ++i) {
        uint8_t hash[32];
        uint8_t buffer[4 + 32];
        memset(buffer, 0, 4);
        // Set first 4 bytes to index, to generate different ts
        Util::IntToFourBytes(buffer, i);
        // Set next 32 bytes as the hash of all the public keys
        std::memcpy(buffer + 4, pkHash, 32);
        Util::Hash256(hash, buffer, 4 + 32);

        // reduces the hash mod the group order, like the former
        // bn_read_bin + bn_mod
        blst_scalar_from_be_bytes(&computedTs[i], hash, 32);
    }
}

enum InvariantResult { BAD = false, GOOD = true, CONTINUE };

// Enforce argument invariants for Agg Sig Verification
InvariantResult VerifyAggregateSignatureArguments(
    const size_t nPubKeys,
    const size_t nMessages,
    const G2Element& signature)
{
    if (nPubKeys == 0) {
        return (nMessages == 0 && signature == G2Element() ? GOOD : BAD);
    }
    if (nPubKeys != nMessages) {
        return BAD;
    }
    return CONTINUE;
}

/* These are all for the min-pubkey-size variant.
   TODO : analogs for min-signature-size
*/
const std::string BasicSchemeMPL::CIPHERSUITE_ID =
    "BLS_SIG_BLS12381G2_XMD:SHA-256_SSWU_RO_NUL_";
const std::string AugSchemeMPL::CIPHERSUITE_ID =
    "BLS_SIG_BLS12381G2_XMD:SHA-256_SSWU_RO_AUG_";
const std::string PopSchemeMPL::CIPHERSUITE_ID =
    "BLS_SIG_BLS12381G2_XMD:SHA-256_SSWU_RO_POP_";
const std::string PopSchemeMPL::POP_CIPHERSUITE_ID =
    "BLS_POP_BLS12381G2_XMD:SHA-256_SSWU_RO_POP_";

PrivateKey CoreMPL::KeyGen(const vector<uint8_t>& seed)
{
    return HDKeys::KeyGen(seed);
}

PrivateKey CoreMPL::KeyGen(const Bytes& seed) { return HDKeys::KeyGen(seed); }

vector<uint8_t> CoreMPL::SkToPk(const PrivateKey& seckey)
{
    return seckey.GetG1Element().Serialize();
}

G1Element CoreMPL::SkToG1(const PrivateKey& seckey)
{
    return seckey.GetG1Element();
}

G2Element CoreMPL::Sign(
    const PrivateKey& seckey,
    const vector<uint8_t>& message)
{
    return CoreMPL::Sign(seckey, Bytes(message));
}

G2Element CoreMPL::Sign(const PrivateKey& seckey, const Bytes& message)
{
    return seckey.SignG2(
        message.begin(),
        message.size(),
        (const uint8_t*)strCiphersuiteId.c_str(),
        strCiphersuiteId.length());
}

bool CoreMPL::Verify(
    const vector<uint8_t>& pubkey,
    const vector<uint8_t>& message,  // unhashed
    const vector<uint8_t>& signature)
{
    return CoreMPL::Verify(
        G1Element::FromBytes(Bytes(pubkey)),
        Bytes(message),
        G2Element::FromBytes(Bytes(signature)));
}

bool CoreMPL::Verify(
    const Bytes& pubkey,
    const Bytes& message,
    const Bytes& signature)
{
    return CoreMPL::Verify(
        G1Element::FromBytes(pubkey), message, G2Element::FromBytes(signature));
}

bool CoreMPL::Verify(
    const G1Element& pubkey,
    const vector<uint8_t>& message,  // unhashed
    const G2Element& signature)
{
    return CoreMPL::Verify(pubkey, Bytes(message), signature);
}

bool CoreMPL::Verify(
    const G1Element& pubkey,
    const Bytes& message,
    const G2Element& signature)
{
    // Keep the relic-era semantics rather than blst_core_verify: elements are
    // gated by IsValid() (which accepts infinity) and the check is the product
    // of pairings, so e.g. an infinity pubkey with an infinity signature
    // verifies as it always did.
    const G2Element hashedPoint = G2Element::FromMessage(message, (const uint8_t*)strCiphersuiteId.c_str(), strCiphersuiteId.length());

    if (!pubkey.IsValid()) {
        return false;
    }
    if (!signature.IsValid()) {
        return false;
    }

    std::array<blst_p1_affine, 2> g1s;
    std::array<blst_p2_affine, 2> g2s;

    G1Element::Generator().Negate().ToAffine(&g1s[0]);
    pubkey.ToAffine(&g1s[1]);
    signature.ToAffine(&g2s[0]);
    hashedPoint.ToAffine(&g2s[1]);

    return CoreMPL::NativeVerify(g1s.data(), g2s.data(), 2);
}

std::array<uint8_t, G2Element::SIZE> CoreMPL::Aggregate(const vector<vector<uint8_t>> &signatures)
{
    vector<G2Element> elements;
    elements.reserve(signatures.size());
    for (const vector<uint8_t>& signature : signatures) {
        elements.push_back(G2Element::FromByteVector(signature));
    }
    return CoreMPL::Aggregate(elements).SerializeToArray();
}

std::array<uint8_t, G2Element::SIZE> CoreMPL::Aggregate(const vector<Bytes>& signatures)
{
    vector<G2Element> elements;
    elements.reserve(signatures.size());
    for (const Bytes& signature : signatures) {
        elements.push_back(G2Element::FromBytes(signature));
    }
    return CoreMPL::Aggregate(elements).SerializeToArray();
}

G2Element CoreMPL::Aggregate(const vector<G2Element>& signatures)
{
    G2Element aggregated;
    for (const G2Element& signature : signatures) {
        aggregated += signature;
    }
    return aggregated;
}

G1Element CoreMPL::Aggregate(const vector<G1Element>& publicKeys)
{
    G1Element aggregated;
    for (const G1Element& publicKey : publicKeys) {
        aggregated += publicKey;
    }
    return aggregated;
}

G2Element CoreMPL::AggregateSecure(std::vector<G1Element> const &vecPublicKeys,
                                   std::vector<G2Element> const &vecSignatures,
                                   const Bytes& message,
                                   const bool fLegacy) {
    if (vecSignatures.size() != vecPublicKeys.size()) {
        throw std::invalid_argument("LegacySchemeMPL::AggregateSigs sigs.size() != pubKeys.size()");
    }

    std::vector<blst_scalar> computedTs(vecPublicKeys.size());
    std::vector<std::pair<std::array<uint8_t, G1Element::SIZE>, const G2Element*>> vecSorted(vecPublicKeys.size());
    for (size_t i = 0; i < vecPublicKeys.size(); i++) {
        vecSorted[i] = std::make_pair(vecPublicKeys[i].SerializeToArray(fLegacy), &vecSignatures[i]);
    }
    std::sort(vecSorted.begin(), vecSorted.end(), [](const auto& a, const auto& b) {
        return std::memcmp(a.first.data(), b.first.data(), G1Element::SIZE) < 0;
    });

    HashPubKeys(computedTs.data(), vecSorted.size(),
                [&](size_t i) { return vecSorted[i].first.data(); });

    // Raise all signatures to power of the corresponding t's and aggregate the results into aggSig
    // Also accumulates aggregation info for each signature
    std::vector<G2Element> expSigs;
    expSigs.reserve(vecSorted.size());
    for (size_t i = 0; i < vecSorted.size(); i++) {
        expSigs.emplace_back(*vecSorted[i].second * computedTs[i]);
    }

    return CoreMPL::Aggregate(expSigs);
}

G2Element CoreMPL::AggregateSecure(std::vector<G1Element> const &vecPublicKeys,
                                   std::vector<G2Element> const &vecSignatures,
                                   const Bytes& message) {
    return CoreMPL::AggregateSecure(vecPublicKeys, vecSignatures, message, false);
}

bool CoreMPL::VerifySecure(const std::vector<G1Element>& vecPublicKeys,
                           const G2Element& signature,
                           const Bytes& message,
                           const bool fLegacy) {
    std::vector<blst_scalar> computedTs(vecPublicKeys.size());
    std::vector<std::array<uint8_t, G1Element::SIZE>> vecSorted(vecPublicKeys.size());
    for (size_t i = 0; i < vecPublicKeys.size(); i++) {
        vecSorted[i] = vecPublicKeys[i].SerializeToArray(fLegacy);
    }
    std::sort(vecSorted.begin(), vecSorted.end(), [](const auto& a, const auto& b) -> bool {
        return std::memcmp(a.data(), b.data(), G1Element::SIZE) < 0;
    });

    HashPubKeys(computedTs.data(), vecSorted.size(),
                [&](size_t i) { return vecSorted[i].data(); });

    G1Element publicKey;
    for (size_t i = 0; i < vecSorted.size(); ++i) {
        G1Element g1 = G1Element::FromBytes(Bytes(vecSorted[i]), fLegacy);
        publicKey += g1 * computedTs[i];
    }

    return AggregateVerify({publicKey}, {message}, {signature});
}

bool CoreMPL::VerifySecure(const std::vector<G1Element>& vecPublicKeys,
                           const G2Element& signature,
                           const Bytes& message) {
    return CoreMPL::VerifySecure(vecPublicKeys, signature, message, false);
}

bool CoreMPL::AggregateVerify(
    const vector<vector<uint8_t>>& pubkeys,
    const vector<vector<uint8_t>>& messages,  // unhashed
    const vector<uint8_t>& signature)
{
    const std::vector<Bytes> vecPubKeyBytes(pubkeys.begin(), pubkeys.end());
    const std::vector<Bytes> vecMessagesBytes(messages.begin(), messages.end());
    return CoreMPL::AggregateVerify(
        vecPubKeyBytes, vecMessagesBytes, Bytes(signature));
}

bool CoreMPL::AggregateVerify(
    const vector<Bytes>& pubkeys,
    const vector<Bytes>& messages,  // unhashed
    const Bytes& signature)
{
    const size_t nPubKeys = pubkeys.size();
    const G2Element signatureElement = G2Element::FromBytes(signature);
    const auto arg_check = VerifyAggregateSignatureArguments(
        nPubKeys, messages.size(), signatureElement);
    if (arg_check != CONTINUE) {
        return arg_check;
    }

    vector<G1Element> pubkeyElements;
    pubkeyElements.reserve(nPubKeys);
    for (size_t i = 0; i < nPubKeys; ++i) {
        pubkeyElements.push_back(G1Element::FromBytes(pubkeys[i]));
    }
    return CoreMPL::AggregateVerify(pubkeyElements, messages, signatureElement);
}

bool CoreMPL::AggregateVerify(
    const vector<G1Element>& pubkeys,
    const vector<vector<uint8_t>>& messages,
    const G2Element& signature)
{
    return CoreMPL::AggregateVerify(
        pubkeys,
        std::vector<Bytes>(messages.begin(), messages.end()),
        signature);
}

bool CoreMPL::AggregateVerify(
    const vector<G1Element>& pubkeys,
    const vector<Bytes>& messages,
    const G2Element& signature)
{
    const size_t nPubKeys = pubkeys.size();
    const auto arg_check =
        VerifyAggregateSignatureArguments(nPubKeys, messages.size(), signature);
    if (arg_check != CONTINUE) {
        return arg_check;
    }

    std::vector<blst_p1_affine> vecG1(nPubKeys + 1);
    std::vector<blst_p2_affine> vecG2(nPubKeys + 1);
    G1Element::Generator().Negate().ToAffine(&vecG1[0]);
    if (!signature.IsValid()) {
        return false;
    }
    signature.ToAffine(&vecG2[0]);

    for (size_t i = 0; i < nPubKeys; ++i) {
        if (!pubkeys[i].IsValid()) {
            return false;
        }
        pubkeys[i].ToAffine(&vecG1[i + 1]);
        G2Element::FromMessage(messages[i], (const uint8_t*)strCiphersuiteId.c_str(), strCiphersuiteId.length()).ToAffine(&vecG2[i + 1]);
    }

    return CoreMPL::NativeVerify(vecG1.data(), vecG2.data(), nPubKeys + 1);
}

bool CoreMPL::NativeVerify(const blst_p1_affine* pubkeys,
                           const blst_p2_affine* mappedHashes,
                           size_t length)
{
    // 1 =? prod e(pubkey[i], hash[i])
    // A pairing with the point at infinity on either side contributes the
    // identity, matching relic's pc_map_sim.
    blst_fp12 candidate = *blst_fp12_one();

    for (size_t i = 0; i < length; ++i) {
        if (blst_p1_affine_is_inf(&pubkeys[i]) ||
            blst_p2_affine_is_inf(&mappedHashes[i])) {
            continue;
        }
        blst_fp12 tmpPairing;
        blst_miller_loop(&tmpPairing, &mappedHashes[i], &pubkeys[i]);
        blst_fp12_mul(&candidate, &candidate, &tmpPairing);
    }

    blst_final_exp(&candidate, &candidate);
    return blst_fp12_is_one(&candidate);
}

PrivateKey CoreMPL::DeriveChildSk(const PrivateKey& sk, uint32_t index)
{
    return HDKeys::DeriveChildSk(sk, index);
}

PrivateKey CoreMPL::DeriveChildSkUnhardened(
    const PrivateKey& sk,
    uint32_t index)
{
    return HDKeys::DeriveChildSkUnhardened(sk, index);
}

G1Element CoreMPL::DeriveChildPkUnhardened(const G1Element& pk, uint32_t index)
{
    return HDKeys::DeriveChildG1Unhardened(pk, index);
}

bool BasicSchemeMPL::AggregateVerify(
    const vector<vector<uint8_t>>& pubkeys,
    const vector<vector<uint8_t>>& messages,
    const vector<uint8_t>& signature)
{
    const size_t nPubKeys = pubkeys.size();
    auto arg_check = VerifyAggregateSignatureArguments(
        nPubKeys, messages.size(), G2Element::FromByteVector(signature));
    if (arg_check != CONTINUE) {
        return arg_check;
    }

    const std::set<vector<uint8_t>> setMessages(
        messages.begin(), messages.end());
    if (setMessages.size() != nPubKeys) {
        return false;
    }
    return CoreMPL::AggregateVerify(pubkeys, messages, signature);
}

bool BasicSchemeMPL::AggregateVerify(
    const vector<Bytes>& pubkeys,
    const vector<Bytes>& messages,
    const Bytes& signature)
{
    const size_t nPubKeys = pubkeys.size();
    const auto arg_check = VerifyAggregateSignatureArguments(
        nPubKeys, messages.size(), G2Element::FromBytes(signature));
    if (arg_check != CONTINUE)
        return arg_check;

    std::set<vector<uint8_t>> setMessages;
    for (const auto& message : messages) {
        setMessages.insert({message.begin(), message.end()});
    }
    if (setMessages.size() != nPubKeys) {
        return false;
    }
    return CoreMPL::AggregateVerify(pubkeys, messages, signature);
}

bool BasicSchemeMPL::AggregateVerify(
    const vector<G1Element>& pubkeys,
    const vector<vector<uint8_t>>& messages,
    const G2Element& signature)
{
    const size_t nPubKeys = pubkeys.size();
    const auto arg_check =
        VerifyAggregateSignatureArguments(nPubKeys, messages.size(), signature);
    if (arg_check != CONTINUE) {
        return arg_check;
    }

    const std::set<vector<uint8_t>> setMessages(
        messages.begin(), messages.end());
    if (setMessages.size() != nPubKeys) {
        return false;
    }
    return CoreMPL::AggregateVerify(pubkeys, messages, signature);
}

bool BasicSchemeMPL::AggregateVerify(
    const vector<G1Element>& pubkeys,
    const vector<Bytes>& messages,
    const G2Element& signature)
{
    const size_t nPubKeys = pubkeys.size();
    const auto arg_check =
        VerifyAggregateSignatureArguments(nPubKeys, messages.size(), signature);
    if (arg_check != CONTINUE)
        return arg_check;

    std::set<vector<uint8_t>> setMessages;
    for (const auto& message : messages) {
        setMessages.insert({message.begin(), message.end()});
    }
    if (setMessages.size() != nPubKeys) {
        return false;
    }
    return CoreMPL::AggregateVerify(pubkeys, messages, signature);
}

G2Element AugSchemeMPL::Sign(
    const PrivateKey& seckey,
    const vector<uint8_t>& message)
{
    return AugSchemeMPL::Sign(seckey, message, seckey.GetG1Element());
}

G2Element AugSchemeMPL::Sign(const PrivateKey& seckey, const Bytes& message)
{
    return AugSchemeMPL::Sign(seckey, message, seckey.GetG1Element());
}

// Used for prepending different augMessage
G2Element AugSchemeMPL::Sign(
    const PrivateKey& seckey,
    const vector<uint8_t>& message,
    const G1Element& prepend_pk)
{
    return AugSchemeMPL::Sign(seckey, Bytes(message), prepend_pk);
}

// Used for prepending different augMessage
G2Element AugSchemeMPL::Sign(
    const PrivateKey& seckey,
    const Bytes& message,
    const G1Element& prepend_pk)
{
    vector<uint8_t> augMessage = prepend_pk.Serialize();
    augMessage.reserve(augMessage.size() + message.size());
    augMessage.insert(augMessage.end(), message.begin(), message.end());
    return CoreMPL::Sign(seckey, augMessage);
}

bool AugSchemeMPL::Verify(
    const vector<uint8_t>& pubkey,
    const vector<uint8_t>& message,
    const vector<uint8_t>& signature)
{
    vector<uint8_t> augMessage(pubkey);
    augMessage.reserve(augMessage.size() + message.size());
    augMessage.insert(augMessage.end(), message.begin(), message.end());
    return CoreMPL::Verify(pubkey, augMessage, signature);
}

bool AugSchemeMPL::Verify(
    const Bytes& pubkey,
    const Bytes& message,
    const Bytes& signature)
{
    vector<uint8_t> augMessage(pubkey.begin(), pubkey.end());
    augMessage.reserve(augMessage.size() + message.size());
    augMessage.insert(augMessage.end(), message.begin(), message.end());
    return CoreMPL::Verify(pubkey, Bytes(augMessage), Bytes(signature));
}

bool AugSchemeMPL::Verify(
    const G1Element& pubkey,
    const vector<uint8_t>& message,
    const G2Element& signature)
{
    return AugSchemeMPL::Verify(pubkey, Bytes(message), signature);
}

bool AugSchemeMPL::Verify(
    const G1Element& pubkey,
    const Bytes& message,
    const G2Element& signature)
{
    vector<uint8_t> augMessage = pubkey.Serialize();
    augMessage.reserve(augMessage.size() + message.size());
    augMessage.insert(augMessage.end(), message.begin(), message.end());
    return CoreMPL::Verify(pubkey, augMessage, signature);
}

bool AugSchemeMPL::AggregateVerify(
    const vector<vector<uint8_t>>& pubkeys,
    const vector<vector<uint8_t>>& messages,
    const vector<uint8_t>& signature)
{
    std::vector<Bytes> vecPubKeyBytes(pubkeys.begin(), pubkeys.end());
    std::vector<Bytes> vecMessagesBytes(messages.begin(), messages.end());
    return AugSchemeMPL::AggregateVerify(
        vecPubKeyBytes, vecMessagesBytes, Bytes(signature));
}

bool AugSchemeMPL::AggregateVerify(
    const vector<Bytes>& pubkeys,
    const vector<Bytes>& messages,
    const Bytes& signature)
{
    size_t nPubKeys = pubkeys.size();
    auto arg_check = VerifyAggregateSignatureArguments(
        nPubKeys, messages.size(), G2Element::FromBytes(signature));
    if (arg_check != CONTINUE) {
        return arg_check;
    }

    vector<vector<uint8_t>> augMessages(nPubKeys);
    for (size_t i = 0; i < nPubKeys; ++i) {
        vector<uint8_t>& aug = augMessages[i];
        aug.reserve(pubkeys[i].size() + messages[i].size());
        aug.insert(aug.end(), pubkeys[i].begin(), pubkeys[i].end());
        aug.insert(aug.end(), messages[i].begin(), messages[i].end());
    }

    std::vector<Bytes> vecAugMessageBytes(
        augMessages.begin(), augMessages.end());
    return CoreMPL::AggregateVerify(pubkeys, vecAugMessageBytes, signature);
}

bool AugSchemeMPL::AggregateVerify(
    const vector<G1Element>& pubkeys,
    const vector<vector<uint8_t>>& messages,
    const G2Element& signature)
{
    std::vector<Bytes> vecMessagesBytes(messages.begin(), messages.end());
    return AugSchemeMPL::AggregateVerify(pubkeys, vecMessagesBytes, signature);
}

bool AugSchemeMPL::AggregateVerify(
    const vector<G1Element>& pubkeys,
    const vector<Bytes>& messages,
    const G2Element& signature)
{
    size_t nPubKeys = pubkeys.size();
    auto arg_check =
        VerifyAggregateSignatureArguments(nPubKeys, messages.size(), signature);
    if (arg_check != CONTINUE) {
        return arg_check;
    }

    vector<vector<uint8_t>> augMessages(nPubKeys);
    for (size_t i = 0; i < nPubKeys; ++i) {
        vector<uint8_t>& aug = augMessages[i];
        vector<uint8_t>&& pubkey = pubkeys[i].Serialize();
        aug.reserve(pubkey.size() + messages[i].size());
        aug.insert(aug.end(), pubkey.begin(), pubkey.end());
        aug.insert(aug.end(), messages[i].begin(), messages[i].end());
    }

    return CoreMPL::AggregateVerify(pubkeys, augMessages, signature);
}

G2Element PopSchemeMPL::PopProve(const PrivateKey& seckey)
{
    std::array<uint8_t, G1Element::SIZE> pubkey_bytes = seckey.GetG1Element().SerializeToArray();

    return seckey.SignG2(
        pubkey_bytes.data(),
        pubkey_bytes.size(),
        (const uint8_t*)POP_CIPHERSUITE_ID.c_str(),
        POP_CIPHERSUITE_ID.length());
}

bool PopSchemeMPL::PopVerify(
    const G1Element& pubkey,
    const G2Element& signature_proof)
{
    blst_p1_affine pubkeyAffine;
    blst_p2_affine sigAffine;

    pubkey.ToAffine(&pubkeyAffine);
    signature_proof.ToAffine(&sigAffine);
    std::array<uint8_t, G1Element::SIZE> pubkey_bytes = pubkey.SerializeToArray();

    auto err = blst_core_verify_pk_in_g1(
        &pubkeyAffine,
        &sigAffine,
        true, /*hash*/
        pubkey_bytes.data(),
        pubkey_bytes.size(),
        (const uint8_t*)POP_CIPHERSUITE_ID.c_str(),
        POP_CIPHERSUITE_ID.length());

    return err == BLST_SUCCESS;
}

bool PopSchemeMPL::PopVerify(
    const vector<uint8_t>& pubkey,
    const vector<uint8_t>& proof)
{
    return PopSchemeMPL::PopVerify(Bytes(pubkey), Bytes(proof));
}

bool PopSchemeMPL::PopVerify(const Bytes& pubkey, const Bytes& proof)
{
    return PopSchemeMPL::PopVerify(
        G1Element::FromBytes(pubkey), G2Element::FromBytes(proof));
}

bool PopSchemeMPL::FastAggregateVerify(
    const vector<G1Element>& pubkeys,
    const vector<uint8_t>& message,
    const G2Element& signature)
{
    return PopSchemeMPL::FastAggregateVerify(
        pubkeys, Bytes(message), signature);
}

bool PopSchemeMPL::FastAggregateVerify(
    const vector<G1Element>& pubkeys,
    const Bytes& message,
    const G2Element& signature)
{
    if (pubkeys.size() == 0) {
        return false;
    }
    // No VerifyAggregateSignatureArguments checks required here as we have
    // exactly one pubkey and one message.
    return CoreMPL::Verify(CoreMPL::Aggregate(pubkeys), message, signature);
}

bool PopSchemeMPL::FastAggregateVerify(
    const vector<vector<uint8_t>>& pubkeys,
    const vector<uint8_t>& message,
    const vector<uint8_t>& signature)
{
    const std::vector<Bytes> vecPubKeyBytes(pubkeys.begin(), pubkeys.end());
    return PopSchemeMPL::FastAggregateVerify(
        vecPubKeyBytes, Bytes(message), Bytes(signature));
}

bool PopSchemeMPL::FastAggregateVerify(
    const vector<Bytes>& pubkeys,
    const Bytes& message,
    const Bytes& signature)
{
    const size_t nPubKeys = pubkeys.size();
    if (nPubKeys == 0) {
        return false;
    }

    vector<G1Element> pkelements;
    for (size_t i = 0; i < nPubKeys; ++i) {
        pkelements.push_back(G1Element::FromBytes(pubkeys[i]));
    }

    return PopSchemeMPL::FastAggregateVerify(
        pkelements, message, G2Element::FromBytes(signature));
}

G2Element LegacySchemeMPL::Sign(const PrivateKey& seckey, const Bytes& message)
{
    return seckey.SignG2(message.begin(), message.size(), nullptr, 0, true);
}

bool LegacySchemeMPL::Verify(const G1Element &pubkey, const Bytes& message, const G2Element &signature)
{
    blst_p1_affine g1s[2];
    blst_p2_affine g2s[2];

    G1Element::Generator().Negate().ToAffine(&g1s[0]);
    pubkey.ToAffine(&g1s[1]);
    signature.ToAffine(&g2s[0]);
    G2Element::FromMessage(message, nullptr, 0, true).ToAffine(&g2s[1]);

    return CoreMPL::NativeVerify(g1s, g2s, 2);
}

G2Element LegacySchemeMPL::AggregateSecure(std::vector<G1Element> const &vecPublicKeys,
                                          std::vector<G2Element> const &vecSignatures,
                                          const Bytes& message) {
    return CoreMPL::AggregateSecure(vecPublicKeys, vecSignatures, message, true);
}

bool LegacySchemeMPL::VerifySecure(const std::vector<G1Element>& vecPublicKeys,
                                   const G2Element& signature,
                                   const Bytes& message) {
    return CoreMPL::VerifySecure(vecPublicKeys, signature, message, true);
}

bool LegacySchemeMPL::AggregateVerify(const vector<G1Element> &pubkeys,
                                      const vector<Bytes> &messages,
                                      const G2Element &signature)
{
    const size_t nPubKeys = pubkeys.size();
    const auto arg_check = VerifyAggregateSignatureArguments(nPubKeys, messages.size(), signature);
    if (arg_check != CONTINUE) return arg_check;

    std::vector<blst_p1_affine> vecG1(nPubKeys + 1);
    std::vector<blst_p2_affine> vecG2(nPubKeys + 1);
    G1Element::Generator().Negate().ToAffine(&vecG1[0]);
    signature.ToAffine(&vecG2[0]);

    for (size_t i = 0; i < nPubKeys; ++i) {
        pubkeys[i].ToAffine(&vecG1[i + 1]);
        G2Element::FromMessage(messages[i], nullptr, 0, true).ToAffine(&vecG2[i + 1]);
    }

    return CoreMPL::NativeVerify(vecG1.data(), vecG2.data(), nPubKeys + 1);
}

}  // end namespace bls
