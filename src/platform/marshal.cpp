// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <platform/marshal.h>

#include <streams.h>
#include <version.h>

#include <algorithm>
#include <string>
#include <variant>

namespace platform::marshal {

namespace {

std::vector<uint8_t> Bytes(const rust::Vec<uint8_t>& bytes) { return {bytes.begin(), bytes.end()}; }

template <typename T>
rust::Vec<uint8_t> ToVec(const T& bytes)
{
    rust::Vec<uint8_t> out;
    out.reserve(bytes.size());
    for (const auto byte : bytes)
        out.push_back(static_cast<uint8_t>(byte));
    return out;
}

template <typename T>
rust::Vec<uint8_t> Serialized(const T& object)
{
    CDataStream stream{SER_NETWORK, PROTOCOL_VERSION};
    stream << object;
    return ToVec(stream);
}

Status FromFfi(const dash_sdk::Status& status)
{
    Status out;
    switch (status.kind) {
    case dash_sdk::StatusKind::Ok:
        out.kind = StatusKind::OK;
        break;
    case dash_sdk::StatusKind::NotFound:
        out.kind = StatusKind::NOT_FOUND;
        break;
    case dash_sdk::StatusKind::Unavailable:
        out.kind = StatusKind::UNAVAILABLE;
        break;
    case dash_sdk::StatusKind::Rejected:
        out.kind = StatusKind::REJECTED;
        break;
    case dash_sdk::StatusKind::Consensus:
        out.kind = StatusKind::CONSENSUS;
        break;
    case dash_sdk::StatusKind::Internal:
        out.kind = StatusKind::INTERNAL;
        break;
    }
    out.consensus_code = status.consensus_code;
    out.message = std::string{status.message};
    return out;
}

IdentityPublicKey FromFfi(const dash_sdk::IdentityKey& key)
{
    IdentityPublicKey out;
    out.id = key.id;
    out.purpose = static_cast<IdentityPublicKey::Purpose>(key.purpose);
    out.security_level = static_cast<IdentityPublicKey::SecurityLevel>(key.security_level);
    out.type = static_cast<IdentityPublicKey::Type>(key.key_type);
    out.read_only = key.read_only;
    out.data = Bytes(key.data);
    out.disabled_at = key.disabled_at;
    return out;
}

DpnsName FromFfi(const dash_sdk::DpnsName& name)
{
    return {std::string{name.label}, std::string{name.normalized_label}, name.owner, name.identity};
}

ContactRequest FromFfi(const dash_sdk::ContactRequest& request)
{
    ContactRequest out;
    out.document_id = request.document_id;
    out.owner = request.owner;
    out.to_user_id = request.to_user_id;
    out.encrypted_public_key = Bytes(request.encrypted_public_key);
    out.sender_key_index = request.sender_key_index;
    out.recipient_key_index = request.recipient_key_index;
    out.account_reference = request.account_reference;
    out.encrypted_account_label = Bytes(request.encrypted_account_label);
    out.auto_accept_proof = Bytes(request.auto_accept_proof);
    out.created_at_ms = request.created_at_ms;
    out.created_at_core_height = request.created_at_core_height;
    return out;
}

template <typename Out, typename In>
std::vector<Out> Each(const rust::Vec<In>& items)
{
    std::vector<Out> out;
    out.reserve(items.size());
    for (const In& item : items)
        out.push_back(FromFfi(item));
    return out;
}

} // namespace

Result<Identity> FromFfi(const dash_sdk::IdentityResult& result)
{
    const auto& identity{result.identity};
    return {FromFfi(result.status),
            {identity.id, identity.balance, identity.revision, Each<IdentityPublicKey>(identity.keys)}};
}

Result<Identifier> FromFfi(const dash_sdk::IdResult& result) { return {FromFfi(result.status), result.id}; }

Result<uint64_t> FromFfi(const dash_sdk::BalanceResult& result) { return {FromFfi(result.status), result.balance}; }

Result<Names> FromFfi(const dash_sdk::NamesResult& result)
{
    return {FromFfi(result.status), {Each<DpnsName>(result.names), result.complete}};
}

Result<std::vector<Contest>> FromFfi(const dash_sdk::ContestsResult& result)
{
    std::vector<Contest> contests;
    contests.reserve(result.contests.size());
    for (const auto& contest : result.contests) {
        contests.push_back({std::string{contest.name}, contest.ends_at_ms});
    }
    return {FromFfi(result.status), std::move(contests)};
}

Result<VoteState> FromFfi(const dash_sdk::VoteStateResult& result)
{
    VoteState state;
    for (const auto& contender : result.state.contenders) {
        state.contenders.push_back({contender.identity, contender.votes});
    }
    state.abstain = result.state.abstain;
    state.lock = result.state.lock;
    state.winner = static_cast<VoteState::Winner>(result.state.winner_kind);
    state.winner_identity = result.state.winner;
    return {FromFfi(result.status), std::move(state)};
}

Result<std::vector<ContactRequest>> FromFfi(const dash_sdk::ContactRequestsResult& result)
{
    return {FromFfi(result.status), Each<ContactRequest>(result.requests)};
}

std::vector<dash_sdk::IdentityKey> ToFfi(const std::vector<IdentityPublicKey>& keys)
{
    std::vector<dash_sdk::IdentityKey> out;
    out.reserve(keys.size());
    for (const IdentityPublicKey& key : keys) {
        dash_sdk::IdentityKey ffi;
        ffi.id = key.id;
        ffi.purpose = static_cast<uint8_t>(key.purpose);
        ffi.security_level = static_cast<uint8_t>(key.security_level);
        ffi.key_type = static_cast<uint8_t>(key.type);
        ffi.read_only = key.read_only;
        ffi.data = ToVec(key.data);
        ffi.disabled_at = key.disabled_at;
        out.push_back(std::move(ffi));
    }
    return out;
}

dash_sdk::AssetLockProof ToFfi(const AssetLockProof& proof)
{
    dash_sdk::AssetLockProof out{};
    if (const auto* instant{std::get_if<InstantAssetLockProof>(&proof)}) {
        out.kind = dash_sdk::AssetLockProofKind::Instant;
        out.transaction = Serialized(*instant->transaction);
        out.instant_lock = ToVec(instant->instant_lock);
        out.output_index = instant->output_index;
    } else {
        const auto& chain{std::get<ChainAssetLockProof>(proof)};
        out.kind = dash_sdk::AssetLockProofKind::Chain;
        out.core_chain_locked_height = chain.core_chain_locked_height;
        const auto out_point{Serialized(chain.out_point)};
        std::copy(out_point.begin(), out_point.end(), out.out_point.begin());
    }
    return out;
}

dash_sdk::ContactRequestInput ToFfi(const ContactRequestInput& input)
{
    dash_sdk::ContactRequestInput out;
    out.sender = input.sender;
    out.recipient = input.recipient;
    out.sender_key_index = input.sender_key_index;
    out.recipient_key_index = input.recipient_key_index;
    out.signing_key_id = input.signing_key_id;
    out.account_reference = input.account_reference;
    out.account_label = input.account_label;
    return out;
}

dash_sdk::Proxy ToFfi(const std::optional<ProxyConfig>& proxy)
{
    dash_sdk::Proxy out;
    out.kind = dash_sdk::ProxyKind::None;
    if (!proxy) return out;
    out.isolate = proxy->randomize_credentials;
    if (const auto* service{std::get_if<CService>(&proxy->address)}) {
        out.kind = dash_sdk::ProxyKind::Tcp;
        out.address = service->ToStringAddrPort();
    } else {
        out.kind = dash_sdk::ProxyKind::Unix;
        out.address = std::get<std::string>(proxy->address);
    }
    return out;
}

} // namespace platform::marshal
