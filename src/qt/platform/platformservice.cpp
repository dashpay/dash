// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/platform/platformservice.h>

#include <chainparams.h>
#include <key_io.h>
#include <logging.h>
#include <netbase.h>
#include <interfaces/node.h>
#include <interfaces/wallet.h>
#include <platform/statetransitions.h>
#include <platform/walletrecords.h>
#include <qt/clientmodel.h>
#include <qt/platform/contactflow.h>
#include <qt/platform/identityflow.h>
#include <qt/platform/platformrecovery.h>
#include <qt/walletmodel.h>
#include <util/strencodings.h>
#include <wallet/platformtypes.h>

#include <univalue.h>

#include <QMetaObject>
#include <QCryptographicHash>
#include <QImage>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPointer>
#include <QUrl>
#include <QVector>

#include <algorithm>
#include <utility>

namespace {
//! Flow advance / retry cadence.
constexpr int TICK_INTERVAL_MS{5'000};
//! Endpoint + quorum key refresh cadence.
constexpr int CONTEXT_INTERVAL_MS{60'000};
constexpr int AVATAR_TIMEOUT_MS{15'000};
constexpr qsizetype MAX_AVATAR_BYTES{2 * 1024 * 1024};
} // namespace

PlatformService::PlatformService(WalletModel& wallet_model, ClientModel& client_model,
                                 std::shared_ptr<platform::PlatformClient> client, QObject* parent) :
    QObject(parent),
    m_wallet_model(wallet_model),
    m_client_model(client_model),
    m_client(std::move(client))
{
    if (auto params{platform::GetParams(Params().NetworkIDString())}) {
        m_params = *params;
    }

    m_identity_flow = std::make_unique<IdentityFlow>(*this, this);
    connect(m_identity_flow.get(), &IdentityFlow::stateChanged, this, &PlatformService::identityStateChanged);
    connect(m_identity_flow.get(), &IdentityFlow::failed, this, &PlatformService::flowFailed);

    m_contact_flow = std::make_unique<ContactFlow>(*this, this);
    connect(m_contact_flow.get(), &ContactFlow::requestSent, this, [this](const QString& id) {
        finishContactRequest(id, true, {});
        refreshContacts();
    });
    connect(m_contact_flow.get(), &ContactFlow::requestFailed, this,
            [this](const QString& id, const QString& error) { finishContactRequest(id, false, error); });

    m_recovery = std::make_unique<PlatformRecovery>(*this, this);
    connect(m_recovery.get(), &PlatformRecovery::finished, this, [this](bool recovered) {
        if (recovered) refreshContacts();
    });

    m_tick_timer = new QTimer(this);
    m_tick_timer->setInterval(TICK_INTERVAL_MS);
    connect(m_tick_timer, &QTimer::timeout, this, [this] { m_identity_flow->advance(); });
    m_tick_timer->start();

    m_context_timer = new QTimer(this);
    m_context_timer->setInterval(CONTEXT_INTERVAL_MS);
    connect(m_context_timer, &QTimer::timeout, this, &PlatformService::updateNodeContext);
    m_context_timer->start();
    updateNodeContext();

    m_identity_flow->resume();
}

PlatformService::~PlatformService()
{
    if (m_client) m_client->shutdown();
}

QString PlatformService::myUsername() const
{
    const auto& rec{m_identity_flow->record()};
    if (rec.state == IdentityFlow::State::REGISTERED) {
        return QString::fromStdString(rec.label);
    }
    return {};
}

std::optional<platform::Identifier> PlatformService::myIdentityId() const
{
    const auto& rec{m_identity_flow->record()};
    if (rec.state >= IdentityFlow::State::IDENTITY_CONFIRMED &&
        rec.state != IdentityFlow::State::FAILED) {
        return rec.identity_id;
    }
    return std::nullopt;
}

void PlatformService::refreshIdentityBalance()
{
    const auto id{myIdentityId()};
    if (!id) return;
    QPointer<PlatformService> self{this};
    m_client->getIdentity(*id, [self](platform::Result<std::optional<platform::Identity>> res) {
        if (!self) return;
        self->post([self, res = std::move(res)] {
            if (!self || !res.ok() || !res.value->has_value()) return;
            Q_EMIT self->identityBalanceLoaded((**res.value).balance);
        });
    });
}

void PlatformService::checkNameAvailability(const QString& name)
{
    const std::string normalized{platform::st::NormalizeLabel(name.toStdString())};
    const bool contested{platform::st::IsContestedLabel(normalized)};
    QPointer<PlatformService> self{this};
    m_client->resolveName(normalized, [self, normalized, contested](platform::Result<std::optional<platform::DpnsName>> res) {
        if (!self) return;
        self->post([self, normalized, contested, res = std::move(res)] {
            if (!self) return;
            if (!res.ok()) {
                Q_EMIT self->nameAvailabilityFailed(QString::fromStdString(normalized),
                                                    QString::fromStdString(res.error));
                return;
            }
            const bool available{!res.value->has_value()};
            if (!available || !contested) {
                Q_EMIT self->nameAvailability(QString::fromStdString(normalized), available, contested);
                return;
            }
            // A contested label absent from the domain tree may still have an
            // active (or locked) vote; only a proven-absent contest makes it
            // truly available.
            self->m_client->getContestedNameState(
                normalized, [self, normalized](platform::Result<platform::ContestedNameState> vote_res) {
                    if (!self) return;
                    self->post([self, normalized, vote_res = std::move(vote_res)] {
                        if (!self) return;
                        if (!vote_res.ok()) {
                            Q_EMIT self->nameAvailabilityFailed(QString::fromStdString(normalized),
                                                                QString::fromStdString(vote_res.error));
                            return;
                        }
                        const bool no_contest{vote_res.value->status ==
                                              platform::ContestedNameState::Status::UNKNOWN};
                        Q_EMIT self->nameAvailability(QString::fromStdString(normalized), no_contest,
                                                      /*contested=*/true);
                    });
                });
        });
    });
}

void PlatformService::checkContestedNameState(const QString& normalized_label)
{
    const std::string normalized{normalized_label.toStdString()};
    QPointer<PlatformService> self{this};
    m_client->getContestedNameState(normalized, [self, normalized](platform::Result<platform::ContestedNameState> res) {
        if (!self) return;
        self->post([self, normalized, res = std::move(res)] {
            if (!self) return;
            if (!res.ok()) {
                Q_EMIT self->contestedNameState(QString::fromStdString(normalized),
                                                platform::ContestedNameState{},
                                                QString::fromStdString(res.error));
                return;
            }
            Q_EMIT self->contestedNameState(QString::fromStdString(normalized), *res.value, QString{});
        });
    });
}

void PlatformService::searchNames(const QString& prefix)
{
    const std::string normalized{platform::st::NormalizeLabel(prefix.toStdString())};
    QPointer<PlatformService> self{this};
    m_client->searchNames(normalized, /*limit=*/25, [self, prefix](platform::Result<std::vector<platform::DpnsName>> res) {
        if (!self) return;
        self->post([self, prefix, res = std::move(res)] {
            if (!self || !res.ok()) return;
            QVector<QPair<QString, QString>> out;
            out.reserve(res.value->size());
            for (const auto& name : *res.value) {
                const QString label{QString::fromStdString(name.label)};
                const QString id{QString::fromStdString(HexStr(name.identity))};
                // searchNames() only returns entries after validating the
                // prefix-query proof, so this is safe contact metadata.
                self->setContactMetadata(id, "username", label);
                out.append({label, id});
            }
            Q_EMIT self->searchResults(prefix, out);
        });
    });
}

void PlatformService::loadProfile(const platform::Identifier& identity)
{
    QPointer<PlatformService> self{this};
    m_client->getProfile(identity, [self, identity](platform::Result<std::optional<platform::Profile>> res) {
        if (!self) return;
        self->post([self, identity, res = std::move(res)] {
            if (!self) return;
            const QString id_hex{QString::fromStdString(HexStr(identity))};
            if (!res.ok()) {
                Q_EMIT self->profileLoadFailed(id_hex, QString::fromStdString(res.error));
                return;
            }
            if (!res.value->has_value()) {
                // Proof-verified absence: the identity has no profile yet.
                Q_EMIT self->profileLoaded(id_hex, {}, {}, {});
                return;
            }
            const auto& profile{**res.value};
            Q_EMIT self->profileLoaded(id_hex,
                                       QString::fromStdString(profile.display_name),
                                       QString::fromStdString(profile.public_message),
                                       QString::fromStdString(profile.avatar_url));
        });
    });
}

bool PlatformService::writeRecord(const std::string& key, const std::vector<unsigned char>& value)
{
    return m_wallet_model.wallet().writePlatformData(key, value);
}

std::vector<unsigned char> PlatformService::readRecord(const std::string& key) const
{
    auto records{m_wallet_model.wallet().getPlatformData(key)};
    const auto it{records.find(key)};
    return it != records.end() ? it->second : std::vector<unsigned char>{};
}

void PlatformService::post(std::function<void()> fn)
{
    QMetaObject::invokeMethod(this, [fn = std::move(fn)] { fn(); }, Qt::QueuedConnection);
}

void PlatformService::refreshContacts()
{
    const auto my_id{myIdentityId()};
    if (!my_id) return;
    QPointer<PlatformService> self{this};
    auto collect = [self](bool to_me) {
        if (!self) return;
        self->m_client->getContactRequests(*self->myIdentityId(), to_me, 0,
            [self, to_me](platform::Result<std::vector<platform::ContactRequest>> res) {
            if (!self || !res.ok()) return;
            self->post([self, to_me, requests = std::move(*res.value)] {
                if (!self) return;
                if (to_me) self->m_incoming_contacts = std::move(requests);
                else self->m_outgoing_contacts = std::move(requests);
                QVector<QPair<QString, QString>> incoming, outgoing;
                for (const auto& cr : self->m_incoming_contacts) {
                    const QString id{QString::fromStdString(HexStr(cr.owner_id))};
                    incoming.append({id, self->contactMetadata(id, "username")});
                    self->hydrateContactMetadata(cr.owner_id);
                }
                for (const auto& cr : self->m_outgoing_contacts) {
                    const QString id{QString::fromStdString(HexStr(cr.to_user_id))};
                    outgoing.append({id, self->contactMetadata(id, "username")});
                    self->hydrateContactMetadata(cr.to_user_id);
                }
                Q_EMIT self->contactsUpdated(incoming, outgoing);
            });
        });
    };
    collect(true);
    collect(false);
}

QString PlatformService::contactMetadata(const QString& identity_hex, const char* field) const
{
    const auto bytes{readRecord("contact/" + std::string{field} + "/" + identity_hex.toStdString())};
    return QString::fromUtf8(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

QString PlatformService::contactDisplayString(const QString& identity_hex) const
{
    const QString username{contactMetadata(identity_hex, "username")};
    if (!username.isEmpty()) return username;
    const QString display_name{contactMetadata(identity_hex, "display-name")};
    if (!display_name.isEmpty()) return display_name;
    return identity_hex.left(8) + "…";
}

QString PlatformService::contactAddressLabel(const QString& identity_hex) const
{
    return tr("%1 (DashPay)").arg(contactDisplayString(identity_hex));
}

void PlatformService::setContactMetadata(const QString& identity_hex, const char* field, const QString& value)
{
    const QByteArray bytes{value.toUtf8()};
    writeRecord("contact/" + std::string{field} + "/" + identity_hex.toStdString(),
                {bytes.begin(), bytes.end()});
}

void PlatformService::hydrateContactMetadata(const platform::Identifier& identity)
{
    const QString id{QString::fromStdString(HexStr(identity))};
    if (m_contact_metadata_pending.contains(id)) return;
    m_contact_metadata_pending.insert(id);

    auto remaining{std::make_shared<int>(2)};
    auto dirty{std::make_shared<bool>(false)};
    QPointer<PlatformService> self{this};
    const auto finished = [self, id, remaining, dirty](bool changed) {
        if (!self) return;
        *dirty |= changed;
        if (--*remaining != 0) return;
        self->m_contact_metadata_pending.remove(id);
        if (*dirty) self->refreshContacts();
    };

    // Both calls verify their Drive proofs before returning data. Never derive
    // a visible label from an unverified contact document or identity id.
    m_client->namesOfIdentity(identity,
        [self, id, finished](platform::Result<std::vector<platform::DpnsName>> result) {
            if (!self) return;
            self->post([self, id, finished, result = std::move(result)] {
                bool changed{false};
                if (result.ok() && !result.value->empty() && self->contactMetadata(id, "username").isEmpty()) {
                    const auto it{std::min_element(result.value->begin(), result.value->end(),
                        [](const auto& a, const auto& b) { return a.normalized_label < b.normalized_label; })};
                    self->setContactMetadata(id, "username", QString::fromStdString(it->label));
                    changed = true;
                }
                finished(changed);
            });
        });
    m_client->getProfile(identity,
        [self, id, finished](platform::Result<std::optional<platform::Profile>> result) {
            if (!self) return;
            self->post([self, id, finished, result = std::move(result)] {
                bool changed{false};
                if (result.ok() && result.value->has_value()) {
                    const QString display_name{QString::fromStdString((**result.value).display_name)};
                    if (self->contactMetadata(id, "display-name") != display_name) {
                        self->setContactMetadata(id, "display-name", display_name);
                        changed = true;
                    }
                }
                finished(changed);
            });
        });
}

bool PlatformService::beginSigningOperation(SigningOperation operation, const QString& identity_hex, QString& error)
{
    if (m_signing_unlock) {
        error = tr("another DashPay operation is still in progress");
        return false;
    }
    auto unlock{m_wallet_model.requestUnlock()};
    if (!unlock.isValid()) {
        error = tr("wallet unlock was cancelled");
        return false;
    }
    m_signing_operation = operation;
    m_signing_identity = identity_hex;
    m_signing_unlock = std::make_unique<WalletModel::UnlockContext>(std::move(unlock));
    return true;
}

void PlatformService::finishContactRequest(const QString& identity_hex, bool ok, const QString& error)
{
    if (m_signing_operation != SigningOperation::CONTACT || m_signing_identity != identity_hex) return;
    m_signing_operation = SigningOperation::NONE;
    m_signing_identity.clear();
    m_signing_unlock.reset();
    Q_EMIT contactRequestFinished(identity_hex, ok, error);
}

void PlatformService::finishProfileUpdate(bool ok, const QString& error)
{
    if (m_signing_operation != SigningOperation::PROFILE) return;
    m_signing_operation = SigningOperation::NONE;
    m_signing_identity.clear();
    m_signing_unlock.reset();
    Q_EMIT profileUpdated(ok, error);
}

bool PlatformService::sendContactRequest(const QString& identity_hex, QString& error)
{
    platform::Identifier identity{};
    const auto bytes{ParseHex(identity_hex.toStdString())};
    if (bytes.size() != identity.size()) {
        error = tr("invalid identity");
        return false;
    }
    if (!beginSigningOperation(SigningOperation::CONTACT, identity_hex, error)) return false;
    std::copy(bytes.begin(), bytes.end(), identity.begin());
    QPointer<PlatformService> self{this};
    m_client->getIdentity(identity, [self, identity, identity_hex](platform::Result<std::optional<platform::Identity>> result) {
        if (!self) return;
        self->post([self, identity, identity_hex, result = std::move(result)] {
            if (!self) return;
            if (!result.ok() || !result.value->has_value()) {
                self->finishContactRequest(identity_hex, false, tr("identity proof failed"));
                return;
            }
            const auto& keys{(**result.value).public_keys};
            const auto it{std::find_if(keys.begin(), keys.end(), [](const auto& key) {
                return key.id == 0 && !key.disabled_at &&
                       key.type == platform::IdentityPublicKey::Type::ECDSA_SECP256K1;
            })};
            if (it == keys.end()) {
                self->finishContactRequest(identity_hex, false, tr("recipient has no usable encryption key"));
                return;
            }
            self->m_contact_flow->sendRequest(identity, it->id, *it);
        });
    });
    return true;
}

bool PlatformService::acceptContact(const QString& identity_hex, QString& error)
{
    const auto it{std::find_if(m_incoming_contacts.begin(), m_incoming_contacts.end(),
        [&identity_hex](const auto& request) { return QString::fromStdString(HexStr(request.owner_id)) == identity_hex; })};
    if (it == m_incoming_contacts.end()) {
        error = tr("incoming request is no longer available");
        return false;
    }
    if (!beginSigningOperation(SigningOperation::CONTACT, identity_hex, error)) return false;
    m_contact_flow->accept(*it);
    return true;
}

void PlatformService::resolvePaymentAddress(const QString& username)
{
    const std::string normalized{platform::st::NormalizeLabel(username.toStdString())};
    QPointer<PlatformService> self{this};
    m_client->resolveName(normalized, [self, username](platform::Result<std::optional<platform::DpnsName>> result) {
        if (!self) return;
        self->post([self, username, result = std::move(result)] {
            if (!self) return;
            if (!result.ok() || !result.value->has_value()) {
                Q_EMIT self->paymentAddressResolved(username, {}, tr("username could not be verified"));
                return;
            }
            const std::string id_hex{HexStr((**result.value).identity)};
            const auto serialized{self->readRecord("contact/key/" + id_hex)};
            if (serialized.size() != 65) {
                Q_EMIT self->paymentAddressResolved(
                    username, {},
                    tr("you are not connected as contacts yet. Open the DashPay tab, send \"%1\" a "
                       "contact request, and wait for them to accept it.").arg(username));
                return;
            }
            const wallet::FriendshipXpub contact_xpub{
                CPubKey{serialized.begin(), serialized.begin() + 33},
                uint256{std::vector<uint8_t>(serialized.begin() + 33, serialized.end())}};
            const std::string cursor_key{"contact/pay-index/" + id_hex};
            const uint32_t index{platform::DecodePaymentCursor(self->readRecord(cursor_key))};
            CTxDestination destination;
            if (!wallet::DeriveFriendshipPaymentDestination(contact_xpub, index, destination)) {
                Q_EMIT self->paymentAddressResolved(username, {}, tr("could not derive contact payment address"));
                return;
            }
            self->writeRecord(cursor_key, platform::EncodePaymentCursor(index + 1));
            // Label the derived destination so transaction history shows the
            // contact's username instead of a bare address.
            self->m_wallet_model.wallet().setAddressBook(
                destination, self->contactAddressLabel(QString::fromStdString(id_hex)).toStdString(), "send");
            Q_EMIT self->paymentAddressResolved(username, QString::fromStdString(EncodeDestination(destination)), {});
        });
    });
}

bool PlatformService::updateProfile(const QString& display_name, const QString& public_message,
                                    const QString& avatar_url, QString& error)
{
    const auto my_id{myIdentityId()};
    if (!my_id) {
        error = tr("register a username first");
        return false;
    }
    if (public_message.size() > 140) {
        error = tr("public message is limited to 140 characters");
        return false;
    }
    if (!avatar_url.isEmpty() && !avatar_url.startsWith("https://", Qt::CaseInsensitive)) {
        error = tr("avatar URL must use HTTPS");
        return false;
    }
    if (!beginSigningOperation(SigningOperation::PROFILE, {}, error)) return false;
    platform::Profile profile;
    profile.owner_id = *my_id;
    profile.display_name = display_name.toStdString();
    profile.public_message = public_message.toStdString();
    profile.avatar_url = avatar_url.toStdString();

    if (avatar_url.isEmpty()) {
        publishProfile(std::move(profile));
        return true;
    }

    auto* manager{new QNetworkAccessManager(this)};
    QNetworkRequest request{QUrl{avatar_url}};
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    QNetworkReply* reply{manager->get(request)};
    auto* timeout{new QTimer(reply)};
    timeout->setSingleShot(true);
    connect(timeout, &QTimer::timeout, reply, &QNetworkReply::abort);
    timeout->start(AVATAR_TIMEOUT_MS);
    auto bytes{std::make_shared<QByteArray>()};
    connect(reply, &QNetworkReply::readyRead, this, [reply, bytes] {
        bytes->append(reply->readAll());
        if (bytes->size() > MAX_AVATAR_BYTES) reply->abort();
    });
    connect(reply, &QNetworkReply::finished, this, [this, manager, reply, timeout, bytes, profile = std::move(profile)]() mutable {
        timeout->stop();
        bytes->append(reply->readAll());
        const bool oversized{bytes->size() > MAX_AVATAR_BYTES};
        const bool network_ok{reply->error() == QNetworkReply::NoError};
        reply->deleteLater();
        manager->deleteLater();
        if (oversized) {
            finishProfileUpdate(false, tr("avatar image exceeds 2 MiB"));
            return;
        }
        if (!network_ok) {
            finishProfileUpdate(false, tr("avatar image could not be downloaded"));
            return;
        }
        QImage image{QImage::fromData(*bytes)};
        if (image.isNull()) {
            finishProfileUpdate(false, tr("avatar URL did not return a supported image"));
            return;
        }
        const QByteArray hash{QCryptographicHash::hash(*bytes, QCryptographicHash::Sha256)};
        profile.avatar_hash.assign(hash.begin(), hash.end());
        const QImage sample{image.scaled(9, 8, Qt::IgnoreAspectRatio, Qt::SmoothTransformation)
                                 .convertToFormat(QImage::Format_Grayscale8)};
        profile.avatar_fingerprint.assign(8, 0);
        for (int y = 0; y < 8; ++y) {
            uint8_t row{0};
            for (int x = 0; x < 8; ++x) {
                if (qGray(sample.pixel(x, y)) > qGray(sample.pixel(x + 1, y))) row |= uint8_t{1} << x;
            }
            profile.avatar_fingerprint[y] = row;
        }
        publishProfile(std::move(profile));
    });
    return true;
}

void PlatformService::publishProfile(platform::Profile profile)
{

    QPointer<PlatformService> self{this};
    m_client->getProfile(profile.owner_id, [self, profile](platform::Result<std::optional<platform::Profile>> current_res) {
        if (!self) return;
        self->post([self, profile, current_res = std::move(current_res)]() mutable {
            if (!self) return;
            if (!current_res.ok()) {
                self->finishProfileUpdate(false, tr("could not verify the current profile"));
                return;
            }
            std::optional<platform::Identifier> existing_id;
            uint64_t revision{1};
            if (current_res.value->has_value()) {
                existing_id = (**current_res.value).document_id;
                revision = (**current_res.value).revision + 1;
            }
            self->m_client->getIdentityContractNonce(profile.owner_id, platform::DASHPAY_CONTRACT_ID,
                [self, profile, existing_id, revision](platform::Result<uint64_t> nonce_res) {
                if (!self) return;
                self->post([self, profile, existing_id, revision, nonce_res = std::move(nonce_res)] {
                    if (!self) return;
                    if (!nonce_res.ok()) {
                        self->finishProfileUpdate(false, tr("could not fetch identity nonce"));
                        return;
                    }
                    interfaces::Wallet& wallet{self->m_wallet_model.wallet()};
                    const auto signer = [&wallet](const uint256& digest, std::vector<uint8_t>& sig) {
                        auto res{wallet.signPlatformDigest(wallet::IdentityAuthKey{0, 1}, digest)};
                        if (!res) return false;
                        sig = std::move(res.value);
                        return true;
                    };
                    auto built{platform::st::BuildProfile(profile.owner_id, *nonce_res.value + 1, profile,
                                                          revision, existing_id, /*signature_public_key_id=*/1, signer)};
                    if (!built.ok()) {
                        self->finishProfileUpdate(false, QString::fromStdString(built.error));
                        return;
                    }
                    self->m_client->broadcastStateTransition(built.value->bytes,
                        [self, profile, revision](platform::Result<platform::BroadcastResult> broadcast) {
                        if (!self) return;
                        self->post([self, profile, revision, broadcast = std::move(broadcast)] {
                            if (!self) return;
                            if (!broadcast.ok() || (!broadcast.value->accepted && broadcast.value->error.find("already") == std::string::npos)) {
                                self->finishProfileUpdate(false, broadcast.ok()
                                                                     ? QString::fromStdString(broadcast.value->error)
                                                                     : tr("broadcast failed"));
                                return;
                            }
                            auto attempts{std::make_shared<int>(12)};
                            auto confirm{std::make_shared<std::function<void()>>()};
                            *confirm = [self, profile, revision, attempts, confirm] {
                                if (!self) return;
                                self->m_client->getProfile(profile.owner_id,
                                    [self, profile, revision, attempts, confirm](platform::Result<std::optional<platform::Profile>> proved) {
                                    if (!self) return;
                                    self->post([self, profile, revision, attempts, confirm, proved = std::move(proved)] {
                                        if (!self) return;
                                        if (proved.ok() && proved.value->has_value()) {
                                            const auto& got{**proved.value};
                                            if (got.revision == revision && got.display_name == profile.display_name &&
                                                got.public_message == profile.public_message && got.avatar_url == profile.avatar_url) {
                                                self->finishProfileUpdate(true, {});
                                                return;
                                            }
                                        }
                                        if (--*attempts == 0) {
                                            self->finishProfileUpdate(false, tr("profile update was not confirmed by a "
                                                                                "proof"));
                                            return;
                                        }
                                        QTimer::singleShot(2500, self, *confirm);
                                    });
                                });
                            };
                            QTimer::singleShot(1500, self, *confirm);
                        });
                    });
                });
            });
        });
    });
}

void PlatformService::updateNodeContext()
{
    interfaces::Node& node{m_client_model.node()};

    // Evonode DAPI endpoints from the deterministic masternode list.
    std::vector<platform::Endpoint> endpoints;
    const auto [mn_list, tip] = node.evo().getListAtChainTip();
    if (mn_list) {
        mn_list->forEachMN(/*only_valid=*/true, [&endpoints](const auto& dmn) {
            const auto services{dmn->getPlatformHTTPSAddrs()};
            for (const auto& service : services) {
                endpoints.push_back(platform::Endpoint{service, dmn->getProTxHash()});
            }
            // Compatibility for deterministic-list entries serialized before
            // the typed NetInfo accessor was introduced. The authoritative
            // object is still the locally validated deterministic list; JSON
            // is used only to decode its legacy address representation.
            if (services.empty()) {
                const UniValue json{dmn->toJson()};
                const UniValue& addresses{json["state"]["addresses"]["platform_https"]};
                if (addresses.isArray()) {
                    for (size_t i = 0; i < addresses.size(); ++i) {
                        if (!addresses[i].isStr()) continue;
                        if (const auto service{Lookup(addresses[i].get_str(), 1443, /*fAllowLookup=*/false)}) {
                            endpoints.push_back(platform::Endpoint{*service, dmn->getProTxHash()});
                        }
                    }
                }
            }
        });
    }
    LogPrint(BCLog::QT, "Platform GUI: loaded %d evonode HTTPS endpoints\n", endpoints.size());
    const bool have_endpoints{!endpoints.empty()};
    m_client->updateEndpoints(std::move(endpoints));

    // Best local ChainLock height: a coarse freshness floor letting the
    // client reject stale (validly signed) platform proofs replayed by an
    // on-path attacker.
    m_client->updateCoreChainLockedHeight(node.llmq().getBestChainLock().m_height);

    // Platform-signing quorum public keys for proof verification.
    const auto llmq_type{static_cast<uint8_t>(Params().GetConsensus().llmqTypePlatform)};
    auto quorums{node.llmq().getPlatformQuorums(llmq_type)};
    std::vector<platform::QuorumKey> keys;
    keys.reserve(quorums.size());
    for (auto& q : quorums) {
        keys.push_back(platform::QuorumKey{q.m_quorum_hash, std::move(q.m_pubkey), q.m_height});
    }
    m_client->updateQuorumKeys(llmq_type, std::move(keys));

    // Seed-only recovery needs endpoints (and their quorum keys) to issue
    // proof-backed queries, so it is armed from here rather than the
    // constructor. maybeStart() is a cheap no-op once it has run.
    if (have_endpoints) m_recovery->maybeStart();
}
