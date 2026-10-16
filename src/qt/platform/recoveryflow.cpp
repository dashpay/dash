// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/platform/recoveryflow.h>

#include <crypto/common.h>
#include <interfaces/wallet.h>
#include <logging.h>
#include <qt/platform/contactflow.h>
#include <qt/platform/identityflow.h>
#include <qt/platform/platformservice.h>
#include <qt/platform/platformui.h>
#include <qt/platform/usernameflow.h>
#include <script/standard.h>
#include <uint256.h>
#include <wallet/platformtypes.h>
#include <wallet/wallet.h>

#include <QThread>

#include <algorithm>
#include <atomic>
#include <limits>
#include <utility>
#include <vector>

namespace {
//! The wallet record of the rescan owed: the earliest birth time, LE64.
constexpr const char* RESCAN_OWED_RECORD{"dashpay/rescan-owed"};

std::optional<int64_t> RescanOwed(interfaces::Wallet& wallet)
{
    const auto records{wallet.getPlatformData(RESCAN_OWED_RECORD)};
    const auto it{records.find(RESCAN_OWED_RECORD)};
    if (it == records.end() || it->second.size() != 8) return std::nullopt;
    return static_cast<int64_t>(ReadLE64(it->second.data()));
}
} // namespace

RecoveryFlow::RecoveryFlow(PlatformService& service, IdentityFlow& identity_flow, UsernameFlow& username_flow,
                           ContactFlow& contact_flow) :
    m_service(service),
    m_identity_flow(identity_flow),
    m_username_flow(username_flow),
    m_contact_flow(contact_flow)
{
    m_service.addWriter(*this);
    connect(&m_service, &PlatformService::changed, this, &RecoveryFlow::update);
    connect(&m_username_flow, &UsernameFlow::changed, this, &RecoveryFlow::update);
    // The contact flow follows the service and the identity flow: once it
    // has, the reads it needs have started.
    connect(&m_contact_flow, &ContactFlow::changed, this, [this] {
        update();
        found();
    });
    rescan();
    update();
}

RecoveryFlow::~RecoveryFlow()
{
    if (!m_rescan) return;
    // Still owed: the next load rescans. Until the scan has reserved the
    // wallet, reserving resets the abort.
    while (!m_rescan->isFinished()) {
        m_service.walletModel().wallet().abortRescan();
        m_rescan->wait(100);
    }
    delete m_rescan;
}

void RecoveryFlow::find()
{
    if (m_step != Step::IDLE) return;
    // Keeps other writes out while the passphrase dialog runs its event loop.
    m_step = Step::FINDING;
    m_error.clear();
    Q_EMIT changed();
    WalletModel::UnlockContext unlock{m_service.walletModel().requestUnlock()};
    if (!unlock.isValid()) return finish(tr("The wallet stayed locked, so DashPay could not look for your identity."));
    if (!m_service.othersIdle(*this)) {
        return finish(tr("Another DashPay request is on its way. Try again once it is done."));
    }
    if (!m_service.consented() && !m_service.start()) {
        return finish(tr("The wallet could not save that DashPay is started."));
    }
    // Held until the contacts are read, so their chains are imported under it.
    m_unlock.emplace(std::move(unlock));
    m_identity_flow.lookUp();
    update();
    found();
}

void RecoveryFlow::finish(const QString& error)
{
    m_step = Step::IDLE;
    m_unlock.reset();
    m_error = error;
    Q_EMIT changed();
}

void RecoveryFlow::update()
{
    if (!m_identity_flow.identity()) {
        m_pending_read = false;
        m_pending_name.reset();
        m_ensured.clear();
    }
    readPendingName();
    ensureKeychains();
    Q_EMIT changed();
}

void RecoveryFlow::found()
{
    if (m_step != Step::FINDING || !m_unlock) return;
    // A lookup the client dropped is made again, and has not failed.
    const bool looking_up{m_identity_flow.step() != IdentityFlow::Step::IDLE ||
                          (!m_identity_flow.lookedUp() && m_identity_flow.error().isEmpty())};
    const bool waiting{looking_up || (m_identity_flow.identity() && !m_contact_flow.swept() && m_contact_flow.reading())};
    if (m_service.gate() == PlatformService::Gate::NONE && waiting) return;
    finish();
}

