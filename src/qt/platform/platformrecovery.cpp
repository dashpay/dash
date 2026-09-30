// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/platform/platformrecovery.h>

#include <hash.h>
#include <interfaces/wallet.h>
#include <logging.h>
#include <platform/client.h>
#include <platform/helpers.h>
#include <platform/walletrecords.h>
#include <pubkey.h>
#include <qt/platform/contactflow.h>
#include <qt/platform/identityflow.h>
#include <qt/platform/platformservice.h>
#include <qt/walletmodel.h>
#include <script/standard.h>
#include <tinyformat.h>
#include <util/strencodings.h>
#include <util/time.h>
#include <wallet/platformtypes.h>

#include <QPointer>
#include <QThread>
#include <QTimer>

#include <algorithm>
#include <chrono>
#include <memory>
#include <set>

using interfaces::Wallet;

namespace {
//! Consecutive proven-absent identity indexes that end the scan (the mobile
//! wallets' shared gap limit).
constexpr uint32_t IDENTITY_GAP_LIMIT{5};
//! Hard cap on probed indexes, bounding a scan whose gap window keeps being
//! reset by finds.
constexpr uint32_t MAX_IDENTITY_INDEX{16};
//! 5x the DIP-15 payment gap of 20: window rebuilt cursors are searched in.
constexpr uint32_t PAY_CURSOR_WINDOW{100};
//! Flow-level cap on paged items collected per query.
constexpr size_t MAX_PAGED_ITEMS{1000};
//! How long the unlock the user gave for a scan lasts. Enough for the
//! identity scan and a usual contact list; a scan still running then goes
//! on locked, and what needs the wallet's keys waits for the next unlock.
constexpr std::chrono::seconds RECOVERY_UNLOCK_WAIT{120};

std::array<uint8_t, 20> PubKeyHash160(const CPubKey& pubkey)
{
    const uint160 hash{Hash160(pubkey)};
    std::array<uint8_t, 20> out;
    std::copy(hash.begin(), hash.end(), out.begin());
    return out;
}

std::string ContactKey(const char* prefix, const platform::Identifier& id) { return prefix + HexStr(id); }

//! The first enabled ECDSA key of `purpose` (and, for AUTHENTICATION,
//! below MASTER) that the wallet derives at the key's id.
std::optional<uint32_t> FindOwnKey(Wallet& wallet, const platform::Identity& identity,
                                   platform::IdentityPublicKey::Purpose purpose)
{
    using Level = platform::IdentityPublicKey::SecurityLevel;
    using Type = platform::IdentityPublicKey::Type;
    for (const auto& key : identity.public_keys) {
        if (key.purpose != purpose || key.type != Type::ECDSA_SECP256K1 || key.disabled_at) continue;
        if (purpose == platform::IdentityPublicKey::Purpose::AUTHENTICATION && key.security_level == Level::MASTER) {
            continue;
        }
        const auto ours{wallet.getPlatformPubKey(wallet::IdentityAuthKey{0, key.id})};
        if (ours && std::equal(key.data.begin(), key.data.end(), ours.value.begin(), ours.value.end())) return key.id;
    }
    return std::nullopt;
}
} // namespace

PlatformRecovery::PlatformRecovery(PlatformService& service, QObject* parent) :
    QObject(parent),
    m_service(service)
{
    // Monotonic, so a wall clock stepped back cannot extend it.
    m_unlock_timer = new QTimer(this);
    m_unlock_timer->setObjectName("recoveryUnlockTimer");
    m_unlock_timer->setSingleShot(true);
    m_unlock_timer->setInterval(RECOVERY_UNLOCK_WAIT);
    connect(m_unlock_timer, &QTimer::timeout, this, [this] {
        LogPrint(BCLog::PLATFORM, "platform-recovery: unlock for the scan expired; locking the wallet again\n");
        m_unlock.reset();
    });
}

PlatformRecovery::~PlatformRecovery()
{
    if (m_rescan_thread) {
        // After stop() the scan has ended and the wallet may be gone.
        if (m_rescan_thread->isRunning()) m_service.walletModel().wallet().abortRescan();
        m_rescan_thread->wait();
        delete m_rescan_thread;
    }
}

