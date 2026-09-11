// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/platform/platformrecovery.h>

#include <hash.h>
#include <interfaces/wallet.h>
#include <logging.h>
#include <platform/client.h>
#include <platform/statetransitions.h>
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

#include <algorithm>
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

std::array<uint8_t, 20> PubKeyHash160(const CPubKey& pubkey)
{
    const uint160 hash{Hash160(pubkey)};
    std::array<uint8_t, 20> out;
    std::copy(hash.begin(), hash.end(), out.begin());
    return out;
}
} // namespace

PlatformRecovery::PlatformRecovery(PlatformService& service, QObject* parent) :
    QObject(parent),
    m_service(service)
{
}

PlatformRecovery::~PlatformRecovery()
{
    if (m_rescan_thread) {
        m_service.walletModel().wallet().abortRescan();
        m_rescan_thread->wait();
        delete m_rescan_thread;
    }
}

void PlatformRecovery::maybeStart()
{
    if (m_attempted) return;
    if (!m_service.readRecord("identity/0").empty()) {
        m_attempted = true; // local state exists; nothing to recover
        return;
    }
    if (!m_service.walletModel().wallet().getPlatformPubKey(wallet::IdentityAuthKey{0, 0})) {
        // Locked or seedless wallet: not marked attempted, so the next
        // context refresh retries (the wallet may be unlocked by then).
        LogPrint(BCLog::QT, "platform-recovery: cannot derive platform keys yet (wallet locked or no seed)\n");
        return;
    }
    m_attempted = true;
    LogPrintf("platform-recovery: no local identity record; probing Platform for identities\n");
    probeIndex(0, /*consecutive_absent=*/0);
}

void PlatformRecovery::probeIndex(uint32_t index, uint32_t consecutive_absent)
{
    // MASTER key (key 0) of identity `index`, whose hash Platform indexes.
    const auto pubkey{m_service.walletModel().wallet().getPlatformPubKey(wallet::IdentityAuthKey{index, 0})};
    if (!pubkey) {
        finish(false, "identity scan aborted: key derivation failed (wallet re-locked?)");
        return;
    }
    QPointer<PlatformRecovery> self{this};
    m_service.client().getIdentityByPublicKeyHash(PubKeyHash160(pubkey.value),
        [self, index, consecutive_absent](platform::Result<std::optional<platform::Identity>> res) {
        if (!self) return;
        self->m_service.post([self, index, consecutive_absent, res = std::move(res)] {
            if (!self) return;
            if (!res.ok()) {
                // A network failure is not proof of absence and must never
                // conclude "no identity". Give up for this session; the next
                // start retries the whole probe.
                if (self->m_found_index) {
                    LogPrintf("platform-recovery: scan unanswered at index %u (%s); recovering the identity already found\n",
                              index, res.error);
                    self->restoreIdentity();
                } else {
                    self->finish(false, strprintf("identity scan incomplete (unanswered query at index %u: %s)",
                                                  index, res.error));
                }
                return;
            }
            uint32_t next_absent{consecutive_absent};
            if (res.value->has_value()) {
                if (!self->m_found_index) {
                    self->m_found_index = index;
                    self->m_identity_id = (**res.value).id;
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
                    self->finish(false, strprintf("no identity on Platform (proven absent through index %u)",
                                                  index));
                }
                return;
            }
            self->probeIndex(next, next_absent);
        });
    });
}

void PlatformRecovery::restoreIdentity()
{
    LogPrintf("platform-recovery: identity found at index %u (id=%s)\n", *m_found_index,
              HexStr(m_identity_id));
    if (m_extra_identities > 0) {
        LogPrintf("platform-recovery: %u additional identities found; recovering only the first\n",
                  m_extra_identities);
    }
    if (*m_found_index != 0) {
        // Every signing/ECDH path in this version derives with identity
        // index 0; a record synthesized for another index would surface an
        // identity that can never sign. Leave the wallet untouched.
        finish(false, strprintf("identity found at index %u; only index 0 recovery is supported",
                                *m_found_index));
        return;
    }
    QPointer<PlatformRecovery> self{this};
    m_service.client().namesOfIdentity(m_identity_id, [self](platform::Result<std::vector<platform::DpnsName>> res) {
        if (!self) return;
        self->m_service.post([self, res = std::move(res)] {
            if (!self) return;
            if (!res.ok()) {
                // Without a proven name answer the record cannot be
                // synthesized truthfully; leave everything for a retry on
                // the next start.
                self->finish(false, "name lookup failed: " + res.error);
                return;
            }
            platform::IdentityRecord record;
            record.identity_id = self->m_identity_id;
            record.started_at = GetTime();
            if (res.value->empty()) {
                // Identity without a username: the GUI offers registration
                // from this state (funded by the identity's credits).
                record.state = platform::IdentityRecord::State::IDENTITY_CONFIRMED;
            } else {
                const auto it{std::min_element(res.value->begin(), res.value->end(),
                    [](const auto& a, const auto& b) { return a.normalized_label < b.normalized_label; })};
                record.state = platform::IdentityRecord::State::REGISTERED;
                record.label = it->label;
                record.normalized_label = it->normalized_label;
                record.contested = platform::st::IsContestedLabel(it->normalized_label);
            }
            if (!self->m_service.writeRecord("identity/0", platform::SerializeIdentityRecord(record))) {
                self->finish(false, "wallet DB write failed");
                return;
            }
            self->m_service.identityFlow().reload();
            LogPrintf("platform-recovery: identity record restored (state=%s name=%s)\n",
                      record.state == platform::IdentityRecord::State::REGISTERED ? "REGISTERED"
                                                                                  : "IDENTITY_CONFIRMED",
                      record.normalized_label);
            self->restoreContacts();
        });
    });
}