void RecoveryFlow::readPendingName()
{
    const auto& identity{m_identity_flow.identity()};
    platform::PlatformClient* client{m_service.client()};
    if (m_pending_read || !identity || !client || !m_username_flow.namesRead() || m_username_flow.username() ||
        !m_pending_call.expired()) {
        return;
    }
    const auto read = [this](platform::Result<std::vector<platform::Contest>> result) {
        // Best effort, as the mobile wallets.
        if (!result.ok()) {
            LogPrintf("DashPay: reading the identity's pending usernames failed: %s\n", result.status.message);
        } else if (!result.value.empty()) {
            m_pending_name = result.value.front().normalized_label;
        }
        m_pending_read = true;
        Q_EMIT changed();
    };
    client->pendingContests(identity->id, PlatformUi::Tracked<std::vector<platform::Contest>>(m_pending_call, read));
}

void RecoveryFlow::ensureKeychains()
{
    const auto& identity{m_identity_flow.identity()};
    if (!identity || !m_contact_flow.swept()) return;
    interfaces::Wallet& wallet{m_service.walletModel().wallet()};
    std::vector<wallet::FriendshipKeychainRequest> missing;
    for (const ContactFlow::Contact& contact : m_contact_flow.contacts()) {
        // Every contact we sent a request to can pay us, answered or not.
        if (contact.state == ContactFlow::State::INCOMING || m_ensured.count(contact.id)) continue;
        // Their payments need our chain, which our request carries: none predates it.
        const wallet::FriendshipKeychainRequest request{ContactFlow::ACCOUNT, uint256{identity->id}, uint256{contact.id},
                                                        static_cast<int64_t>(contact.sent_at_ms / 1000)};
        const auto chain{wallet.getFriendshipReceivingXpub(request)};
        CTxDestination first;
        // A locked wallet: "Find my identity" imports them under its passphrase.
        if (!chain || !wallet::DeriveFriendshipPaymentDestination(chain.value, 0, first)) return;
        m_ensured.insert(contact.id);
        if (!wallet.isSpendable(first)) missing.push_back(request);
    }
    if (missing.empty()) return;

    int64_t from{RescanOwed(wallet).value_or(std::numeric_limits<int64_t>::max())};
    for (const auto& request : missing) {
        from = std::min(from, request.birth_time);
    }
    std::vector<unsigned char> owed(8);
    WriteLE64(owed.data(), static_cast<uint64_t>(from));
    // Before the chains exist: a crash after importing one can't lose its rescan.
    if (!wallet.writePlatformData(RESCAN_OWED_RECORD, owed)) {
        m_error = tr("The wallet could not save that it needs a rescan, so your contacts' payments are not "
                     "looked for yet.");
        return;
    }
    m_owed_again = rescanning();
    for (const auto& request : missing) {
        if (!wallet.ensureFriendshipReceivingKeychain(request)) {
            m_error = tr("Dash Core could not add the addresses your DashPay contacts pay you at.");
        }
    }
    rescan();
}

void RecoveryFlow::rescan()
{
    interfaces::Wallet& wallet{m_service.walletModel().wallet()};
    const auto from{RescanOwed(wallet)};
    if (m_rescan || !from) return;
    m_pruned_from = wallet.rescanPrunedFrom(*from);
    if (m_pruned_from) {
        LogPrintf("DashPay: the rescan for contact payments needs blocks from height %d, which are pruned\n",
                  *m_pruned_from);
        Q_EMIT changed();
        return;
    }
    // The scan blocks until it is done, and takes the wallet's rescan reservation.
    auto status{std::make_shared<std::atomic<wallet::RescanStatus>>(wallet::RescanStatus::FAILURE)};
    m_rescan = QThread::create([&wallet, status, from = *from] { status->store(wallet.rescanFromTime(from)); });
    connect(m_rescan, &QThread::finished, this, [this, &wallet, status] {
        m_rescan->deleteLater();
        m_rescan = nullptr;
        if (status->load() != wallet::RescanStatus::SUCCESS) {
            m_owed_again = false;
            m_error = tr("The wallet rescan for payments from your DashPay contacts did not finish. It runs again "
                         "the next time this wallet is loaded.");
        } else if (std::exchange(m_owed_again, false)) {
            return rescan();
        } else {
            wallet.writePlatformData(RESCAN_OWED_RECORD, {});
            m_error.clear();
        }
        Q_EMIT changed();
    });
    m_rescan->start();
    Q_EMIT changed();
}