void PlatformRecovery::stop()
{
    m_unlock_timer->stop();
    m_unlock.reset();
    if (m_rescan_thread) {
        m_service.walletModel().wallet().abortRescan();
        m_rescan_thread->wait();
    }
}

QString PlatformRecovery::registrationBlocker() const
{
    switch (m_outcome) {
    case Outcome::NO_IDENTITY:
    case Outcome::RESTORED:
        return {};
    case Outcome::NEEDS_UNLOCK:
        return tr("Unlock your wallet so DashPay can check whether your recovery phrase already has a DashPay "
                  "username.");
    case Outcome::UNUSABLE:
        return tr("This recovery phrase already has a DashPay identity. It was created with keys Dash Core can't "
                  "use. Keep using it in the app that created it.");
    case Outcome::FAILED:
        return m_last_error.text;
    case Outcome::PENDING:
        break;
    }
    return tr("Looking for a DashPay username that belongs to this wallet's recovery phrase…");
}

void PlatformRecovery::maybeStart()
{
    if (m_running || m_service.stopped() || m_outcome == Outcome::NO_IDENTITY || m_outcome == Outcome::UNUSABLE) return;
    const bool have_identity{!m_service.readRecord(platform::records::IDENTITY).empty()};
    const bool contacts_owed{!m_service.readRecord(platform::records::RECOVERY_PENDING).empty()};
    if (have_identity && !contacts_owed) {
        setOutcome(Outcome::RESTORED); // local state exists; nothing to recover
        return;
    }
    reset();
    // The MASTER key hashes the scan looks identities up by, derived before
    // any request so the scan itself needs no key.
    Wallet& wallet{m_service.walletModel().wallet()};
    for (uint32_t index = 0; index < (have_identity ? 1 : MAX_IDENTITY_INDEX); ++index) {
        const auto pubkey{wallet.getPlatformPubKey(wallet::IdentityAuthKey{index, 0})};
        if (!pubkey) {
            // Locked or seedless wallet: the next context refresh or unlock
            // retries (the identity's keys come from the seed).
            LogPrint(BCLog::PLATFORM, "platform-recovery: no platform keys yet (wallet locked or no seed)\n");
            m_probe_hashes.clear();
            setOutcome(Outcome::NEEDS_UNLOCK);
            return;
        }
        m_probe_hashes.push_back(PubKeyHash160(pubkey.value));
    }
    if (have_identity) {
        // The identity is restored; only the contact phase is owed.
        const auto id{m_service.myIdentityId()};
        if (!id) {
            setOutcome(Outcome::RESTORED);
            return;
        }
        setRunning(true);
        m_identity.id = *id;
        LogPrintf("platform-recovery: resuming contact restoration for identity %s\n", HexStr(*id));
        collectRequests(/*to_me=*/true, platform::Identifier{}, {});
        return;
    }
    setRunning(true);
    // A failed run stays reported while it is retried.
    if (m_outcome != Outcome::FAILED) setOutcome(Outcome::PENDING);
    LogPrintf("platform-recovery: no local identity record; probing Platform for identities\n");
    probeIndex(0, /*consecutive_absent=*/0);
}

void PlatformRecovery::unlockAndStart()
{
    if (m_running || m_service.stopped()) return;
    m_unlock.emplace(m_service.walletModel().requestUnlock());
    if (m_unlock->isValid()) {
        m_unlock_timer->start();
        maybeStart();
    }
    if (!m_running) {
        m_unlock_timer->stop();
        m_unlock.reset();
    }
}

bool PlatformRecovery::walletLocked() const
{
    return !m_service.walletModel().wallet().getPlatformPubKey(wallet::IdentityAuthKey{0, 0});
}

