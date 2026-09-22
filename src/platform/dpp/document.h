// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_PLATFORM_DPP_DOCUMENT_H
#define BITCOIN_PLATFORM_DPP_DOCUMENT_H

#include <platform/types.h>
#include <span.h>

#include <cstdint>

namespace platform_ffi {
struct PlatformClient;
} // namespace platform_ffi

/**
 * Decoding of stored (platform-serialized) DPNS and DashPay documents into
 * the GUI types. Thin adapters over the Platform SDK bindings, which
 * deserialize with the real rs-dpp against the pinned system data contracts
 * under the protocol version the SDK has seen the network run.
 */
namespace platform::dpp {

//! Decode the GUI-relevant fields of a DPNS `domain` document. Returns false
//! on malformed input.
bool DecodeDpnsDomain(const platform_ffi::PlatformClient& sdk, Span<const uint8_t> doc, DpnsName& out);

//! Decode the GUI-relevant fields of a DashPay `profile` document.
bool DecodeDashPayProfile(const platform_ffi::PlatformClient& sdk, Span<const uint8_t> doc, Profile& out);

//! Decode a DashPay `contactRequest` document.
bool DecodeDashPayContactRequest(const platform_ffi::PlatformClient& sdk, Span<const uint8_t> doc,
                                 ContactRequest& out);

} // namespace platform::dpp

#endif // BITCOIN_PLATFORM_DPP_DOCUMENT_H
