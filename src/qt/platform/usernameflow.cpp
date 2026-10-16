// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/platform/usernameflow.h>

#include <logging.h>
#include <qt/platform/identityflow.h>
#include <qt/platform/platformservice.h>
#include <qt/platform/platformui.h>

#include <algorithm>
#include <utility>

namespace {
//! The header shows the first of the identity's names.
constexpr uint32_t NAMES_PAGE_LIMIT{10};

//! The duffs an asset lock pays to buy `credits`.
CAmount DuffsFor(uint64_t credits)
{
    return static_cast<CAmount>((credits + PlatformUi::CREDITS_PER_DUFF - 1) / PlatformUi::CREDITS_PER_DUFF);
}
} // namespace

UsernameFlow::UsernameFlow(PlatformService& service, IdentityFlow& identity_flow, ConfirmContest confirm_contest) :
    m_service(service),
    m_identity_flow(identity_flow),
    m_confirm_contest(std::move(confirm_contest))
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
        m_contest.reset();
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
    m_short_balance.reset();
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
    // A premium name has no domain until its vote ends, so it is free to join.
    const bool premium{platform::IsContestedUsername(m_label)};
    if (premium && !m_confirm_contest()) return finish();

    m_unlock.emplace(m_service.walletModel().requestUnlock());
    if (!m_unlock->isValid()) return finish(tr("The wallet stayed locked, so no username was registered."));
    // The passphrase dialog may have let another flow start.
    if (!m_service.othersIdle(*this)) {
        return finish(tr("Another DashPay payment is on its way. Try again once it is done."));
    }
    m_step = Step::PREPARING;
    if (!identity) {
        m_identity_flow.create(premium ? PREMIUM_REGISTRATION_AMOUNT : IdentityFlow::REGISTRATION_AMOUNT);
    } else if (premium && identity->balance < PREMIUM_CREDITS) {
        m_short_balance = identity->balance;
        m_identity_flow.topUp(std::max(IdentityFlow::MIN_TOP_UP, DuffsFor(PREMIUM_CREDITS - identity->balance)));
    }
    update();
}

void UsernameFlow::send(const platform::Identity& identity)
{
    platform::PlatformClient* client{m_service.client()};
    // Paused for a rebuild: changed() comes back here.
    if (!client) return;
    const auto key{PlatformUi::DocumentSigningKey(identity)};
    if (!key) return finish(tr("Your identity has no key DashPay can register a username with."));
    // A declined or failed top-up left the balance as it was.
    if (m_short_balance && identity.balance <= *m_short_balance) {
        return finish(tr("Your identity was not topped up, so the username was not registered."));
    }
    platform::Signing signing{m_service.walletModel().wallet(), IdentityFlow::IDENTITY_INDEX,
                              PlatformUi::KeepUnlocked(std::move(*m_unlock))};
    m_unlock.reset();
    m_step = Step::REGISTERING;
    client->registerName(m_label, identity.id, *key, /*contest_fund=*/0, std::move(signing),
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
    if (platform::IsContestedUsername(m_label)) {
        // Not ours until the masternodes award it: read again, it is pending.
        m_names_read = false;
        return finish();
    }
    m_names_read = true;
    m_username = m_label;
    finish();
}

void UsernameFlow::readContest(const std::string& label)
{
    platform::PlatformClient* client{m_service.client()};
    if (!client || !m_contest_call.expired()) return;
    client->contestedVoteState(label, PlatformUi::Tracked<platform::VoteState>(m_contest_call, [this](auto result) {
                                   m_contest.reset();
                                   if (result.ok()) {
                                       m_contest = std::move(result.value);
                                   } else {
                                       m_error = PlatformUi::Describe(result.status);
                                   }
                                   Q_EMIT changed();
                               }));
}
