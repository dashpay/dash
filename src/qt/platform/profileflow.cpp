// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/platform/profileflow.h>

#include <logging.h>
#include <platform/client.h>
#include <qt/platform/contactflow.h>
#include <qt/platform/identityflow.h>
#include <qt/platform/platformservice.h>
#include <qt/platform/platformui.h>
#include <qt/walletmodel.h>

#include <algorithm>
#include <utility>
#include <vector>

namespace {
//! Whether a field the user gave is to be published: not empty, and not
//! what Platform already holds.
bool IsNew(const QString& value, const std::string& published)
{
    return !value.isEmpty() && value != QString::fromStdString(published).trimmed();
}
} // namespace

ProfileFlow::ProfileFlow(PlatformService& service, IdentityFlow& identity_flow, const ContactFlow& contact_flow) :
    m_service(service),
    m_identity_flow(identity_flow),
    m_contact_flow(contact_flow)
{
    m_service.addWriter(*this);
    connect(&m_service, &PlatformService::changed, this, &ProfileFlow::update);
    connect(&m_identity_flow, &IdentityFlow::changed, this, &ProfileFlow::update);
    connect(&m_contact_flow, &ContactFlow::changed, this, &ProfileFlow::update);
    update();
}

bool ProfileFlow::ownRead() const
{
    const auto& identity{m_identity_flow.identity()};
    return identity && m_profiles.count(identity->id);
}

std::optional<platform::Profile> ProfileFlow::profile(const platform::Identifier& owner) const
{
    const auto it{m_profiles.find(owner)};
    return it == m_profiles.end() ? std::nullopt : it->second;
}

std::optional<platform::Profile> ProfileFlow::own() const
{
    const auto& identity{m_identity_flow.identity()};
    return identity ? profile(identity->id) : std::nullopt;
}

void ProfileFlow::update()
{
    // The identity is forgotten when a gate closes; its profiles go with it.
    const auto& identity{m_identity_flow.identity()};
    // Reading the contacts again reads their profiles again.
    const bool swept{m_contact_flow.swept()};
    if (!swept) {
        if (m_swept) ++m_generation;
        std::erase_if(m_profiles, [&](const auto& profile) { return !identity || profile.first != identity->id; });
    }
    m_swept = swept;
    // A call the client dropped never calls back.
    if (m_call.expired()) {
        if (m_step == Step::SAVING && m_sent) {
            // Only a closed gate drops a write, and the profiles went with
            // the identity: they are read again, and say whether it landed.
            finish();
        }
    }
    read();
    Q_EMIT changed();
}

void ProfileFlow::read()
{
    const auto& identity{m_identity_flow.identity()};
    platform::PlatformClient* client{m_service.client()};
    if (!identity || !client || !m_read_call.expired()) return;
    std::vector<platform::Identifier> owners;
    const auto want = [&](const platform::Identifier& owner) {
        if (owners.size() < platform::MAX_PROFILE_OWNERS && !m_profiles.count(owner) &&
            std::find(owners.begin(), owners.end(), owner) == owners.end()) {
            owners.push_back(owner);
        }
    };
    want(identity->id);
    for (const auto& contact : m_contact_flow.contacts()) {
        want(contact.id);
    }
    if (owners.empty()) return;
    client->getProfiles(owners, PlatformUi::Tracked<std::vector<platform::Profile>>(
                                    m_read_call, [this, owners, generation = m_generation](auto result) {
                                        // Answered: no longer being read.
                                        m_read_call.reset();
                                        // The contacts were read again meanwhile: so are their profiles.
                                        if (generation != m_generation) return update();
                                        if (!result.ok()) {
                                            // Read again with the next change, such as a new client.
                                            m_read_error = PlatformUi::Describe(result.status);
                                            Q_EMIT changed();
                                            return;
                                        }
                                        m_read_error.clear();
                                        for (const platform::Identifier& owner : owners) {
                                            m_profiles[owner] = std::nullopt;
                                        }
                                        for (platform::Profile& profile : result.value) {
                                            const auto it{m_profiles.find(profile.owner)};
                                            if (it != m_profiles.end()) it->second = std::move(profile);
                                        }
                                        update();
                                    }));
}

void ProfileFlow::save(const QString& display_name, const QString& public_message)
{
    if (m_step != Step::IDLE || !ownRead() || !m_service.client()) return;
    if (display_name.toUcs4().size() > MAX_DISPLAY_NAME) {
        return finish(tr("The display name can have at most %1 characters.").arg(MAX_DISPLAY_NAME));
    }
    if (public_message.toUcs4().size() > MAX_PUBLIC_MESSAGE) {
        return finish(tr("The message can have at most %1 characters.").arg(MAX_PUBLIC_MESSAGE));
    }
    const platform::Profile published{own().value_or(platform::Profile{})};
    m_fields = {};
    if (IsNew(display_name, published.display_name)) {
        m_fields.display_name = display_name.toStdString();
    }
    if (IsNew(public_message, published.public_message)) {
        m_fields.public_message = public_message.toStdString();
    }
    if (m_fields.display_name.empty() && m_fields.public_message.empty()) {
        return finish(tr("There is nothing new to publish."));
    }
    m_error.clear();
    publish();
}

void ProfileFlow::publish()
{
    const auto key{m_identity_flow.identity() ? PlatformUi::DocumentSigningKey(*m_identity_flow.identity()) : std::nullopt};
    if (!key) return finish(tr("Your identity has no key DashPay can sign a profile with."));
    m_step = Step::SAVING;
    Q_EMIT changed();
    WalletModel::UnlockContext unlock{m_service.walletModel().requestUnlock()};
    if (!unlock.isValid()) return finish(tr("The wallet stayed locked, so the profile was not saved."));
    // The passphrase dialog may have let another flow start, or closed a gate.
    if (!m_service.othersIdle(*this)) {
        return finish(tr("Another DashPay request is on its way. Try again once it is done."));
    }
    platform::PlatformClient* client{m_service.client()};
    const auto& identity{m_identity_flow.identity()};
    if (!client || !identity || !ownRead()) return finish();

    const bool create{!own()};
    platform::Signing signing{m_service.walletModel().wallet(), IdentityFlow::IDENTITY_INDEX,
                              PlatformUi::KeepUnlocked(std::move(unlock))};
    auto done{m_service.trackWrite<platform::Profile>(
        PlatformUi::Tracked<platform::Profile>(m_call, [this](auto result) { onSaved(std::move(result)); }))};
    m_sent = true;
    if (create) {
        client->createProfile(identity->id, *key, m_fields, std::move(signing), std::move(done));
    } else {
        client->updateProfile(identity->id, *key, m_fields, std::move(signing), std::move(done));
    }
}

void ProfileFlow::onSaved(platform::Result<platform::Profile> result)
{
    const auto& identity{m_identity_flow.identity()};
    if (!result.ok()) {
        LogPrintf("DashPay: saving the profile failed: %s\n", result.status.message);
        // Sent or not, reading it again says.
        if (identity) m_profiles.erase(identity->id);
        const QString why{PlatformUi::Describe(result.status)};
        return finish(why.isEmpty() ? tr("The profile could not be saved.") : why);
    }
    m_profiles[result.value.owner] = std::move(result.value);
    finish();
}

void ProfileFlow::finish(const QString& error)
{
    m_step = Step::IDLE;
    m_sent = false;
    m_error = error;
    m_fields = {};
    read();
    Q_EMIT changed();
}
