// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/platform/usernameflow.h>

#include <logging.h>
#include <qt/platform/identityflow.h>
#include <qt/platform/platformservice.h>
#include <qt/platform/platformui.h>

#include <utility>

namespace {
//! The header shows the first of the identity's names.
constexpr uint32_t NAMES_PAGE_LIMIT{10};
} // namespace

UsernameFlow::UsernameFlow(PlatformService& service, IdentityFlow& identity_flow) :
    m_service(service),
    m_identity_flow(identity_flow)
{
    m_service.addWriter(*this);
    connect(&m_service, &PlatformService::changed, this, &UsernameFlow::update);
    connect(&m_identity_flow, &IdentityFlow::changed, this, &UsernameFlow::update);
    update();
}

void UsernameFlow::update()
{
    // The identity flow forgets the identity when a gate closes, which is
    // also what drops a write: once the identity is found again, its names
    // say whether a dropped registration landed.
    if (!m_identity_flow.identity()) {
        m_names_read = false;
        m_username.reset();
    }
    // A call the client dropped never calls back.
    if (m_step == Step::CHECKING && m_call.expired()) {
        resolve();
    } else if (m_step == Step::REGISTERING && m_call.expired()) {
        finish();
    } else if (m_step == Step::PREPARING && m_identity_flow.step() == IdentityFlow::Step::IDLE) {
        if (m_identity_flow.identity()) {
            send(*m_identity_flow.identity());
        } else {
            // Creating it failed or was declined; the identity flow says why.
            finish();
        }
    }
    readNames();
    Q_EMIT changed();
}

void UsernameFlow::finish(const QString& error)
{
    m_step = Step::IDLE;
    m_unlock.reset();
    m_error = error;
    readNames();
    Q_EMIT changed();
}

void UsernameFlow::readNames()
{
    const auto& identity{m_identity_flow.identity()};
    platform::PlatformClient* client{m_service.client()};
    if (m_step != Step::IDLE || m_names_read || !identity || !m_names_call.expired() || !client) return;
    client->namesOfIdentity(identity->id, NAMES_PAGE_LIMIT, /*max_pages=*/1,
                            PlatformUi::Tracked<platform::Names>(m_names_call, [this](auto result) {
                                if (!result.ok()) {
                                    LogPrintf("DashPay: reading the identity's usernames failed: %s\n",
                                              result.status.message);
                                }
                                m_names_read = true;
                                m_username.reset();
                                if (result.ok() && !result.value.names.empty()) {
                                    m_username = result.value.names.front().label;
                                }
                                Q_EMIT changed();
                            }));
}

void UsernameFlow::registerName(const std::string& label)
{
    if (m_step != Step::IDLE || !m_service.client()) return;
    if (!platform::IsValidUsername(label)) {
        return finish(tr("A username has 3 to 63 letters, digits and hyphens, starts and ends with a letter or "
                         "digit, and has no two hyphens in a row."));
    }
    // Contested names need a masternode vote; registering them comes later.
    if (platform::IsContestedUsername(label)) {
        return finish(tr("This is a premium username, which a later version of Dash Core will register. Add a digit "
                         "from 2 to 9, or use 20 characters or more."));
    }
    m_label = label;
    m_step = Step::CHECKING;
    m_error.clear();
    resolve();
    Q_EMIT changed();
}

void UsernameFlow::resolve()
{
    platform::PlatformClient* client{m_service.client()};
    if (!client) return finish();
    client->resolveName(m_label, PlatformUi::Tracked<platform::Identifier>(m_call, [this](auto result) {
                            onResolved(std::move(result));
                        }));
}

void UsernameFlow::onResolved(platform::Result<platform::Identifier> result)
{
    const auto& identity{m_identity_flow.identity()};
    if (result.ok()) {
        if (identity && result.value == identity->id) {
            m_username = m_label;
            return finish();
        }
        return finish(tr("“%1” is taken.").arg(QString::fromStdString(m_label)));
    }
    if (result.status.kind != platform::StatusKind::NOT_FOUND) return finish(PlatformUi::Describe(result.status));

    m_unlock.emplace(m_service.walletModel().requestUnlock());
    if (!m_unlock->isValid()) return finish(tr("The wallet stayed locked, so no username was registered."));
    // The passphrase dialog may have let another flow start.
    if (!m_service.othersIdle(*this)) {
        return finish(tr("Another DashPay payment is on its way. Try again once it is done."));
    }
    m_step = Step::PREPARING;
    if (!m_identity_flow.identity()) m_identity_flow.create();
    update();
}

void UsernameFlow::send(const platform::Identity& identity)
{
    platform::PlatformClient* client{m_service.client()};
    // Paused for a rebuild: changed() comes back here.
    if (!client) return;
    const auto key{PlatformUi::DocumentSigningKey(identity)};
    if (!key) return finish(tr("Your identity has no key DashPay can register a username with."));
    platform::Signing signing{m_service.walletModel().wallet(), IdentityFlow::IDENTITY_INDEX,
                              PlatformUi::KeepUnlocked(std::move(*m_unlock))};
    m_unlock.reset();
    m_step = Step::REGISTERING;
    client->registerName(m_label, identity.id, *key, std::move(signing),
                         m_service.trackWrite<platform::Identifier>(PlatformUi::Tracked<platform::Identifier>(
                             m_call, [this](auto result) { onRegistered(std::move(result)); })));
}

void UsernameFlow::onRegistered(platform::Result<platform::Identifier> result)
{
    if (!result.ok()) {
        LogPrintf("DashPay: username registration failed: %s\n", result.status.message);
        // It may have landed all the same: the names, read again, say.
        m_names_read = false;
        return finish(PlatformUi::Describe(result.status));
    }
    m_names_read = true;
    m_username = m_label;
    finish();
}
