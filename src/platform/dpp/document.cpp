// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <platform/dpp/document.h>

#include <dash/platform/ffi.h>

#include <algorithm>

namespace platform::dpp {

namespace {

rust::Slice<const uint8_t> ToSlice(Span<const uint8_t> data)
{
    return {data.data(), data.size()};
}

bool CopyId(const rust::Vec<uint8_t>& bytes, Identifier& out)
{
    if (bytes.size() != out.size()) return false;
    std::copy(bytes.begin(), bytes.end(), out.begin());
    return true;
}

} // namespace

bool DecodeDpnsDomain(Span<const uint8_t> doc, DpnsName& out)
{
    try {
        const platform_ffi::FfiDpnsName decoded{platform_ffi::decode_dpns_domain(ToSlice(doc))};
        DpnsName name;
        name.label = std::string{decoded.label};
        name.normalized_label = std::string{decoded.normalized_label};
        name.parent_domain = std::string{decoded.parent_domain};
        if (!CopyId(decoded.identity, name.identity) || !CopyId(decoded.document_id, name.document_id)) {
            return false;
        }
        out = std::move(name);
        return true;
    } catch (const rust::Error&) {
        return false;
    }
}

bool DecodeDashPayProfile(Span<const uint8_t> doc, Profile& out)
{
    try {
        const platform_ffi::FfiProfile decoded{platform_ffi::decode_dashpay_profile(ToSlice(doc))};
        Profile profile;
        if (!CopyId(decoded.document_id, profile.document_id) || !CopyId(decoded.owner_id, profile.owner_id)) {
            return false;
        }
        profile.display_name = std::string{decoded.display_name};
        profile.public_message = std::string{decoded.public_message};
        profile.avatar_url = std::string{decoded.avatar_url};
        profile.avatar_hash.assign(decoded.avatar_hash.begin(), decoded.avatar_hash.end());
        profile.avatar_fingerprint.assign(decoded.avatar_fingerprint.begin(),
                                          decoded.avatar_fingerprint.end());
        profile.created_at = decoded.created_at;
        profile.updated_at = decoded.updated_at;
        profile.revision = decoded.revision;
        out = std::move(profile);
        return true;
    } catch (const rust::Error&) {
        return false;
    }
}

bool DecodeDashPayContactRequest(Span<const uint8_t> doc, ContactRequest& out)
{
    try {
        const platform_ffi::FfiContactRequest decoded{
            platform_ffi::decode_contact_request(ToSlice(doc))};
        ContactRequest contact;
        if (!CopyId(decoded.owner_id, contact.owner_id) ||
            !CopyId(decoded.to_user_id, contact.to_user_id) ||
            !CopyId(decoded.document_id, contact.document_id)) {
            return false;
        }
        contact.encrypted_public_key.assign(decoded.encrypted_public_key.begin(),
                                            decoded.encrypted_public_key.end());
        contact.sender_key_index = decoded.sender_key_index;
        contact.recipient_key_index = decoded.recipient_key_index;
        contact.account_reference = decoded.account_reference;
        contact.encrypted_account_label.assign(decoded.encrypted_account_label.begin(),
                                               decoded.encrypted_account_label.end());
        contact.core_height_created_at = decoded.core_height_created_at;
        contact.created_at = decoded.created_at;
        out = std::move(contact);
        return true;
    } catch (const rust::Error&) {
        return false;
    }
}

} // namespace platform::dpp