void PlatformRecovery::restoreContacts()
{
    const auto my_id{m_service.myIdentityId()};
    if (!my_id) {
        finish(true, "identity restored; contacts skipped (identity id unavailable)");
        return;
    }
    using Requests = std::optional<std::vector<platform::ContactRequest>>;
    auto incoming{std::make_shared<Requests>()};
    auto outgoing{std::make_shared<Requests>()};
    auto remaining{std::make_shared<int>(2)};
    QPointer<PlatformRecovery> self{this};
    const auto step = [self, incoming, outgoing, remaining] {
        if (--*remaining != 0 || !self) return;
        if (!*incoming || !*outgoing) {
            // Telling an established contact from a bare request needs proof
            // of BOTH directions; without it nothing is safe to import. The
            // identity itself is already restored.
            self->finish(true, "contact requests unavailable; contacts not restored");
            return;
        }
        for (auto& request : **incoming) {
            const auto& their{request.owner_id};
            if (std::any_of((*outgoing)->begin(), (*outgoing)->end(),
                            [&their](const auto& sent) { return sent.to_user_id == their; })) {
                self->m_established.push_back(std::move(request));
            }
        }
        LogPrintf("platform-recovery: %u incoming / %u outgoing requests; %u established contact(s)\n",
                  static_cast<unsigned>((*incoming)->size()), static_cast<unsigned>((*outgoing)->size()),
                  static_cast<unsigned>(self->m_established.size()));
        self->restoreNextContact();
    };
    const auto collect = [this, self, my_id, step](bool to_me, std::shared_ptr<Requests> store) {
        m_service.client().getContactRequests(*my_id, to_me, /*since_ms=*/0,
            [self, step, store](platform::Result<std::vector<platform::ContactRequest>> res) {
            if (!self) return;
            self->m_service.post([step, store, res = std::move(res)] {
                if (res.ok()) *store = std::move(*res.value);
                step();
            });
        });
    };
    collect(/*to_me=*/true, incoming);
    collect(/*to_me=*/false, outgoing);
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
    m_service.client().getIdentity(request.owner_id,
        [self, request](platform::Result<std::optional<platform::Identity>> res) {
        if (!self) return;
        self->m_service.post([self, request, res = std::move(res)] {
            if (!self) return;
            const std::string id_hex{HexStr(request.owner_id)};
            const auto skip = [self, &id_hex](const std::string& why) {
                LogPrintf("platform-recovery: contact %s skipped: %s\n", id_hex, why);
                self->restoreNextContact();
            };
            if (!res.ok() || !res.value->has_value()) return skip("identity proof failed");
            // The sender key referenced by the request performs the ECDH; a
            // key disabled after the friendship was made still decrypts.
            const auto& keys{(**res.value).public_keys};
            const auto it{std::find_if(keys.begin(), keys.end(), [&request](const auto& key) {
                return key.id == request.sender_key_index &&
                       key.type == platform::IdentityPublicKey::Type::ECDSA_SECP256K1;
            })};
            if (it == keys.end()) return skip("sender encryption key unavailable");
            ContactFlow& contacts{self->m_service.contactFlow()};
            std::vector<uint8_t> their_xpub;
            if (!contacts.decryptXpub(request.recipient_key_index, it->data,
                                      request.encrypted_public_key, their_xpub)) {
                return skip("could not decrypt the contact request");
            }
            // The friendship chain cannot predate the request document; its
            // created_at bounds the imported descriptor's rescan.
            QString import_error;
            const int64_t creation_time{static_cast<int64_t>(request.created_at / 1000)};
            if (!contacts.importKeychains(request.owner_id, their_xpub, creation_time, import_error)) {
                return skip(import_error.toStdString());
            }
            // Same record shapes ContactFlow::accept()/confirmRequest() write;
            // the username record hydrates through refreshContacts() later.
            self->m_service.writeRecord("contact/key/" + id_hex, their_xpub);
            self->m_service.writeRecord("contact/in/" + id_hex,
                                        std::vector<unsigned char>(request.document_id.begin(),
                                                                   request.document_id.end()));
            self->m_service.writeRecord("contact/out/" + id_hex, {1});
            self->m_restored.push_back({request.owner_id, their_xpub});
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
        const wallet::FriendshipXpub contact_xpub{
            CPubKey{contact.xpub.begin(), contact.xpub.begin() + 33},
            uint256{std::vector<uint8_t>(contact.xpub.begin() + 33, contact.xpub.end())}};
        const auto derive = [&contact_xpub](uint32_t index) -> std::optional<CScript> {
            CTxDestination destination;
            if (!wallet::DeriveFriendshipPaymentDestination(contact_xpub, index, destination)) {
                return std::nullopt;
            }
            return GetScriptForDestination(destination);
        };
        const uint32_t cursor{platform::ComputePaymentCursor(PAY_CURSOR_WINDOW, derive, wallet_scripts)};
        const std::string id_hex{HexStr(contact.identity)};
        m_service.writeRecord("contact/pay-index/" + id_hex, platform::EncodePaymentCursor(cursor));
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
    // startRescan() blocks its calling thread for the whole scan (see
    // RescanWalletActivity), so run it on a dedicated thread. The imported
    // friendship descriptors carry creation_time, letting the wallet start
    // from the earliest key birth instead of genesis. The destructor aborts
    // and joins a scan still running at shutdown.
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
    finish(true, strprintf("identity and %u contact(s) restored; rescan started",
                           static_cast<unsigned>(m_restored.size())));
}

void PlatformRecovery::finish(bool recovered, const std::string& outcome)
{
    LogPrintf("platform-recovery: %s\n", outcome);
    Q_EMIT finished(recovered);
}
