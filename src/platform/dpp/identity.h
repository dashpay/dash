// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_PLATFORM_DPP_IDENTITY_H
#define BITCOIN_PLATFORM_DPP_IDENTITY_H

#include <platform/types.h>
#include <span.h>

#include <optional>
#include <string>

namespace platform_ffi {
struct PlatformClient;
} // namespace platform_ffi

/**
 * Decoding of platform-serialized Identity / IdentityPublicKey objects into
 * the platform::Identity / platform::IdentityPublicKey GUI types. Thin
 * adapters over the Platform SDK bindings, which deserialize with the real
 * rs-dpp.
 */
namespace platform::dpp {

//! Decodes a platform-serialized Identity. Returns std::nullopt and sets
//! error on malformed input.
std::optional<platform::Identity> DecodeIdentity(const platform_ffi::PlatformClient& sdk,
                                                 Span<const uint8_t> bytes, std::string& error);

//! Decodes a standalone platform-serialized IdentityPublicKey.
std::optional<platform::IdentityPublicKey> DecodeIdentityPublicKey(const platform_ffi::PlatformClient& sdk,
                                                                   Span<const uint8_t> bytes,
                                                                   std::string& error);

} // namespace platform::dpp

#endif // BITCOIN_PLATFORM_DPP_IDENTITY_H
