// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <platform/marshal.h>

#include <algorithm>

namespace platform::marshal {

namespace {

std::vector<uint8_t> Bytes(const rust::Vec<uint8_t>& in) { return {in.begin(), in.end()}; }

rust::Vec<uint8_t> RustBytes(const std::vector<uint8_t>& in)
{
    rust::Vec<uint8_t> out;
    out.reserve(in.size());
    for (const uint8_t byte : in)
        out.push_back(byte);
    return out;
}

bool ValueIsMeaningful(const Status& status)
{
    return status.kind == StatusKind::OK || status.kind == StatusKind::UNSUPPORTED_PROTOCOL_VERSION;
}

template <typename T, typename V, typename Fn>
Result<T> Verified(const V& verified, const Fn& fill)
{
    Result<T> out;
    out.status = FromFfi(verified.status);
    out.metadata = FromFfi(verified.meta);
    if (ValueIsMeaningful(out.status)) out.value = fill();
    return out;
}

template <typename T, typename V>
Result<Paged<T>> VerifiedPage(const V& verified)
{
    return Verified<Paged<T>>(verified, [&] {
        Paged<T> page;
        page.items.reserve(verified.items.size());
        for (const auto& item : verified.items)
            page.items.push_back(FromFfi(item));
        page.next_start_after = verified.page.next_start_after;
        page.has_more = verified.page.has_more;
        return page;
    });
}

} // namespace

Status FromFfi(const platform_ffi::Status& status)
{
    Status out;
    // A kind a newer shell adds is still a failure this build cannot act on.
    const auto kind{static_cast<uint8_t>(status.kind)};
    out.kind = kind <= static_cast<uint8_t>(StatusKind::INTERNAL) ? static_cast<StatusKind>(kind) : StatusKind::INTERNAL;
    out.consensus_code = status.consensus_code;
    out.message = std::string(status.message);
    return out;
}

ResponseMetadata FromFfi(const platform_ffi::Meta& meta)
{
    ResponseMetadata out;
    out.height = meta.height;
    out.core_chain_locked_height = meta.core_chain_locked_height;
    out.time_ms = meta.time_ms;
    out.protocol_version = meta.protocol_version;
    return out;
}

ContractBounds FromFfi(const platform_ffi::ContractBounds& bounds)
{
    ContractBounds out;
    out.kind = static_cast<ContractBounds::Kind>(bounds.kind);
    out.contract_id = bounds.contract_id;
    out.document_type = std::string(bounds.document_type);
    return out;
}

IdentityPublicKey FromFfi(const platform_ffi::IdentityKey& key)
{
    IdentityPublicKey out;
    out.id = key.id;
    out.purpose = static_cast<IdentityPublicKey::Purpose>(key.purpose);
    out.security_level = static_cast<IdentityPublicKey::SecurityLevel>(key.security_level);
    out.type = static_cast<IdentityPublicKey::Type>(key.key_type);
    out.read_only = key.read_only;
    out.data = Bytes(key.data);
    if (key.disabled_at != 0) out.disabled_at = key.disabled_at;
    out.contract_bounds = FromFfi(key.bounds);
    return out;
}

Identity FromFfi(const platform_ffi::Identity& identity)
{
    Identity out;
    out.id = identity.id;
    out.balance = identity.balance;
    out.revision = identity.revision;
    out.public_keys.reserve(identity.keys.size());
    for (const auto& key : identity.keys)
        out.public_keys.push_back(FromFfi(key));
    return out;
}

DpnsName FromFfi(const platform_ffi::DpnsName& name)
{
    DpnsName out;
    out.label = std::string(name.label);
    out.normalized_label = std::string(name.normalized_label);
    out.parent_domain = std::string(name.parent);
    out.identity = name.identity;
    out.document_id = name.document_id;
    out.owner_id = name.owner;
    return out;
}

Profile FromFfi(const platform_ffi::Profile& profile)
{
    Profile out;
    out.document_id = profile.document_id;
    out.owner_id = profile.owner;
    out.revision = profile.revision;
    out.display_name = std::string(profile.display_name);
    out.public_message = std::string(profile.public_message);
    out.avatar_url = std::string(profile.avatar_url);
    out.avatar_hash = Bytes(profile.avatar_hash);
    out.avatar_fingerprint = Bytes(profile.avatar_fingerprint);
    out.core_payment_address = Bytes(profile.core_payment_address);
    out.platform_payment_address = Bytes(profile.platform_payment_address);
    out.shielded_address = Bytes(profile.shielded_address);
    out.created_at = profile.created_at;
    out.updated_at = profile.updated_at;
    return out;
}

ContactRequest FromFfi(const platform_ffi::ContactRequest& request)
{
    ContactRequest out;
    out.document_id = request.document_id;
    out.owner_id = request.owner;
    out.to_user_id = request.to_user_id;
    out.encrypted_public_key = Bytes(request.encrypted_public_key);
    out.sender_key_index = request.sender_key_index;
    out.recipient_key_index = request.recipient_key_index;
    out.account_reference = request.account_reference;
    out.encrypted_account_label = Bytes(request.encrypted_account_label);
    out.auto_accept_proof = Bytes(request.auto_accept_proof);
    out.created_at = request.created_at;
    out.core_height_created_at = request.core_height_created_at;
    return out;
}

ContestedNameState FromFfi(const platform_ffi::ContestedState& state)
{
    ContestedNameState out;
    out.contenders.reserve(state.contenders.size());
    for (const auto& contender : state.contenders) {
        out.contenders.push_back({contender.identity, contender.has_votes ? contender.votes : 0});
    }
    out.abstain_votes = state.abstain;
    out.lock_votes = state.lock;
    switch (state.winner_kind) {
    case platform_ffi::WinnerKind::WonByIdentity:
        out.outcome = ContestedNameState::Outcome::WON;
        out.winner = state.winner;
        break;
    case platform_ffi::WinnerKind::Locked:
        out.outcome = ContestedNameState::Outcome::LOCKED;
        break;
    case platform_ffi::WinnerKind::NoWinner:
        out.outcome = ContestedNameState::Outcome::OPEN;
        break;
    }
    out.ends_at = state.ends_at;
    return out;
}

Built FromFfi(const platform_ffi::Built& built)
{
    Built out;
    out.bytes = Bytes(built.bytes);
    out.hash = uint256{built.hash};
    out.object_id = built.object_id;
    return out;
}

Result<Identity> FromFfi(const platform_ffi::VerifiedIdentity& verified)
{
    return Verified<Identity>(verified, [&] { return FromFfi(verified.value); });
}

Result<uint64_t> FromFfi(const platform_ffi::VerifiedU64& verified)
{
    return Verified<uint64_t>(verified, [&] { return verified.value; });
}

Result<DpnsName> FromFfi(const platform_ffi::VerifiedDpnsName& verified)
{
    return Verified<DpnsName>(verified, [&] { return FromFfi(verified.value); });
}

Result<Paged<DpnsName>> FromFfi(const platform_ffi::VerifiedDpnsNames& verified)
{
    return VerifiedPage<DpnsName>(verified);
}

Result<Profile> FromFfi(const platform_ffi::VerifiedProfile& verified)
{
    return Verified<Profile>(verified, [&] { return FromFfi(verified.value); });
}

Result<Paged<ContactRequest>> FromFfi(const platform_ffi::VerifiedContactRequests& verified)
{
    return VerifiedPage<ContactRequest>(verified);
}

Result<ContestedNameState> FromFfi(const platform_ffi::VerifiedContested& verified)
{
    return Verified<ContestedNameState>(verified, [&] { return FromFfi(verified.value); });
}

platform_ffi::ContractBounds ToFfi(const ContractBounds& bounds)
{
    platform_ffi::ContractBounds out;
    out.kind = static_cast<platform_ffi::BoundsKind>(bounds.kind);
    out.contract_id = bounds.contract_id;
    out.document_type = bounds.document_type;
    return out;
}

platform_ffi::IdentityKey ToFfi(const IdentityPublicKey& key)
{
    platform_ffi::IdentityKey out;
    out.id = key.id;
    out.purpose = static_cast<uint8_t>(key.purpose);
    out.security_level = static_cast<uint8_t>(key.security_level);
    out.key_type = static_cast<uint8_t>(key.type);
    out.read_only = key.read_only;
    out.data = RustBytes(key.data);
    out.disabled_at = key.disabled_at.value_or(0);
    out.bounds = ToFfi(key.contract_bounds);
    return out;
}

platform_ffi::Identity ToFfi(const Identity& identity)
{
    platform_ffi::Identity out;
    out.id = identity.id;
    out.balance = identity.balance;
    out.revision = identity.revision;
    out.keys.reserve(identity.public_keys.size());
    for (const auto& key : identity.public_keys)
        out.keys.push_back(ToFfi(key));
    return out;
}

platform_ffi::Profile ToFfi(const Profile& profile)
{
    platform_ffi::Profile out;
    out.document_id = profile.document_id;
    out.owner = profile.owner_id;
    out.revision = profile.revision;
    out.display_name = profile.display_name;
    out.public_message = profile.public_message;
    out.avatar_url = profile.avatar_url;
    out.avatar_hash = RustBytes(profile.avatar_hash);
    out.avatar_fingerprint = RustBytes(profile.avatar_fingerprint);
    out.core_payment_address = RustBytes(profile.core_payment_address);
    out.platform_payment_address = RustBytes(profile.platform_payment_address);
    out.shielded_address = RustBytes(profile.shielded_address);
    out.created_at = profile.created_at;
    out.updated_at = profile.updated_at;
    return out;
}

platform_ffi::NewIdentityKey ToFfi(const NewIdentityKey& key)
{
    platform_ffi::NewIdentityKey out;
    out.id = key.id;
    out.purpose = static_cast<uint8_t>(key.purpose);
    out.security_level = static_cast<uint8_t>(key.security_level);
    std::copy(key.pubkey.begin(), key.pubkey.end(), out.pubkey.begin());
    out.bounds = ToFfi(key.contract_bounds);
    return out;
}

platform_ffi::AssetLockProofInput ToFfi(const AssetLockProof& proof)
{
    platform_ffi::AssetLockProofInput out;
    out.is_instant = proof.is_instant;
    out.transaction = RustBytes(proof.transaction);
    out.instant_lock = RustBytes(proof.instant_lock);
    out.output_index = proof.output_index;
    out.core_chain_locked_height = proof.core_chain_locked_height;
    out.out_point = proof.out_point;
    return out;
}

platform_ffi::Proxy ToFfi(const std::optional<ProxyConfig>& proxy)
{
    platform_ffi::Proxy out;
    out.kind = 0;
    out.isolate = false;
    if (!proxy) return out;
    if (const auto* service{std::get_if<CService>(&proxy->address)}) {
        out.kind = 1;
        out.address = service->ToStringAddrPort();
    } else {
        out.kind = 2;
        out.address = std::get<std::string>(proxy->address);
    }
    out.isolate = proxy->randomize_credentials;
    return out;
}

} // namespace platform::marshal
