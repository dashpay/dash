// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <platform/helpers.h>

#include <platform/marshal.h>
#include <util/translation.h>

#include <dash/platform/ffi.h>

#include <algorithm>

namespace platform::helpers {

std::string NormalizeLabel(const std::string& label) { return std::string(platform_ffi::normalize_label(label)); }

bool IsValidUsername(const std::string& label) { return platform_ffi::is_valid_username(label); }

bool IsContestedUsername(const std::string& label) { return platform_ffi::is_contested_username(label); }

Identifier SystemContractId(SystemContract which)
{
    // The ids are protocol constants of compiled-in contracts; the bridge
    // only fails for an unknown enumerator, which cannot be passed here.
    return platform_ffi::system_contract_id(static_cast<platform_ffi::SystemContract>(which));
}

uint64_t CreditsPerDuff() { return platform_ffi::credits_per_duff(); }

util::Result<std::array<uint8_t, 69>> Dip15DecryptXpub(const std::array<uint8_t, 32>& shared_secret,
                                                       const std::vector<uint8_t>& ciphertext)
{
    try {
        const platform_ffi::CompactXpub xpub{
            platform_ffi::dip15_decrypt_xpub(shared_secret,
                                             rust::Slice<const uint8_t>{ciphertext.data(), ciphertext.size()})};
        std::array<uint8_t, 69> out;
        auto it{std::copy(xpub.parent_fingerprint.begin(), xpub.parent_fingerprint.end(), out.begin())};
        it = std::copy(xpub.chain_code.begin(), xpub.chain_code.end(), it);
        std::copy(xpub.public_key.begin(), xpub.public_key.end(), it);
        return out;
    } catch (const std::exception& e) {
        return util::Error{Untranslated(e.what())};
    }
}

uint32_t Dip15AccountReferenceFromMac(const std::array<uint8_t, 32>& mac, uint32_t account_index, uint32_t version)
{
    return platform_ffi::dip15_account_reference_from_mac(mac, account_index, version);
}

AccountReference Dip15UnmaskAccountReference(const std::array<uint8_t, 32>& mac, uint32_t reference)
{
    const platform_ffi::AccountRef unmasked{platform_ffi::dip15_unmask_account_reference_from_mac(mac, reference)};
    return {unmasked.version, unmasked.account_index};
}

std::optional<uint32_t> Dip15SelectRecipientKey(const Identity& identity)
{
    try {
        return platform_ffi::dip15_select_recipient_key(marshal::ToFfi(identity));
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

bool Dip15ReceiveKeysAcceptable(IdentityPublicKey::Purpose sender_purpose, IdentityPublicKey::Purpose recipient_purpose,
                                uint32_t recipient_key_id)
{
    return platform_ffi::dip15_receive_keys_acceptable(static_cast<uint8_t>(sender_purpose),
                                                       static_cast<uint8_t>(recipient_purpose), recipient_key_id);
}

} // namespace platform::helpers
