// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <platform/dpp/identity.h>

#include <dash/platform/ffi.h>

#include <algorithm>

namespace platform::dpp {

namespace {

platform::IdentityPublicKey KeyFromFfi(const platform_ffi::FfiIdentityKey& key)
{
    platform::IdentityPublicKey out;
    out.id = key.id;
    out.purpose = static_cast<platform::IdentityPublicKey::Purpose>(key.purpose);
    out.security_level = static_cast<platform::IdentityPublicKey::SecurityLevel>(key.security_level);
    out.type = static_cast<platform::IdentityPublicKey::Type>(key.key_type);
    out.read_only = key.read_only;
    out.data.assign(key.data.begin(), key.data.end());
    out.disabled_at = key.has_disabled_at ? std::optional<uint64_t>{key.disabled_at} : std::nullopt;
    return out;
}

} // namespace

std::optional<platform::Identity> DecodeIdentity(Span<const uint8_t> bytes, std::string& error)
{
    try {
        const platform_ffi::FfiIdentity decoded{
            platform_ffi::decode_identity(rust::Slice<const uint8_t>{bytes.data(), bytes.size()})};
        if (decoded.id.size() != std::tuple_size_v<Identifier>) {
            error = "bridge returned a malformed identity id";
            return std::nullopt;
        }
        platform::Identity identity;
        std::copy(decoded.id.begin(), decoded.id.end(), identity.id.begin());
        identity.balance = decoded.balance;
        identity.revision = decoded.revision;
        identity.public_keys.reserve(decoded.keys.size());
        for (const platform_ffi::FfiIdentityKey& key : decoded.keys) {
            identity.public_keys.push_back(KeyFromFfi(key));
        }
        return identity;
    } catch (const rust::Error& e) {
        error = e.what();
        return std::nullopt;
    }
}

std::optional<platform::IdentityPublicKey> DecodeIdentityPublicKey(Span<const uint8_t> bytes,
                                                                   std::string& error)
{
    try {
        return KeyFromFfi(platform_ffi::decode_identity_public_key(
            rust::Slice<const uint8_t>{bytes.data(), bytes.size()}));
    } catch (const rust::Error& e) {
        error = e.what();
        return std::nullopt;
    }
}

} // namespace platform::dpp
