// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <platform/dpp/document.h>
#include <platform/dpp/identity.h>
#include <platform/types.h>
#include <test/fuzz/fuzz.h>

#include <dash/platform/ffi.h>

#include <cstdint>
#include <string>

//! Every input below is handed to rs-dpp through the Platform SDK's cxx
//! bridge and parsed long before anything about it has been verified. A
//! malformed object must fail; it must never take the process down. The
//! bridge converts Rust panics into C++ exceptions, so a panic reaching the
//! caller as a crash, or not being caught by the adapters, is the bug these
//! targets look for. Proof verification itself runs inside dash-sdk against
//! network responses and is fuzzed upstream (rs-drive-proof-verifier).

namespace {

const platform_ffi::PlatformClient& Sdk()
{
    static const rust::Box<platform_ffi::PlatformClient> sdk{[] {
        auto client{platform_ffi::new_platform_client()};
        client->set_context("test", std::uint32_t{6}, "dash-testnet-51", std::uint32_t{12}, std::uint32_t{0});
        return client;
    }()};
    return *sdk;
}

} // namespace

FUZZ_TARGET(platform_decode_identity)
{
    std::string error;
    (void)platform::dpp::DecodeIdentity(Sdk(), buffer, error);
}

FUZZ_TARGET(platform_decode_identity_public_key)
{
    std::string error;
    (void)platform::dpp::DecodeIdentityPublicKey(Sdk(), buffer, error);
}

FUZZ_TARGET(platform_decode_dpns_domain)
{
    platform::DpnsName out;
    (void)platform::dpp::DecodeDpnsDomain(Sdk(), buffer, out);
}

FUZZ_TARGET(platform_decode_dashpay_profile)
{
    platform::Profile out;
    (void)platform::dpp::DecodeDashPayProfile(Sdk(), buffer, out);
}

FUZZ_TARGET(platform_decode_contact_request)
{
    platform::ContactRequest out;
    (void)platform::dpp::DecodeDashPayContactRequest(Sdk(), buffer, out);
}