void PlatformRecovery::probeIndex(uint32_t index, uint32_t consecutive_absent)
{
    // Platform indexes identities by the hash of their MASTER key (key 0).
    QPointer<PlatformRecovery> self{this};
    m_service.client().getIdentityByPublicKeyHash(m_probe_hashes.at(index), [self, index, consecutive_absent](
                                                                                platform::Result<platform::Identity> res) {
        if (!self) return;
        self->m_service.post([self, index, consecutive_absent, res = std::move(res)] {
            if (!self) return;
            if (!res.ok() && !res.provenAbsent()) {
                // A failure is not proof of absence and must never conclude
                // "no identity". Give up for this session; the next start
                // retries the whole probe.
                if (self->m_found_index) {
                    LogPrintf("platform-recovery: index %u unanswered (%s); recovering the identity found\n", index,
                              res.status.message);
                    self->restoreIdentity();
                } else {
                    self->fail(res.status, strprintf("identity scan incomplete (unanswered query at index %u: %s)",
                                                     index, res.status.message));
                }
                return;
            }
            uint32_t next_absent{consecutive_absent};
            if (res.ok()) {
                if (!self->m_found_index) {
                    self->m_found_index = index;
                    self->m_identity = *res.value;
                } else {
                    ++self->m_extra_identities;
                }
                next_absent = 0;
            } else {
                ++next_absent; // proof-verified absence counts toward the gap
            }
            const uint32_t next{index + 1};
            if (next_absent >= IDENTITY_GAP_LIMIT || next >= MAX_IDENTITY_INDEX) {
                if (self->m_found_index) {
                    self->restoreIdentity();
                } else {
                    self->setOutcome(Outcome::NO_IDENTITY);
                    self->finish(false, strprintf("no identity on Platform (proven absent through index %u)", index));
                }
                return;
            }
            self->probeIndex(next, next_absent);
        });
    });
}

void PlatformRecovery::restoreIdentity()
{
    LogPrintf("platform-recovery: identity found at index %u (id=%s)\n", *m_found_index, HexStr(m_identity.id));
    if (m_extra_identities > 0) {
        LogPrintf("platform-recovery: %u additional identities found; recovering only the first\n", m_extra_identities);
    }
    if (*m_found_index != 0) {
        // Every signing/ECDH path in this version derives with identity
        // index 0; a record synthesized for another index would surface an
        // identity that can never sign. Leave the wallet untouched.
        setOutcome(Outcome::UNUSABLE);
        finish(false, strprintf("identity found at index %u; only index 0 recovery is supported", *m_found_index));
        return;
    }
    collectNames(platform::Identifier{}, {});
}

