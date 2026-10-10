// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <test/util/platform_client.h>

#include <util/strencodings.h>
#include <util/translation.h>

#include <algorithm>

using platform::Built;
using platform::ContactRequest;
using platform::ContestedNameState;
using platform::DpnsName;
using platform::Identifier;
using platform::Identity;
using platform::Paged;
using platform::Profile;
using platform::Result;
using platform::Status;
using platform::StatusKind;

template <typename T>
Result<T> FakePlatformClient::Pop(std::deque<Result<T>>& scripted)
{
    if (needs_endpoints && (endpoint_updates.empty() || endpoint_updates.back().empty())) {
        return platform_test::Failed<T>(StatusKind::UNAVAILABLE, "no evonode endpoints");
    }
    if (scripted.empty()) return platform_test::Failed<T>(StatusKind::UNAVAILABLE, "nothing scripted");
    Result<T> out{std::move(scripted.front())};
    scripted.pop_front();
    return out;
}

util::Result<Built> FakePlatformClient::PopBuild(const platform::SigningOperation& op)
{
    build_kinds.push_back(op.kind());
    build_key_ids.push_back(op.keyIds());
    if (builds.empty()) return util::Error{Untranslated("nothing scripted")};
    util::Result<Built> out{std::move(builds.front())};
    builds.pop_front();
    return out;
}

void FakePlatformClient::resolveName(const std::string& normalized_label, Callback<DpnsName> cb)
{
    calls.push_back({"resolveName", normalized_label});
    cb(Pop(resolve_name));
}

void FakePlatformClient::searchNames(const std::string& prefix, uint32_t limit, const Identifier& start_after,
                                     Callback<Paged<DpnsName>> cb)
{
    calls.push_back({"searchNames", prefix, start_after});
    cb(Pop(search_names));
}

void FakePlatformClient::namesOfIdentity(const Identifier& identity, const Identifier& start_after,
                                         Callback<Paged<DpnsName>> cb)
{
    calls.push_back({"namesOfIdentity", HexStr(identity), start_after});
    cb(Pop(names_of_identity));
}

void FakePlatformClient::getIdentity(const Identifier& id, Callback<Identity> cb)
{
    calls.push_back({"getIdentity", HexStr(id)});
    cb(Pop(identities));
}

void FakePlatformClient::getIdentityByPublicKeyHash(const std::array<uint8_t, 20>& pubkey_hash, Callback<Identity> cb)
{
    calls.push_back({"getIdentityByPublicKeyHash", HexStr(pubkey_hash)});
    cb(Pop(identities_by_pubkey_hash));
}

void FakePlatformClient::getIdentityContractNonce(const Identifier& id, const Identifier& contract_id, Callback<uint64_t> cb)
{
    calls.push_back({"getIdentityContractNonce", HexStr(id) + "/" + HexStr(contract_id)});
    cb(Pop(nonces));
}

void FakePlatformClient::getProfile(const Identifier& owner_id, Callback<Profile> cb)
{
    calls.push_back({"getProfile", HexStr(owner_id)});
    cb(Pop(profiles));
}

void FakePlatformClient::getContactRequests(const Identifier& identity, bool to_me, uint64_t since_ms,
                                            const Identifier& start_after, Callback<Paged<ContactRequest>> cb)
{
    calls.push_back(
        {to_me ? "getContactRequests/to_me" : "getContactRequests/from_me", HexStr(identity), start_after, since_ms});
    cb(Pop(contact_requests));
}

void FakePlatformClient::getContestedNameState(const std::string& normalized_label, Callback<ContestedNameState> cb)
{
    calls.push_back({"getContestedNameState", normalized_label});
    cb(Pop(contested_states));
}

void FakePlatformClient::broadcastStateTransition(const std::vector<uint8_t>& state_transition, BroadcastCallback cb)
{
    calls.push_back({"broadcastStateTransition", HexStr(state_transition)});
    broadcast_bytes.push_back(state_transition);
    Status status;
    if (broadcasts.empty()) {
        status.kind = StatusKind::UNAVAILABLE;
        status.message = "nothing scripted";
    } else {
        status = broadcasts.front();
        broadcasts.pop_front();
    }
    cb(std::move(status));
}

util::Result<Built> FakePlatformClient::buildIdentityCreate(const platform::SigningOperation& op,
                                                            const platform::AssetLockProof& proof,
                                                            const std::vector<platform::NewIdentityKey>& keys)
{
    last_asset_lock_proof = proof;
    last_identity_keys = keys;
    return PopBuild(op);
}

util::Result<Built> FakePlatformClient::buildDpnsPreorder(const platform::SigningOperation& op, const Identifier&,
                                                          uint64_t, const std::string&, const std::array<uint8_t, 32>& salt)
{
    build_salts.push_back(salt);
    return PopBuild(op);
}

util::Result<Built> FakePlatformClient::buildDpnsDomain(const platform::SigningOperation& op, const Identifier&,
                                                        uint64_t, const std::string&, const std::array<uint8_t, 32>& salt)
{
    build_salts.push_back(salt);
    return PopBuild(op);
}

util::Result<Built> FakePlatformClient::buildProfile(const platform::SigningOperation& op, const Identifier&, uint64_t,
                                                     const Profile& existing, const platform::ProfileInput&)
{
    last_profile_existing = existing;
    return PopBuild(op);
}

util::Result<Built> FakePlatformClient::buildContactRequest(const platform::SigningOperation& op, const Identity&,
                                                            const Identity&, uint64_t,
                                                            const platform::ContactRequestInput& input)
{
    last_contact_request_input = input;
    return PopBuild(op);
}

util::Result<uint64_t> FakePlatformClient::contestedVoteFundCredits()
{
    if (!contested_fund_credits) return util::Error{Untranslated("protocol version not verified yet")};
    return *contested_fund_credits;
}

void FakePlatformClient::updateEndpoints(std::vector<platform::Endpoint> endpoints)
{
    endpoint_updates.push_back(std::move(endpoints));
}

void FakePlatformClient::updateQuorumKeys(uint8_t, std::vector<platform::QuorumKey> keys)
{
    quorum_key_updates.push_back(std::move(keys));
}

void FakePlatformClient::updateCoreChainLockedHeight(int32_t height) { chainlock_heights.push_back(height); }

void FakePlatformClient::shutdown() { shut_down = true; }

size_t FakePlatformClient::countCalls(const std::string& method) const
{
    return std::count_if(calls.begin(), calls.end(), [&](const Call& call) { return call.method == method; });
}

namespace platform_test {

Status BroadcastStatus(StatusKind kind, uint32_t consensus_code)
{
    Status status;
    status.kind = kind;
    status.consensus_code = consensus_code;
    return status;
}

Identifier IdentifierFromByte(uint8_t byte)
{
    Identifier id{};
    id.fill(byte);
    return id;
}

} // namespace platform_test
