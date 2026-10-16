// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <test/util/platform_client.h>

#include <util/strencodings.h>

#include <algorithm>

using platform::Identifier;

template <typename T>
void FakePlatformClient::Answer(Call call, std::deque<platform::Result<T>>& scripted, Callback<T> cb,
                                std::unique_ptr<platform::UnlockScope> unlock)
{
    if (shut_down) return;
    calls.push_back(std::move(call));
    platform::Result<T> result;
    if (scripted.empty()) {
        result.status.kind = platform::StatusKind::UNAVAILABLE;
        result.status.message = "nothing scripted";
    } else {
        result = std::move(scripted.front());
        scripted.pop_front();
    }
    m_pending.push_back([cb = std::move(cb), result = std::move(result),
                         unlock = std::shared_ptr<platform::UnlockScope>{std::move(unlock)}]() mutable {
        unlock.reset();
        cb(std::move(result));
    });
}

void FakePlatformClient::getIdentity(const Identifier& id, Callback<platform::Identity> cb)
{
    Answer({"getIdentity", HexStr(id), std::nullopt}, identities, std::move(cb));
}

void FakePlatformClient::getIdentityByPublicKeyHash(const std::array<uint8_t, 20>& pubkey_hash,
                                                    Callback<platform::Identity> cb)
{
    Answer({"getIdentityByPublicKeyHash", HexStr(pubkey_hash), std::nullopt}, identities_by_pubkey_hash, std::move(cb));
}

void FakePlatformClient::resolveName(const std::string& label, Callback<Identifier> cb)
{
    Answer({"resolveName", label, std::nullopt}, resolved_names, std::move(cb));
}

void FakePlatformClient::namesOfIdentity(const Identifier& identity, uint32_t, uint32_t, Callback<platform::Names> cb)
{
    Answer({"namesOfIdentity", HexStr(identity), std::nullopt}, names, std::move(cb));
}

void FakePlatformClient::pendingContests(const Identifier& identity, Callback<std::vector<platform::Contest>> cb)
{
    Answer({"pendingContests", HexStr(identity), std::nullopt}, contests, std::move(cb));
}

void FakePlatformClient::contactRequests(const Identifier& identity, platform::ContactDirection direction, uint64_t,
                                         Callback<std::vector<platform::ContactRequest>> cb)
{
    const bool sent{direction == platform::ContactDirection::SENT};
    Answer({sent ? "sentRequests" : "receivedRequests", HexStr(identity), std::nullopt},
           sent ? sent_requests : received_requests, std::move(cb));
}

void FakePlatformClient::registerIdentity(std::vector<platform::IdentityPublicKey>, platform::AssetLockProof proof,
                                          platform::Signing signing, Callback<platform::Identity> cb)
{
    if (!shut_down) proofs.push_back(std::move(proof));
    Answer({"registerIdentity", "", signing.identity_index}, registered_identities, std::move(cb),
           std::move(signing.unlock));
}

void FakePlatformClient::topUpIdentity(const Identifier& identity, platform::AssetLockProof proof,
                                       wallet::TopupFundingKey, platform::Signing signing, Callback<uint64_t> cb)
{
    if (!shut_down) proofs.push_back(std::move(proof));
    Answer({"topUpIdentity", HexStr(identity), signing.identity_index}, top_ups, std::move(cb), std::move(signing.unlock));
}

void FakePlatformClient::registerName(const std::string& label, const Identifier& identity, uint32_t key_id,
                                      platform::Signing signing, Callback<Identifier> cb)
{
    if (!shut_down) name_signers.emplace_back(identity, key_id);
    Answer({"registerName", label, signing.identity_index}, registered_names, std::move(cb), std::move(signing.unlock));
}

void FakePlatformClient::sendContactRequest(const platform::ContactRequestInput& input, platform::Signing signing,
                                            Callback<Identifier> cb)
{
    if (!shut_down) contact_inputs.push_back(input);
    Answer({"sendContactRequest", HexStr(input.recipient), signing.identity_index}, sent_contact_requests,
           std::move(cb), std::move(signing.unlock));
}

void FakePlatformClient::shutdown()
{
    shut_down = true;
    m_pending.clear();
}

void FakePlatformClient::deliverAll()
{
    while (!m_pending.empty()) {
        deliverNext();
    }
}

void FakePlatformClient::deliverNext()
{
    if (m_pending.empty()) return;
    auto deliver{std::move(m_pending.front())};
    m_pending.pop_front();
    deliver();
}

size_t FakePlatformClient::countCalls(const std::string& method) const
{
    return std::count_if(calls.begin(), calls.end(), [&](const Call& call) { return call.method == method; });
}