void PlatformRecovery::collectNames(const platform::Identifier& start_after, std::vector<platform::DpnsName> names)
{
    QPointer<PlatformRecovery> self{this};
    m_service.client().namesOfIdentity(
        m_identity.id, start_after,
        [self, names = std::move(names)](platform::Result<platform::Paged<platform::DpnsName>> res) mutable {
            if (!self) return;
            self->m_service.post([self, names = std::move(names), res = std::move(res)]() mutable {
                if (!self) return;
                if (!res.ok() && !res.provenAbsent()) {
                    // Without a proven name answer the record cannot be
                    // synthesized truthfully; leave everything for a retry on
                    // the next start.
                    self->fail(res.status, "name lookup failed: " + res.status.message);
                    return;
                }
                if (res.ok()) {
                    names.insert(names.end(), res.value->items.begin(), res.value->items.end());
                    if (res.value->has_more && names.size() < MAX_PAGED_ITEMS) {
                        self->collectNames(res.value->next_start_after, std::move(names));
                        return;
                    }
                }
                if (self->walletLocked()) {
                    // Locked again (by the user, or the scan's unlock ran
                    // out): the keys can not be compared, so nothing is
                    // concluded and the next unlock scans again.
                    self->setOutcome(Outcome::NEEDS_UNLOCK);
                    self->finish(false, "identity found but the wallet was locked again before its keys were checked");
                    return;
                }
                using Purpose = platform::IdentityPublicKey::Purpose;
                Wallet& wallet{self->m_service.walletModel().wallet()};
                const auto auth_key{FindOwnKey(wallet, self->m_identity, Purpose::AUTHENTICATION)};
                const auto encryption_key{FindOwnKey(wallet, self->m_identity, Purpose::ENCRYPTION)};
                const auto decryption_key{FindOwnKey(wallet, self->m_identity, Purpose::DECRYPTION)};
                if (!auth_key || !encryption_key || !decryption_key) {
                    // Registered by a wallet whose keys this one does not
                    // derive: a record for it would surface an identity that
                    // can neither sign documents nor exchange contacts.
                    self->setOutcome(Outcome::UNUSABLE);
                    self->finish(false,
                                 "identity carries no signing, encryption and decryption keys this wallet holds");
                    return;
                }
                platform::IdentityRecord record;
                record.identity_id = self->m_identity.id;
                record.auth_key_id = *auth_key;
                record.encryption_key_id = *encryption_key;
                record.decryption_key_id = *decryption_key;
                record.started_at = GetTime();
                if (names.empty()) {
                    // Identity without a username: the GUI offers registration
                    // from this state (funded by the identity's credits).
                    record.state = platform::IdentityRecord::State::IDENTITY_CONFIRMED;
                } else {
                    const auto it{std::min_element(names.begin(), names.end(), [](const auto& a, const auto& b) {
                        return a.normalized_label < b.normalized_label;
                    })};
                    record.state = platform::IdentityRecord::State::REGISTERED;
                    record.label = it->label;
                    record.normalized_label = it->normalized_label;
                    record.contested = platform::helpers::IsContestedUsername(it->label);
                }
                // The contact phase is owed until it completes.
                if (!self->m_service.writeRecord(platform::records::RECOVERY_PENDING, {1}) ||
                    !self->m_service.writeRecord(platform::records::IDENTITY, platform::SerializeIdentityRecord(record))) {
                    self->finish(false, "wallet DB write failed");
                    return;
                }
                self->setOutcome(Outcome::RESTORED);
                self->m_service.identityFlow().reload();
                LogPrintf("platform-recovery: identity record restored (state=%s name=%s keys=%u/%u/%u)\n",
                          record.state == platform::IdentityRecord::State::REGISTERED ? "REGISTERED" : "IDENTITY_CONFIRMED",
                          record.normalized_label, *auth_key, *encryption_key, *decryption_key);
                self->collectRequests(/*to_me=*/true, platform::Identifier{}, {});
            });
        });
}

void PlatformRecovery::collectRequests(bool to_me, const platform::Identifier& start_after,
                                       std::vector<platform::ContactRequest> requests)
{
    QPointer<PlatformRecovery> self{this};
    m_service.client().getContactRequests(
        m_identity.id, to_me, /*since_ms=*/0, start_after,
        [self, to_me, requests = std::move(requests)](platform::Result<platform::Paged<platform::ContactRequest>> res) mutable {
            if (!self) return;
            self->m_service.post([self, to_me, requests = std::move(requests), res = std::move(res)]() mutable {
                if (!self) return;
                if (!res.ok() && !res.provenAbsent()) {
                    // Telling an established contact from a bare request needs
                    // proof of BOTH directions; without it nothing is safe to
                    // import. The identity itself is restored; the contacts
                    // stay owed.
                    self->m_contacts_incomplete = true;
                    self->finish(true, "contact requests unavailable; contacts not restored yet");
                    return;
                }
                if (res.ok()) {
                    requests.insert(requests.end(), res.value->items.begin(), res.value->items.end());
                    if (res.value->has_more && requests.size() < MAX_PAGED_ITEMS) {
                        self->collectRequests(to_me, res.value->next_start_after, std::move(requests));
                        return;
                    }
                }
                if (to_me) {
                    // A contact that sent again replaced its payment
                    // addresses: its newest request is the one restored.
                    self->m_incoming = ContactFlow::NewestPerSender(requests);
                    self->collectRequests(/*to_me=*/false, platform::Identifier{}, {});
                    return;
                }
                self->m_outgoing = std::move(requests);
                // Every request we sent is remembered, answered or not, with
                // the time of the first one: the keychain's birth, and what
                // tells Add contact that it was sent.
                for (const auto& sent : self->m_outgoing) {
                    const std::string key{ContactKey(platform::records::CONTACT_OUT_PREFIX, sent.to_user_id)};
                    if (!self->m_service.readRecord(key).empty()) continue;
                    self->m_service.writeRecord(key,
                                                platform::EncodeContactOutRecord(static_cast<int64_t>(sent.created_at / 1000)));
                }
                for (auto& request : self->m_incoming) {
                    const auto& their{request.owner_id};
                    const bool sent{std::any_of(self->m_outgoing.begin(), self->m_outgoing.end(),
                                                [&their](const auto& s) { return s.to_user_id == their; })};
                    if (!sent) continue;
                    if (self->m_service.isEstablished(QString::fromStdString(HexStr(their)))) continue; // done earlier
                    self->m_established.push_back(std::move(request));
                }
                LogPrintf("platform-recovery: %u incoming / %u outgoing requests; restoring %u contact(s)\n",
                          static_cast<unsigned>(self->m_incoming.size()), static_cast<unsigned>(self->m_outgoing.size()),
                          static_cast<unsigned>(self->m_established.size()));
                self->restoreNextContact();
            });
        });
}

