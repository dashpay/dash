// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <platform/dpp/document.h>
#include <platform/dpp/identity.h>
#include <platform/drive/queries.h>
#include <platform/types.h>
#include <test/fuzz/FuzzedDataProvider.h>
#include <test/fuzz/fuzz.h>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

//! Every input below arrives from a DAPI node the wallet does not trust, is
//! handed to rs-dpp / drive-proof-verifier through the cxx bridge, and is
//! parsed long before anything about it has been verified. A malformed proof
//! must fail; it must never take the process down. The bridge converts Rust
//! panics into C++ exceptions, so a panic reaching the caller as a crash — or
//! not being caught by the adapters — is the bug these targets look for.

namespace {

//! Verification needs a network and a quorum key set before it will run. The
//! key is deliberately not a real quorum key: proofs cannot pass the final
//! signature check, which leaves the fuzzer working on everything before it —
//! protobuf decoding, GroveDB proof replay and document deserialization.
void InitVerifyContext()
{
    std::string error;
    platform::drive::SetContext("test", /*platform_activation_height=*/0, error);

    platform::drive::BridgeQuorumKey key;
    key.proof_hash.fill(0x11);
    key.pubkey.assign(48, 0x22);
    platform::drive::UpdateQuorumKeys(/*llmq_type=*/4, {key}, error);
}

//! Splits a fuzz buffer into the request the transport sent and the response
//! it got back, the pair every verifier takes. The split point is drawn from
//! the tail of the buffer, so a seed can be built by appending the request
//! length to a captured request/response pair.
std::pair<std::vector<uint8_t>, std::vector<uint8_t>> ConsumeRequestResponse(FuzzBufferType buffer)
{
    FuzzedDataProvider provider{buffer.data(), buffer.size()};
    const size_t request_size{provider.ConsumeIntegralInRange<size_t>(0, provider.remaining_bytes())};
    auto request{provider.ConsumeBytes<uint8_t>(request_size)};
    auto response{provider.ConsumeRemainingBytes<uint8_t>()};
    return {std::move(request), std::move(response)};
}

} // namespace

FUZZ_TARGET(platform_decode_identity)
{
    std::string error;
    (void)platform::dpp::DecodeIdentity(buffer, error);
}

FUZZ_TARGET(platform_decode_identity_public_key)
{
    std::string error;
    (void)platform::dpp::DecodeIdentityPublicKey(buffer, error);
}

FUZZ_TARGET(platform_decode_dpns_domain)
{
    platform::DpnsName name;
    (void)platform::dpp::DecodeDpnsDomain(buffer, name);
}

FUZZ_TARGET(platform_decode_dashpay_profile)
{
    platform::Profile profile;
    (void)platform::dpp::DecodeDashPayProfile(buffer, profile);
}

FUZZ_TARGET(platform_decode_contact_request)
{
    platform::ContactRequest request;
    (void)platform::dpp::DecodeDashPayContactRequest(buffer, request);
}

FUZZ_TARGET(platform_verify_get_identity, .init = InitVerifyContext)
{
    const auto [request, response] = ConsumeRequestResponse(buffer);
    std::optional<platform::Identity> identity;
    platform::ResponseMetadata meta;
    std::string error;
    if (!platform::drive::VerifyGetIdentity(request, response, identity, meta, error)) {
        assert(!error.empty());
    }
}

FUZZ_TARGET(platform_verify_get_identity_by_pubkey_hash, .init = InitVerifyContext)
{
    const auto [request, response] = ConsumeRequestResponse(buffer);
    std::optional<platform::Identity> identity;
    platform::ResponseMetadata meta;
    std::string error;
    if (!platform::drive::VerifyGetIdentityByPubKeyHash(request, response, identity, meta, error)) {
        assert(!error.empty());
    }
}

FUZZ_TARGET(platform_verify_get_identity_nonce, .init = InitVerifyContext)
{
    const auto [request, response] = ConsumeRequestResponse(buffer);
    std::optional<uint64_t> nonce;
    platform::ResponseMetadata meta;
    std::string error;
    if (!platform::drive::VerifyGetIdentityNonce(request, response, nonce, meta, error)) {
        assert(!error.empty());
    }
}

FUZZ_TARGET(platform_verify_get_identity_contract_nonce, .init = InitVerifyContext)
{
    const auto [request, response] = ConsumeRequestResponse(buffer);
    std::optional<uint64_t> nonce;
    platform::ResponseMetadata meta;
    std::string error;
    if (!platform::drive::VerifyGetIdentityContractNonce(request, response, nonce, meta, error)) {
        assert(!error.empty());
    }
}

FUZZ_TARGET(platform_verify_get_documents, .init = InitVerifyContext)
{
    const auto [request, response] = ConsumeRequestResponse(buffer);
    std::vector<platform::Bytes> documents;
    platform::ResponseMetadata meta;
    std::string error;
    if (!platform::drive::VerifyGetDocuments(request, response, documents, meta, error)) {
        assert(!error.empty());
        return;
    }
    // Verified documents are the input of the decoders above, so keep feeding
    // them onwards: a document that survives proof verification is still only
    // as trustworthy as the node that served it.
    for (const auto& document : documents) {
        platform::DpnsName name;
        (void)platform::dpp::DecodeDpnsDomain(document, name);
        platform::Profile profile;
        (void)platform::dpp::DecodeDashPayProfile(document, profile);
        platform::ContactRequest contact_request;
        (void)platform::dpp::DecodeDashPayContactRequest(document, contact_request);
    }
}

FUZZ_TARGET(platform_verify_get_contested_vote_state, .init = InitVerifyContext)
{
    const auto [request, response] = ConsumeRequestResponse(buffer);
    platform::drive::ContestedVoteState state;
    platform::ResponseMetadata meta;
    std::string error;
    if (!platform::drive::VerifyGetContestedVoteState(request, response, state, meta, error)) {
        assert(!error.empty());
    }
}