void PlatformRecovery::restoreNextContact()
{
    if (m_established.empty()) {
        rebuildPayCursors();
        return;
    }
    const platform::ContactRequest request{std::move(m_established.back())};
    m_established.pop_back();
    QPointer<PlatformRecovery> self{this};
    m_service.client().getIdentity(request.owner_id, [self, request](platform::Result<platform::Identity> res) {
        if (!self) return;
        self->m_service.post([self, request, res = std::move(res)] {
            if (!self) return;
            const std::string id_hex{HexStr(request.owner_id)};
            // A contact skipped for a reason the next run may resolve (an
            // unanswered read, a re-locked wallet) keeps the phase owed; one
            // whose request this wallet can never decrypt does not.
            const auto skip = [self, &id_hex](const std::string& why, bool retry) {
                LogPrintf("platform-recovery: contact %s skipped: %s\n", id_hex, why);
                if (retry) self->m_contacts_incomplete = true;
                self->restoreNextContact();
            };
            if (!res.ok())
                return skip(res.provenAbsent() ? "identity no longer exists" : "identity proof failed",
                            !res.provenAbsent());
            ContactFlow& contacts{self->m_service.contactFlow()};
            QString error;
            const auto their_xpub{contacts.decryptXpub(request, *res.value, error)};
            if (!their_xpub) return skip(error.toStdString(), self->walletLocked());
            // The birth time is that of our own first request to this
            // contact (written above), never the counterparty's document time.
            const auto birth_time{platform::DecodeContactOutRecord(
                self->m_service.readRecord(ContactKey(platform::records::CONTACT_OUT_PREFIX, request.owner_id)))};
            if (!birth_time) return skip("wallet DB write failed", /*retry=*/true);
            if (!contacts.importKeychains(request.owner_id, *their_xpub, *birth_time, error)) {
                return skip(error.toStdString(), /*retry=*/true);
            }
            // Same record shapes ContactFlow::accept()/confirmRequest() write;
            // the username record hydrates through refreshContacts() later.
            self->m_service.writeRecord(ContactKey(platform::records::CONTACT_KEY_PREFIX, request.owner_id),
                                        {their_xpub->begin(), their_xpub->end()});
            self->m_service.writeRecord(ContactKey(platform::records::CONTACT_IN_PREFIX, request.owner_id),
                                        {request.document_id.begin(), request.document_id.end()});
            self->m_restored.push_back({request.owner_id, *their_xpub});
            LogPrintf("platform-recovery: contact %s restored\n", id_hex);
            self->restoreNextContact();
        });
    });
}

void PlatformRecovery::rebuildPayCursors()
{
    Wallet& wallet{m_service.walletModel().wallet()};
    std::set<CScript> wallet_scripts;
    if (!m_restored.empty()) {
        for (const auto& wtx : wallet.getWalletTxs()) {
            for (const auto& out : wtx.tx->vout) {
                wallet_scripts.insert(out.scriptPubKey);
            }
        }
    }
    for (const auto& contact : m_restored) {
        const auto contact_xpub{PlatformService::FriendshipXpubFromCompact(contact.xpub)};
        std::vector<CTxDestination> destinations;
        const auto derive = [&contact_xpub, &destinations](uint32_t index) -> std::optional<CScript> {
            CTxDestination destination;
            if (!wallet::DeriveFriendshipPaymentDestination(contact_xpub, index, destination)) {
                return std::nullopt;
            }
            destinations.push_back(destination);
            return GetScriptForDestination(destination);
        };
        const uint32_t cursor{platform::ComputePaymentCursor(PAY_CURSOR_WINDOW, derive, wallet_scripts)};
        const std::string id_hex{HexStr(contact.identity)};
        m_service.writeRecord(ContactKey(platform::records::CONTACT_PAY_INDEX_PREFIX, contact.identity),
                              platform::EncodePaymentCursor(cursor));
        // What this seed paid the contact before, from any wallet, reads as
        // a payment to them, as a send from here does; a label the user
        // gave the address stays.
        const std::string label{m_service.contactAddressLabel(QString::fromStdString(id_hex)).toStdString()};
        for (const auto& destination : destinations) {
            std::string existing;
            if (!wallet_scripts.count(GetScriptForDestination(destination)) ||
                (wallet.getAddress(destination, &existing, nullptr, nullptr) && !existing.empty())) {
                continue;
            }
            wallet.setAddressBook(destination, label, "send");
        }
        LogPrintf("platform-recovery: contact %s payment cursor rebuilt: %u\n", id_hex, cursor);
    }
    startRescan();
}

void PlatformRecovery::startRescan()
{
    if (m_restored.empty()) {
        finish(true, "identity restored; no contacts to rescan for");
        return;
    }
    // startRescan() blocks its calling thread for the whole scan, so run it
    // on a dedicated thread. The imported friendship descriptors carry their
    // birth time, letting the wallet start from the earliest key birth
    // instead of genesis. The destructor aborts and joins a scan still
    // running at shutdown.
    Wallet* wallet{&m_service.walletModel().wallet()};
    m_rescan_thread = QThread::create([wallet] {
        const auto status{wallet->startRescan(/*from_genesis=*/false)};
        LogPrintf("platform-recovery: rescan finished (status=%d)\n", static_cast<int>(status));
    });
    QPointer<PlatformRecovery> self{this};
    connect(m_rescan_thread, &QThread::finished, this, [self] {
        if (!self) return;
        self->m_rescan_thread->deleteLater();
        self->m_rescan_thread = nullptr;
        self->m_service.refreshContacts();
    });
    m_rescan_thread->start();
    finish(true,
           strprintf("identity and %u contact(s) restored; rescan started", static_cast<unsigned>(m_restored.size())));
}

void PlatformRecovery::reset()
{
    m_probe_hashes.clear();
    m_found_index.reset();
    m_identity = {};
    m_extra_identities = 0;
    m_incoming.clear();
    m_outgoing.clear();
    m_established.clear();
    m_restored.clear();
    m_contacts_incomplete = false;
}

void PlatformRecovery::fail(const platform::Status& status, const std::string& outcome)
{
    setOutcome(Outcome::FAILED);
    m_last_error = PlatformUi::Describe(status, PlatformUi::Context::READ, tr("Look for an existing identity"));
    finish(false, outcome);
}

void PlatformRecovery::finish(bool recovered, const std::string& outcome)
{
    if (recovered && !m_contacts_incomplete) {
        m_service.writeRecord(platform::records::RECOVERY_PENDING, {});
    }
    m_unlock_timer->stop();
    m_unlock.reset();
    LogPrintf("platform-recovery: %s\n", outcome);
    setRunning(false);
    Q_EMIT finished(recovered);
}

void PlatformRecovery::setOutcome(Outcome outcome)
{
    if (m_outcome == outcome) return;
    m_outcome = outcome;
    Q_EMIT stateChanged();
}

void PlatformRecovery::setRunning(bool running)
{
    if (m_running == running) return;
    m_running = running;
    Q_EMIT stateChanged();
}
