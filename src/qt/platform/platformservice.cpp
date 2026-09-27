// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/platform/platformservice.h>

#include <chainparams.h>
#include <chainparamsbase.h>
#include <interfaces/handler.h>
#include <interfaces/node.h>
#include <interfaces/wallet.h>
#include <key_io.h>
#include <logging.h>
#include <netbase.h>
#include <platform/helpers.h>
#include <platform/walletrecords.h>
#include <qt/clientmodel.h>
#include <qt/platform/contactflow.h>
#include <qt/platform/identityflow.h>
#include <qt/platform/platformrecovery.h>
#include <qt/platform/platformui.h>
#include <util/strencodings.h>
#include <util/system.h>
#include <util/threadnames.h>

#include <QMetaObject>
#include <QPointer>

#include <algorithm>
#include <utility>

namespace {
//! Flow advance / retry cadence.
constexpr int TICK_INTERVAL_MS{5'000};
//! Endpoint + quorum key refresh cadence.
constexpr int CONTEXT_INTERVAL_MS{60'000};
//! Flow-level cap on contact requests collected per direction.
constexpr size_t MAX_CONTACT_REQUESTS{1000};
//! Base duffs an identity registration locks, before any vote reserve.
//! Matches the mobile wallets' default for an uncontested registration.
constexpr CAmount IDENTITY_FUNDING_DUFFS{1000000};
constexpr int PROFILE_CONFIRM_ATTEMPTS{12};
constexpr int PROFILE_CONFIRM_INTERVAL_MS{2500};
//! A contact's username and profile are re-read at most this often: every
//! contacts refresh past it (the list is read while it is shown) reads them
//! again, so a contact's profile edit shows within minutes.
constexpr int64_t CONTACT_METADATA_TTL_SECONDS{5 * 60};

//! Keeps the wallet unlocked for the life of a SigningOperation.
class UnlockScope final : public platform::UnlockScope
{
public:
    explicit UnlockScope(WalletModel::UnlockContext&& context) :
        m_context(std::move(context))
    {
    }

private:
    WalletModel::UnlockContext m_context;
};

//! The client the flows use: every result is observed by the service
//! before the flow sees it, so a protocol-version or chain-id signal on any
//! read freezes writes no matter which flow issued it.
class ObservingClient final : public platform::PlatformClient
{
public:
    ObservingClient(PlatformService& service, std::unique_ptr<platform::PlatformClient> inner) :
        m_service(service),
        m_inner(std::move(inner))
    {
    }

    void resolveName(const std::string& normalized_label, Callback<platform::DpnsName> cb) override
    {
        m_inner->resolveName(normalized_label, observe(std::move(cb)));
    }
    void searchNames(const std::string& prefix, uint32_t limit, const platform::Identifier& start_after,
                     Callback<platform::Paged<platform::DpnsName>> cb) override
    {
        m_inner->searchNames(prefix, limit, start_after, observe(std::move(cb)));
    }
    void namesOfIdentity(const platform::Identifier& identity, const platform::Identifier& start_after,
                         Callback<platform::Paged<platform::DpnsName>> cb) override
    {
        m_inner->namesOfIdentity(identity, start_after, observe(std::move(cb)));
    }
    void getIdentity(const platform::Identifier& id, Callback<platform::Identity> cb) override
    {
        m_inner->getIdentity(id, observe(std::move(cb)));
    }
    void getIdentityByPublicKeyHash(const std::array<uint8_t, 20>& pubkey_hash, Callback<platform::Identity> cb) override
    {
        m_inner->getIdentityByPublicKeyHash(pubkey_hash, observe(std::move(cb)));
    }
    void getIdentityContractNonce(const platform::Identifier& id, const platform::Identifier& contract_id,
                                  Callback<uint64_t> cb) override
    {
        m_inner->getIdentityContractNonce(id, contract_id, observe(std::move(cb)));
    }
    void getProfile(const platform::Identifier& owner_id, Callback<platform::Profile> cb) override
    {
        m_inner->getProfile(owner_id, observe(std::move(cb)));
    }
    void getContactRequests(const platform::Identifier& identity, bool to_me, uint64_t since_ms,
                            const platform::Identifier& start_after,
                            Callback<platform::Paged<platform::ContactRequest>> cb) override
    {
        m_inner->getContactRequests(identity, to_me, since_ms, start_after, observe(std::move(cb)));
    }
    void getContestedNameState(const std::string& normalized_label, Callback<platform::ContestedNameState> cb) override
    {
        m_inner->getContestedNameState(normalized_label, observe(std::move(cb)));
    }
    void broadcastStateTransition(const std::vector<uint8_t>& state_transition, BroadcastCallback cb) override
    {
        QPointer<PlatformService> service{&m_service};
        m_inner->broadcastStateTransition(state_transition, [service, cb = std::move(cb)](platform::Status status) {
            if (service)
                service->post([service, status] {
                    if (service) service->observeStatus(status, /*verified_read=*/false, /*protocol_version=*/0);
                });
            cb(std::move(status));
        });
    }

    util::Result<platform::Built> buildIdentityCreate(const platform::SigningOperation& op,
                                                      const platform::AssetLockProof& proof,
                                                      const std::vector<platform::NewIdentityKey>& keys) override
    {
        return m_inner->buildIdentityCreate(op, proof, keys);
    }
    util::Result<platform::Built> buildDpnsPreorder(const platform::SigningOperation& op,
                                                    const platform::Identifier& owner, uint64_t nonce,
                                                    const std::string& label, const std::array<uint8_t, 32>& salt) override
    {
        return m_inner->buildDpnsPreorder(op, owner, nonce, label, salt);
    }
    util::Result<platform::Built> buildDpnsDomain(const platform::SigningOperation& op,
                                                  const platform::Identifier& owner, uint64_t nonce,
                                                  const std::string& label, const std::array<uint8_t, 32>& salt) override
    {
        return m_inner->buildDpnsDomain(op, owner, nonce, label, salt);
    }
    util::Result<platform::Built> buildProfile(const platform::SigningOperation& op, const platform::Identifier& owner,
                                               uint64_t nonce, const platform::Profile& existing,
                                               const platform::ProfileInput& input) override
    {
        return m_inner->buildProfile(op, owner, nonce, existing, input);
    }
    util::Result<platform::Built> buildContactRequest(const platform::SigningOperation& op,
                                                      const platform::Identity& sender,
                                                      const platform::Identity& recipient, uint64_t nonce,
                                                      const platform::ContactRequestInput& input) override
    {
        return m_inner->buildContactRequest(op, sender, recipient, nonce, input);
    }
    util::Result<uint64_t> contestedVoteFundCredits() override { return m_inner->contestedVoteFundCredits(); }

    void updateEndpoints(std::vector<platform::Endpoint> endpoints) override
    {
        m_inner->updateEndpoints(std::move(endpoints));
    }
    void updateQuorumKeys(uint8_t llmq_type, std::vector<platform::QuorumKey> keys) override
    {
        m_inner->updateQuorumKeys(llmq_type, std::move(keys));
    }
    void updateCoreChainLockedHeight(int32_t height) override { m_inner->updateCoreChainLockedHeight(height); }
    void shutdown() override { m_inner->shutdown(); }

private:
    //! The status reaches the service on the GUI thread before the caller's
    //! own posted continuation, which is queued behind it.
    template <typename T>
    Callback<T> observe(Callback<T> cb)
    {
        QPointer<PlatformService> service{&m_service};
        return [service, cb = std::move(cb)](platform::Result<T> res) {
            if (service) {
                const platform::Status status{res.status};
                const uint32_t protocol_version{res.metadata.protocol_version};
                service->post([service, status, protocol_version] {
                    if (service) service->observeStatus(status, /*verified_read=*/true, protocol_version);
                });
            }
            cb(std::move(res));
        };
    }

    PlatformService& m_service;
    std::unique_ptr<platform::PlatformClient> m_inner;
};

std::optional<PlatformNetworkSettings> g_network_settings_for_testing;

std::optional<platform::ProxyConfig> ReadProxy(interfaces::Node& node, Network net)
{
    Proxy proxy;
    if (!node.getProxy(net, proxy)) return std::nullopt;
    if (proxy.m_is_unix_socket) {
        return platform::ProxyConfig{proxy.m_unix_socket_path.substr(ADDR_PREFIX_UNIX.size()),
                                     proxy.m_randomize_credentials};
    }
    return platform::ProxyConfig{proxy.proxy, proxy.m_randomize_credentials};
}

//! Why writes are refused while the service is frozen or on another chain.
QString RefusalText(platform::StatusKind kind)
{
    platform::Status status;
    status.kind = kind;
    return PlatformUi::Describe(status, PlatformUi::Context::READ, {}).text;
}

std::optional<platform::Identifier> ParseIdentifier(const QString& hex)
{
    const auto bytes{TryParseHex<uint8_t>(hex.toStdString())};
    platform::Identifier id{};
    if (!bytes || bytes->size() != id.size()) return std::nullopt;
    std::copy(bytes->begin(), bytes->end(), id.begin());
    return id;
}

} // namespace

PlatformNetworkSettings PlatformNetworkSettings::Read(interfaces::Node& node)
{
    if (g_network_settings_for_testing) return *g_network_settings_for_testing;
    PlatformNetworkSettings settings;
    settings.ipv4 = node.isReachable(NET_IPV4);
    settings.ipv6 = node.isReachable(NET_IPV6);
    settings.onion = node.isReachable(NET_ONION);
    // -proxy sets IPv4 and IPv6 together.
    settings.clearnet_proxy = ReadProxy(node, NET_IPV4);
    settings.onion_proxy = ReadProxy(node, NET_ONION);
    return settings;
}

void PlatformNetworkSettings::SetForTesting(std::optional<PlatformNetworkSettings> settings)
{
    g_network_settings_for_testing = std::move(settings);
}

bool PlatformRoute::allows(const CService& endpoint) const
{
    if (endpoint.IsIPv4()) return ipv4;
    if (endpoint.IsIPv6()) return ipv6;
    if (endpoint.IsTor()) return onion;
    return false;
}

std::optional<PlatformRoute> PlatformRoute::Choose(const PlatformNetworkSettings& settings)
{
    // One proxy per client: onion evonodes share the clearnet route only
    // when the onion proxy is that same proxy (plain -proxy).
    if (settings.ipv4 || settings.ipv6) {
        return PlatformRoute{settings.clearnet_proxy, settings.ipv4, settings.ipv6,
                             settings.onion && settings.onion_proxy && settings.onion_proxy == settings.clearnet_proxy};
    }
    if (settings.onion && settings.onion_proxy) {
        return PlatformRoute{settings.onion_proxy, /*ipv4=*/false, /*ipv6=*/false, /*onion=*/true};
    }
    return std::nullopt;
}

std::string PlatformService::EffectiveChainId()
{
    const std::string override{gArgs.GetArg("-platformchainid", "")};
    if (!override.empty() && Params().NetworkIDString() != CBaseChainParams::MAIN) return override;
    return Params().PlatformChainId();
}

bool PlatformService::IsEnabled(interfaces::Wallet& wallet)
{
    const auto records{wallet.getPlatformData(platform::records::ENABLED)};
    const auto it{records.find(platform::records::ENABLED)};
    return it != records.end() && it->second == std::vector<unsigned char>{1};
}

bool PlatformService::Enable(interfaces::Wallet& wallet)
{
    return wallet.writePlatformData(platform::records::VERSION, platform::EncodeRecordVersion()) &&
           wallet.writePlatformData(platform::records::ENABLED, {1});
}

bool PlatformService::WipeRecords(interfaces::Wallet& wallet, const std::string& keep)
{
    bool ok{true};
    for (const char* prefix : {"platform/", "identity/", "contact/"}) {
        for (const auto& [key, value] : wallet.getPlatformData(prefix)) {
            if (key != keep) ok &= wallet.writePlatformData(key, {});
        }
    }
    return ok;
}

bool PlatformService::HoldsUnconsumedFunding(interfaces::Wallet& wallet)
{
    const auto records{wallet.getPlatformData(platform::records::IDENTITY)};
    const auto it{records.find(platform::records::IDENTITY)};
    platform::IdentityRecord record;
    return it != records.end() && platform::DeserializeIdentityRecord(it->second, record) &&
           IdentityFlow::HoldsUnconsumedFunding(record);
}

bool PlatformService::DiscardRecords(interfaces::Wallet& wallet)
{
    const std::string keep{HoldsUnconsumedFunding(wallet) ? platform::records::IDENTITY : ""};
    return WipeRecords(wallet, keep) && Enable(wallet);
}

wallet::FriendshipXpub PlatformService::FriendshipXpubFromCompact(const wallet::CompactXpub& compact)
{
    wallet::FriendshipXpub xpub;
    std::copy_n(compact.begin(), xpub.parent_fingerprint.size(), xpub.parent_fingerprint.begin());
    xpub.chaincode = ChainCode{Span{compact.data() + 4, 32}};
    xpub.pubkey.Set(compact.begin() + 36, compact.end());
    return xpub;
}

PlatformAvailability PlatformService::Availability(WalletModel& wallet_model, ClientModel& client_model,
                                                   const PlatformService* service)
{
    using Gate = PlatformAvailability::Gate;
    PlatformAvailability out;
    interfaces::Wallet& wallet{wallet_model.wallet()};
    out.enabled = IsEnabled(wallet);
    if (EffectiveChainId().empty()) {
        out.gate = Gate::NO_PLATFORM;
        out.title = tr("DashPay isn't available on this network");
        out.reason = tr("Dash Platform does not run on this network.");
    } else if (wallet.isLegacy()) {
        out.gate = Gate::LEGACY_WALLET;
        out.title = tr("This wallet can't use DashPay");
        out.reason = tr("DashPay needs a descriptor wallet. Create a new wallet, or migrate this one with the "
                        "migratewallet console command.");
    } else if (wallet.privateKeysDisabled() || wallet.hasExternalSigner()) {
        out.gate = Gate::NO_PRIVATE_KEYS;
        out.title = tr("This wallet can't use DashPay");
        out.reason = tr("DashPay needs a wallet that holds its own private keys.");
    } else if (!PlatformRoute::Choose(PlatformNetworkSettings::Read(client_model.node()))) {
        out.gate = Gate::NETWORK_SETTINGS;
        out.title = tr("DashPay can't reach Dash Platform");
        out.reason = tr("Your network settings allow no network DashPay can use. DashPay needs IPv4 or IPv6, "
                        "directly or through your proxy, or Tor with evonodes that have onion addresses.");
    } else if (service && service->unreachable()) {
        out.gate = Gate::UNREACHABLE;
        out.title = tr("DashPay can't reach Dash Platform");
        out.reason = tr("No evonode can be reached over the networks your network settings allow, such as Tor "
                        "only. DashPay stays off until one can.");
    } else if (!client_model.node().getNetworkActive()) {
        out.gate = Gate::NETWORK_INACTIVE;
        out.title = tr("DashPay is paused");
        out.reason = tr("Network activity is turned off.");
    } else if (client_model.node().isInitialBlockDownload()) {
        // A persisted ChainLock passes the next gate on a node that has been
        // offline; its masternode list and quorums are stale until synced.
        out.gate = Gate::SYNCING;
        out.title = tr("Waiting for sync");
        out.reason = tr("DashPay starts once Dash Core has finished syncing.");
    } else if (client_model.node().llmq().getBestChainLock().m_height <= 0) {
        out.gate = Gate::NO_CHAINLOCK;
        out.title = tr("Almost ready");
        out.reason = tr("DashPay starts once this node has received a ChainLock, usually a few minutes after "
                        "syncing.");
    }
    return out;
}

PlatformService::PlatformService(WalletModel& wallet_model, ClientModel& client_model,
                                 std::unique_ptr<platform::PlatformClient> client, PlatformRoute route, QObject* parent) :
    QObject(parent),
    m_wallet_model(wallet_model),
    m_client_model(client_model),
    m_client(std::make_unique<ObservingClient>(*this, std::move(client))),
    m_route(std::move(route)),
    m_chain_id(EffectiveChainId())
{
    // The records carry the chain id of the first verified response they
    // were written after; none yet means nothing was verified so far.
    const auto recorded{readRecord(platform::records::CHAIN_ID)};
    const std::string recorded_chain_id{recorded.begin(), recorded.end()};
    m_chain_id_stamped = !recorded_chain_id.empty();
    if (m_chain_id_stamped && recorded_chain_id != m_chain_id) {
        LogPrintf("Platform GUI: wallet records were written for chain %s, this node verifies %s\n", recorded_chain_id,
                  m_chain_id);
        m_network_changed = true;
    } else {
        // Records of another layout are never migrated: wipe them and let
        // recovery rebuild the state from chain.
        const bool have_records{!m_wallet_model.wallet().getPlatformData("identity/").empty() ||
                                !m_wallet_model.wallet().getPlatformData("contact/").empty()};
        if (!platform::IsRecordSetCurrent(readRecord(platform::records::VERSION), have_records)) {
            LogPrintf("Platform GUI: wallet records use another layout version; discarding them for recovery\n");
            DiscardRecords(m_wallet_model.wallet());
            m_chain_id_stamped = false;
        }
    }

    m_identity_flow = std::make_unique<IdentityFlow>(*this);
    connect(m_identity_flow.get(), &IdentityFlow::stateChanged, this, &PlatformService::identityStateChanged);
    connect(m_identity_flow.get(), &IdentityFlow::failed, this, &PlatformService::flowFailed);

    m_contact_flow = std::make_unique<ContactFlow>(*this);
    connect(m_contact_flow.get(), &ContactFlow::requestPending, this, [this](const QString& id) {
        if (m_contact_request_in_flight != id) return;
        // Still in flight: the next request would reuse its nonce.
        m_contact_request_pending = true;
        Q_EMIT contactRequestPending(id);
        publishContacts();
    });
    connect(m_contact_flow.get(), &ContactFlow::requestUnconfirmed, this, [this](const QString& id) {
        if (m_contact_request_in_flight != id) return;
        m_contact_request_in_flight.clear();
        m_contact_request_pending = false;
        refreshContacts();
    });
    connect(m_contact_flow.get(), &ContactFlow::requestSent, this, [this](const QString& id) {
        m_accepted_errors.remove(id);
        m_accepted_refused.remove(id);
        finishContactRequest(id, true, {}, {});
        refreshContacts();
    });
    connect(m_contact_flow.get(), &ContactFlow::requestFailed, this,
            [this](const QString& id, const QString& error, const QString& details) {
                finishContactRequest(id, false, error, details);
            });
    connect(m_contact_flow.get(), &ContactFlow::acceptedCompleted, this,
            [this](const QString& id, const QString& error, bool retryable) {
                m_completing_contacts.remove(id);
                const auto status{m_wallet_model.getEncryptionStatus()};
                // The locked wallet is the row's own wording.
                if (error.isEmpty() ||
                    (retryable && (status == WalletModel::Locked || status == WalletModel::UnlockedForMixingOnly))) {
                    m_accepted_errors.remove(id);
                } else {
                    m_accepted_errors.insert(id, error);
                }
                if (!retryable) m_accepted_refused.insert(id);
                publishContacts();
            });

    m_recovery = std::make_unique<PlatformRecovery>(*this);
    connect(m_recovery.get(), &PlatformRecovery::finished, this, [this](bool recovered) {
        if (recovered) refreshContacts();
    });

    m_tick_timer = new QTimer(this);
    m_tick_timer->setInterval(TICK_INTERVAL_MS);
    connect(m_tick_timer, &QTimer::timeout, this, [this] {
        if (!m_network_changed) m_identity_flow->advance();
    });
    m_tick_timer->start();

    m_context_thread = new QThread(this);
    m_context_worker = new QObject();
    m_context_worker->moveToThread(m_context_thread);
    m_context_thread->start();
    QMetaObject::invokeMethod(m_context_worker, [] { util::ThreadRename("qt-platformctx"); });

    m_context_timer = new QTimer(this);
    m_context_timer->setInterval(CONTEXT_INTERVAL_MS);
    connect(m_context_timer, &QTimer::timeout, this, &PlatformService::updateNodeContext);
    m_context_timer->start();

    // A new ChainLock moves the freshness anchor at once rather than on the
    // next timer tick.
    m_chainlock_handler = m_client_model.node().handleNotifyChainLock([this](const std::string&, int32_t height) {
        post([this, height] { m_client->updateCoreChainLockedHeight(height); });
    });
    connect(&m_client_model, &ClientModel::networkActiveChanged, this, [this](bool active) {
        updateNodeContext();
        Q_EMIT networkActiveChanged(active);
    });
    // A wallet unlock is the user action a parked flow waits for, and what
    // a recovery that could not derive the identity's keys needs.
    connect(&m_wallet_model, &WalletModel::encryptionStatusChanged, this, [this] {
        if (m_wallet_model.getEncryptionStatus() != WalletModel::Unlocked) return;
        m_identity_flow->retryAfterUnlock();
        if (!m_network_changed) m_recovery->maybeStart();
        completeAcceptedContacts();
    });

    updateNodeContext();
    if (!m_network_changed) m_identity_flow->advance();
}

PlatformService::~PlatformService() { stop(); }

void PlatformService::stop()
{
    if (m_stopped) return;
    m_stopped = true;
    m_chainlock_handler.reset();
    disconnect(&m_client_model, nullptr, this, nullptr);
    disconnect(&m_wallet_model, nullptr, this, nullptr);
    m_tick_timer->stop();
    m_context_timer->stop();
    // A collection still running on the worker finishes first; post()
    // drops its report.
    m_context_thread->quit();
    m_context_thread->wait();
    delete m_context_worker;
    m_context_worker = nullptr;
    m_identity_flow->stop();
    m_recovery->stop();
    // The flows hold callbacks into the client; stop it before they go.
    m_client->shutdown();
    // The profile dialog waits for its update to end before it can close.
    finishProfileUpdate(false, tr("DashPay has stopped because Dash Core is shutting down."), {});
}

void PlatformService::discardStateAfterNetworkChange()
{
    // The chain id is stamped again by the next verified response, so a
    // chain id no response has verified never ends up in the records.
    if (!DiscardRecords(m_wallet_model.wallet())) return;
    m_network_changed = false;
    m_chain_id_stamped = false;
    m_identity_flow->reload();
    m_recovery->maybeStart();
    Q_EMIT platformNetworkChanged();
}

PlatformService::SigningAttempt PlatformService::beginSigningOperation(
    platform::OperationKind kind, std::vector<uint32_t> key_ids,
    std::optional<platform::IdentityPublicKey> document_key, std::optional<wallet::RegistrationFundingKey> funding_key)
{
    SigningAttempt attempt;
    if (!writesAllowed(attempt.refusal)) return attempt;
    WalletModel::UnlockContext unlock{m_wallet_model.requestUnlock()};
    if (!unlock.isValid()) {
        attempt.unlock_declined = true;
        return attempt;
    }
    attempt.op.emplace(platform::SigningOperation(m_wallet_model.wallet(), kind, std::move(key_ids), std::move(document_key),
                                                  funding_key, std::make_unique<UnlockScope>(std::move(unlock))));
    return attempt;
}

bool PlatformService::registrationAllowed(QString& error) const
{
    if (!writesAllowed(error)) return false;
    error = m_recovery->registrationBlocker();
    return error.isEmpty();
}

bool PlatformService::writesAllowed(QString& error) const
{
    if (m_stopped) {
        error = tr("DashPay has stopped because Dash Core is shutting down.");
        return false;
    }
    if (m_unsupported_version) {
        error = RefusalText(platform::StatusKind::UNSUPPORTED_PROTOCOL_VERSION);
        return false;
    }
    if (m_network_changed) {
        // The records say so, not an answer from Platform.
        error = tr("This wallet's DashPay data is for another Dash Platform network. Rebuild it on the DashPay tab "
                   "first.");
        return false;
    }
    if (m_network_mismatch) {
        error = RefusalText(platform::StatusKind::CHAIN_ID_MISMATCH);
        return false;
    }
    return true;
}

QString PlatformService::networkMismatch() const
{
    if (!m_network_mismatch) return {};
    return RefusalText(platform::StatusKind::CHAIN_ID_MISMATCH);
}

QString PlatformService::networkMismatchDetails() const
{
    if (!m_network_mismatch) return {};
    return tr("Expected chain: %1").arg(QString::fromStdString(m_chain_id)) + QLatin1Char('\n') +
           tr("Received: %1").arg(QString::fromStdString(m_network_mismatch_message));
}

bool PlatformService::networkActive() const { return !m_stopped && m_client_model.node().getNetworkActive(); }

std::optional<platform::IdentityPublicKey> PlatformService::documentSigningKey(const platform::Identity& identity) const
{
    using Purpose = platform::IdentityPublicKey::Purpose;
    using Type = platform::IdentityPublicKey::Type;
    const uint32_t id{m_identity_flow->record().auth_key_id};
    const auto ours{m_wallet_model.wallet().getPlatformPubKey(wallet::IdentityAuthKey{0, id})};
    for (const auto& key : identity.public_keys) {
        if (key.id != id || key.purpose != Purpose::AUTHENTICATION || key.type != Type::ECDSA_SECP256K1 ||
            key.disabled_at || key.security_level == platform::IdentityPublicKey::SecurityLevel::MASTER) {
            continue;
        }
        // A locked wallet cannot check the key; the signing step's own
        // unlock follows and the wallet refuses a key it does not hold.
        if (ours && !std::equal(key.data.begin(), key.data.end(), ours.value.begin(), ours.value.end())) continue;
        return key;
    }
    return std::nullopt;
}

QString PlatformService::myUsername() const
{
    const auto& rec{m_identity_flow->record()};
    return rec.state == IdentityFlow::State::REGISTERED ? QString::fromStdString(rec.label) : QString{};
}

std::optional<platform::Identifier> PlatformService::myIdentityId() const { return m_identity_flow->identityId(); }

std::optional<CAmount> PlatformService::identityFundingAmount(bool contested) const
{
    if (!contested) return IDENTITY_FUNDING_DUFFS;
    const auto credits{contestedNameCredits()};
    if (!credits) return std::nullopt;
    return IDENTITY_FUNDING_DUFFS + static_cast<CAmount>(*credits / platform::helpers::CreditsPerDuff());
}

std::optional<uint64_t> PlatformService::contestedNameCredits() const
{
    const auto credits{m_client->contestedVoteFundCredits()};
    if (!credits) return std::nullopt;
    return *credits;
}

std::optional<uint64_t> PlatformService::contestedNameRequiredCredits() const
{
    const auto reserve{contestedNameCredits()};
    if (!reserve) return std::nullopt;
    return *reserve + 2 * DOCUMENT_FEE_RESERVE_CREDITS;
}

void PlatformService::refreshIdentityBalance()
{
    const auto id{myIdentityId()};
    if (!id) return;
    QPointer<PlatformService> self{this};
    m_client->getIdentity(*id, [self](platform::Result<platform::Identity> res) {
        if (!self) return;
        self->post([self, res = std::move(res)] {
            if (!self) return;
            if (res.ok()) {
                Q_EMIT self->identityBalanceLoaded(res.value->balance);
            } else {
                const auto error{PlatformUi::Describe(res.status, PlatformUi::Context::READ, tr("Load balance"))};
                Q_EMIT self->identityBalanceFailed(error.text, error.details);
            }
        });
    });
}

void PlatformService::checkNameAvailability(const QString& name)
{
    const std::string label{name.toStdString()};
    const std::string normalized{platform::helpers::NormalizeLabel(label)};
    const bool contested{platform::helpers::IsContestedUsername(label)};
    QPointer<PlatformService> self{this};
    m_client->resolveName(normalized, [self, normalized, contested](platform::Result<platform::DpnsName> res) {
        if (!self) return;
        self->post([self, normalized, contested, res = std::move(res)] {
            if (!self) return;
            const QString label{QString::fromStdString(normalized)};
            if (res.ok()) {
                if (res.value->identity == self->myIdentityId()) {
                    Q_EMIT self->nameIsOurs(label);
                } else {
                    Q_EMIT self->nameAvailability(label, /*available=*/false, contested);
                }
                return;
            }
            if (!res.provenAbsent()) {
                const auto error{PlatformUi::Describe(res.status, PlatformUi::Context::NAME_CHECK, tr("Check username"))};
                Q_EMIT self->nameAvailabilityFailed(label, error.text, error.details);
                return;
            }
            if (!contested) {
                Q_EMIT self->nameAvailability(label, /*available=*/true, /*contested=*/false);
                return;
            }
            // A contested label absent from the domain tree may still have an
            // active (or locked) vote; only a proven-absent contest makes it
            // truly available.
            self->m_client->getContestedNameState(normalized, [self, label](
                                                                  platform::Result<platform::ContestedNameState> vote_res) {
                if (!self) return;
                self->post([self, label, vote_res = std::move(vote_res)] {
                    if (!self) return;
                    if (vote_res.provenAbsent()) {
                        Q_EMIT self->nameAvailability(label, /*available=*/true, /*contested=*/true);
                    } else if (vote_res.ok()) {
                        Q_EMIT self->nameAvailability(label, /*available=*/false, /*contested=*/true);
                    } else {
                        const auto error{PlatformUi::Describe(vote_res.status, PlatformUi::Context::NAME_CHECK,
                                                              tr("Check username vote"))};
                        Q_EMIT self->nameAvailabilityFailed(label, error.text, error.details);
                    }
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
            const QString label{QString::fromStdString(normalized)};
            if (res.ok()) {
                Q_EMIT self->contestedNameState(label, *res.value, QString{});
            } else {
                Q_EMIT self->contestedNameState(label, platform::ContestedNameState{},
                                                res.provenAbsent()
                                                    ? tr("No vote is open for this username.")
                                                    : PlatformUi::Describe(res.status, PlatformUi::Context::READ, {}).text);
            }
        });
    });
}

void PlatformService::searchNames(const QString& prefix)
{
    const std::string normalized{platform::helpers::NormalizeLabel(prefix.toStdString())};
    QPointer<PlatformService> self{this};
    m_client->searchNames(normalized, SEARCH_PAGE_SIZE, platform::Identifier{},
                          [self, prefix](platform::Result<platform::Paged<platform::DpnsName>> res) {
                              if (!self) return;
                              self->post([self, prefix, res = std::move(res)] {
                                  if (!self) return;
                                  if (!res.ok() && !res.provenAbsent()) {
                                      const auto error{PlatformUi::Describe(res.status, PlatformUi::Context::SEARCH,
                                                                            tr("Search usernames"))};
                                      Q_EMIT self->searchFailed(prefix, error.text, error.details);
                                      return;
                                  }
                                  QVector<QPair<QString, QString>> out;
                                  if (res.ok()) {
                                      out.reserve(res.value->items.size());
                                      for (const auto& name : res.value->items) {
                                          const QString label{QString::fromStdString(name.label)};
                                          const QString id{QString::fromStdString(HexStr(name.identity))};
                                          // Proved, but only remembered for this session: the
                                          // wallet records name contacts, not everyone searched.
                                          self->m_searched_usernames.insert(id, label);
                                          out.append({label, id});
                                      }
                                  }
                                  Q_EMIT self->searchResults(prefix, out);
                                  if (res.ok()) {
                                      for (const auto& name : res.value->items) {
                                          self->loadSearchProfile(name.identity);
                                      }
                                  }
                              });
                          });
}

void PlatformService::loadSearchProfile(const platform::Identifier& identity)
{
    // One proved read per result of the page (at most SEARCH_PAGE_SIZE),
    // each identity once a session: the search has shown the node who they
    // are already.
    const QString id{QString::fromStdString(HexStr(identity))};
    if (!contactMetadata(id, platform::records::CONTACT_DISPLAY_NAME_PREFIX).isEmpty()) {
        Q_EMIT searchProfileLoaded(id, contactMetadata(id, platform::records::CONTACT_DISPLAY_NAME_PREFIX));
        return;
    }
    if (m_searched_display_names.contains(id)) return;
    m_searched_display_names.insert(id, {});
    QPointer<PlatformService> self{this};
    m_client->getProfile(identity, [self, id](platform::Result<platform::Profile> res) {
        if (!self) return;
        self->post([self, id, res = std::move(res)] {
            if (!self) return;
            if (!res.ok()) {
                // Unanswered: asked again by the next search that finds them.
                if (!res.provenAbsent()) self->m_searched_display_names.remove(id);
                return;
            }
            const QString display_name{QString::fromStdString(res.value->display_name)};
            self->m_searched_display_names.insert(id, display_name);
            if (!display_name.isEmpty()) Q_EMIT self->searchProfileLoaded(id, display_name);
        });
    });
}

std::optional<bool> PlatformService::canReceiveContactRequests(const QString& identity_hex) const
{
    const auto it{m_recipients_checked.find(identity_hex)};
    if (it == m_recipients_checked.end()) return std::nullopt;
    return *it;
}

void PlatformService::checkRecipient(const QString& identity_hex)
{
    const auto identity{ParseIdentifier(identity_hex)};
    if (!identity || m_recipients_checked.contains(identity_hex) || m_recipients_checking.contains(identity_hex)) return;
    m_recipients_checking.insert(identity_hex);
    QPointer<PlatformService> self{this};
    m_client->getIdentity(*identity, [self, identity_hex](platform::Result<platform::Identity> result) {
        if (!self) return;
        self->post([self, identity_hex, result = std::move(result)] {
            if (!self) return;
            self->m_recipients_checking.remove(identity_hex);
            // Unanswered: the row stays open and sending says what it finds.
            if (result.ok()) self->recordRecipient(identity_hex, *result.value);
        });
    });
}

void PlatformService::recordRecipient(const QString& identity_hex, const platform::Identity& identity)
{
    const bool can_receive{platform::helpers::Dip15SelectRecipientKey(identity).has_value()};
    m_recipients_checked.insert(identity_hex, can_receive);
    Q_EMIT recipientChecked(identity_hex, can_receive);
}

void PlatformService::loadProfile(const platform::Identifier& identity)
{
    QPointer<PlatformService> self{this};
    m_client->getProfile(identity, [self, identity](platform::Result<platform::Profile> res) {
        if (!self) return;
        self->post([self, identity, res = std::move(res)] {
            if (!self) return;
            const QString id_hex{QString::fromStdString(HexStr(identity))};
            // A newer protocol version still returns the profile to show.
            if (res.value) {
                Q_EMIT self->profileLoaded(id_hex, QString::fromStdString(res.value->display_name),
                                           QString::fromStdString(res.value->public_message), res.value->revision);
            } else if (res.provenAbsent()) {
                Q_EMIT self->profileLoaded(id_hex, {}, {}, 0);
            } else {
                const auto error{PlatformUi::Describe(res.status, PlatformUi::Context::READ, tr("Load profile"))};
                Q_EMIT self->profileLoadFailed(id_hex, error.text, error.details);
            }
        });
    });
}

void PlatformService::loadMyIdentity()
{
    const auto id{myIdentityId()};
    if (!id) return;
    QPointer<PlatformService> self{this};
    m_client->getIdentity(*id, [self](platform::Result<platform::Identity> res) {
        if (!self) return;
        self->post([self, res = std::move(res)] {
            if (!self) return;
            // A newer protocol version still returns the identity to show.
            if (res.value) {
                Q_EMIT self->myIdentityLoaded(*res.value);
            } else {
                const auto error{PlatformUi::Describe(res.status, PlatformUi::Context::READ, tr("Load identity"))};
                Q_EMIT self->myIdentityFailed(error.text, error.details);
            }
        });
    });
}

bool PlatformService::writeRecord(const std::string& key, const std::vector<unsigned char>& value)
{
    if (m_stopped) return false;
    return m_wallet_model.wallet().writePlatformData(key, value);
}

std::vector<unsigned char> PlatformService::readRecord(const std::string& key) const
{
    if (m_stopped) return {};
    auto records{m_wallet_model.wallet().getPlatformData(key)};
    const auto it{records.find(key)};
    return it != records.end() ? it->second : std::vector<unsigned char>{};
}

void PlatformService::post(std::function<void()> fn)
{
    auto run_unless_stopped = [this, fn = std::move(fn)] {
        if (!m_stopped) fn();
    };
    QMetaObject::invokeMethod(this, std::move(run_unless_stopped), Qt::QueuedConnection);
}

void PlatformService::observeStatus(const platform::Status& status, bool verified_read, uint32_t protocol_version)
{
    // The client only verifies a response signed for m_chain_id, so a
    // verified read is what proves the chain id the records belong to.
    const bool verified{verified_read && (status.kind == platform::StatusKind::OK ||
                                          status.kind == platform::StatusKind::PROVEN_ABSENT ||
                                          status.kind == platform::StatusKind::UNSUPPORTED_PROTOCOL_VERSION)};
    if (verified) {
        if (!m_chain_id_stamped && !m_network_changed) {
            m_chain_id_stamped = writeRecord(platform::records::CHAIN_ID, {m_chain_id.begin(), m_chain_id.end()});
        }
        // What a verified read showed, as the SDK ratchets it: never down.
        if (status.kind != platform::StatusKind::UNSUPPORTED_PROTOCOL_VERSION) {
            m_protocol_version = std::max(m_protocol_version, protocol_version);
        }
        if (m_network_mismatch) {
            m_network_mismatch = false;
            LogPrintf("Platform GUI: Platform responses verify for chain %s again\n", m_chain_id);
            Q_EMIT networkMismatchChanged();
        }
    }
    switch (status.kind) {
    case platform::StatusKind::CHAIN_ID_MISMATCH:
        m_network_mismatch_message = status.message;
        if (!m_network_mismatch) {
            m_network_mismatch = true;
            LogPrintf("Platform GUI: %s\n", status.message);
            Q_EMIT networkMismatchChanged();
        }
        break;
    case platform::StatusKind::UNSUPPORTED_PROTOCOL_VERSION:
        if (!m_unsupported_version) {
            m_unsupported_version = true;
            LogPrintf("Platform GUI: %s; writes are disabled until Dash Core is updated\n", status.message);
            Q_EMIT unsupportedProtocolVersion();
        }
        break;
    case platform::StatusKind::REJECTED:
        LogPrintf("Platform GUI: response rejected: %s\n", status.message);
        break;
    case platform::StatusKind::UNAVAILABLE:
        LogPrint(BCLog::PLATFORM, "Platform GUI: Platform unavailable: %s\n", status.message);
        break;
    case platform::StatusKind::INTERNAL:
        LogPrintLevel(BCLog::PLATFORM, BCLog::Level::Warning, "Platform GUI: internal error: %s\n", status.message);
        break;
    case platform::StatusKind::OK:
    case platform::StatusKind::PROVEN_ABSENT:
    case platform::StatusKind::ALREADY_EXISTS:
    case platform::StatusKind::CONSENSUS:
        break;
    }
}

void PlatformService::refreshContacts()
{
    if (m_stopped || !myIdentityId()) return;
    // Without endpoints (network activity off, syncing, or not pushed again
    // yet after a pause) Platform is not asked: the refresh runs when they
    // arrive. One asked while another runs follows it.
    if (!m_have_endpoints || m_contacts_refreshing) {
        m_contacts_refresh_again = true;
        return;
    }
    m_contacts_refresh_again = false;
    m_contacts_refreshing = true;
    m_contacts_partial = false;
    collectContactRequests(/*to_me=*/true, platform::Identifier{}, {});
}

void PlatformService::finishContactsRefresh()
{
    m_contacts_refreshing = false;
    if (m_contacts_refresh_again) refreshContacts();
}

void PlatformService::collectContactRequests(bool to_me, const platform::Identifier& start_after,
                                             std::vector<platform::ContactRequest> collected)
{
    const auto my_id{myIdentityId()};
    if (!my_id) {
        m_contacts_refreshing = false;
        return;
    }
    QPointer<PlatformService> self{this};
    m_client->getContactRequests(
        *my_id, to_me, /*since_ms=*/0, start_after,
        [self, to_me,
         collected = std::move(collected)](platform::Result<platform::Paged<platform::ContactRequest>> res) mutable {
            if (!self) return;
            self->post([self, to_me, collected = std::move(collected), res = std::move(res)]() mutable {
                if (!self) return;
                // A newer protocol version still returns the proved requests
                // to show; only writes are frozen.
                if (!res.value && !res.provenAbsent()) {
                    // The endpoints went away meanwhile, or another refresh
                    // was asked for: not an error, the list is read again.
                    if (!self->m_have_endpoints) self->m_contacts_refresh_again = true;
                    if (!self->m_contacts_refresh_again) {
                        const auto error{
                            PlatformUi::Describe(res.status, PlatformUi::Context::READ, tr("Refresh contacts"))};
                        Q_EMIT self->contactsRefreshFailed(error.text, error.details);
                    }
                    self->finishContactsRefresh();
                    return;
                }
                if (res.value) {
                    collected.insert(collected.end(), res.value->items.begin(), res.value->items.end());
                    if (res.value->has_more && collected.size() < MAX_CONTACT_REQUESTS) {
                        // One page per enqueue: continue from the cursor.
                        self->collectContactRequests(to_me, res.value->next_start_after, std::move(collected));
                        return;
                    }
                    // Past the cap the rest is unread: what was read (the
                    // oldest requests) is shown, but not as the whole list.
                    if (res.value->has_more) self->m_contacts_partial = true;
                }
                if (to_me) {
                    // A sender that sent again (DIP-15 re-send) replaced its
                    // earlier requests: only its newest counts.
                    self->m_incoming_contacts = ContactFlow::NewestPerSender(collected);
                    self->m_incoming_ids.clear();
                    for (const auto& request : self->m_incoming_contacts) {
                        self->m_incoming_ids.insert(QString::fromStdString(HexStr(request.owner_id)));
                    }
                    self->collectContactRequests(/*to_me=*/false, platform::Identifier{}, {});
                    return;
                }
                self->m_outgoing_contacts = std::move(collected);
                self->m_outgoing_ids.clear();
                for (const auto& request : self->m_outgoing_contacts) {
                    self->m_outgoing_ids.insert(QString::fromStdString(HexStr(request.to_user_id)));
                }
                // A new request (or a new one from a known sender) may come
                // with a new name or profile: those are read again now.
                QHash<QString, platform::Identifier> documents;
                for (const auto& cr : self->m_incoming_contacts) {
                    const QString id{QString::fromStdString(HexStr(cr.owner_id))};
                    const auto known{self->m_incoming_documents.find(id)};
                    const bool changed{known == self->m_incoming_documents.end() || *known != cr.document_id};
                    // A newer request may also finish what an older one could not.
                    if (changed) self->m_accepted_refused.remove(id);
                    self->hydrateContactMetadata(cr.owner_id, /*force=*/changed);
                    documents.insert(id, cr.document_id);
                }
                self->m_incoming_documents = std::move(documents);
                for (const auto& cr : self->m_outgoing_contacts) {
                    self->hydrateContactMetadata(cr.to_user_id, /*force=*/false);
                }
                self->publishContacts();
                if (self->m_contacts_partial) {
                    Q_EMIT self->contactsRefreshFailed(
                        tr("There are more contact requests than Dash Core can read, so the newest ones are not "
                           "shown."),
                        tr("Dash Core reads at most %1 contact requests in each direction.").arg(MAX_CONTACT_REQUESTS));
                } else {
                    Q_EMIT self->contactsRefreshed();
                }
                self->completeAcceptedContacts();
                self->finishContactsRefresh();
            });
        });
}

void PlatformService::publishContacts()
{
    QVector<QPair<QString, QString>> incoming, outgoing;
    for (const auto& cr : m_incoming_contacts) {
        const QString id{QString::fromStdString(HexStr(cr.owner_id))};
        incoming.append({id, contactMetadata(id, platform::records::CONTACT_USERNAME_PREFIX)});
    }
    for (const auto& cr : m_outgoing_contacts) {
        const QString id{QString::fromStdString(HexStr(cr.to_user_id))};
        outgoing.append({id, contactMetadata(id, platform::records::CONTACT_USERNAME_PREFIX)});
    }
    // A request Platform is still confirming is listed as sent already.
    const QString pending{pendingContactRequest()};
    if (!pending.isEmpty() && !m_outgoing_ids.contains(pending)) {
        outgoing.append({pending, contactMetadata(pending, platform::records::CONTACT_USERNAME_PREFIX)});
    }
    Q_EMIT contactsUpdated(incoming, outgoing);
}

void PlatformService::completeAcceptedContacts()
{
    // A contact who answered our request is established as the mobile
    // wallets do it: their request is decrypted and the keychains imported,
    // and nothing is broadcast. That needs the wallet's keys, so a locked
    // wallet leaves the contact accepted until the user unlocks it.
    const auto status{m_wallet_model.getEncryptionStatus()};
    if (status == WalletModel::Locked || status == WalletModel::UnlockedForMixingOnly) return;
    for (const auto& request : m_incoming_contacts) {
        const QString id{QString::fromStdString(HexStr(request.owner_id))};
        // A contact whose newest request is not the one we established from
        // sent again with new payment addresses (DIP-15): we pay those now.
        const auto established_from{readRecord(platform::records::CONTACT_IN_PREFIX + id.toStdString())};
        const bool superseded{isEstablished(id) &&
                              !std::equal(established_from.begin(), established_from.end(),
                                          request.document_id.begin(), request.document_id.end())};
        if ((!isAccepted(id) && !superseded) || id == m_contact_request_in_flight || m_completing_contacts.contains(id) ||
            m_accepted_refused.contains(id)) {
            continue;
        }
        if (superseded) {
            LogPrint(BCLog::PLATFORM, "Platform GUI: contact %s sent a newer request; switching to it\n",
                     id.toStdString());
        }
        m_completing_contacts.insert(id);
        m_contact_flow->completeAccepted(request);
    }
}

QString PlatformService::contactMetadata(const QString& identity_hex, const char* prefix) const
{
    const auto bytes{readRecord(prefix + identity_hex.toStdString())};
    if (bytes.empty() && prefix == platform::records::CONTACT_USERNAME_PREFIX) {
        return m_searched_usernames.value(identity_hex);
    }
    if (bytes.empty() && prefix == platform::records::CONTACT_DISPLAY_NAME_PREFIX) {
        return m_searched_display_names.value(identity_hex);
    }
    return QString::fromUtf8(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

QString PlatformService::contactDisplayString(const QString& identity_hex) const
{
    const QString username{contactMetadata(identity_hex, platform::records::CONTACT_USERNAME_PREFIX)};
    if (!username.isEmpty()) return username;
    const QString display_name{contactMetadata(identity_hex, platform::records::CONTACT_DISPLAY_NAME_PREFIX)};
    // A profile name is free text the contact chose; it is never presented
    // as a verified name.
    if (!display_name.isEmpty()) return tr("“%1” (profile name)").arg(display_name);
    return PlatformUi::identityIdBase58(identity_hex).left(8) + QStringLiteral("…");
}

QString PlatformService::contactAddressLabel(const QString& identity_hex) const
{
    return tr("%1 (DashPay)").arg(contactDisplayString(identity_hex));
}

bool PlatformService::isAccepted(const QString& identity_hex) const
{
    return m_incoming_ids.contains(identity_hex) && m_outgoing_ids.contains(identity_hex) && !isEstablished(identity_hex);
}

QString PlatformService::contactRequestBlocker() const
{
    if (m_contact_request_pending) {
        return tr("Dash Platform is still confirming your last contact request. Try again in a minute.");
    }
    if (!m_contact_request_in_flight.isEmpty())
        return tr("Another contact request is still being sent. Wait for it to finish.");
    if (m_profile_update_in_flight) {
        return tr("Your profile change is still being saved to Dash Platform. Try again in a minute.");
    }
    // It was signed with the identity's first DashPay nonce.
    if (m_identity_flow->profilePending()) {
        return tr("Your profile is still being published to Dash Platform. Try again in a minute.");
    }
    return {};
}

bool PlatformService::isHidden(const QString& identity_hex) const
{
    return !readRecord(platform::records::CONTACT_HIDDEN_PREFIX + identity_hex.toStdString()).empty();
}

void PlatformService::setHidden(const QString& identity_hex, bool hidden)
{
    if (isHidden(identity_hex) == hidden) return;
    // An empty value deletes the record.
    writeRecord(platform::records::CONTACT_HIDDEN_PREFIX + identity_hex.toStdString(),
                hidden ? std::vector<unsigned char>{1} : std::vector<unsigned char>{});
    Q_EMIT hiddenContactsChanged();
}

bool PlatformService::sentRequestTo(const QString& identity_hex) const
{
    return m_outgoing_ids.contains(identity_hex) || pendingContactRequest() == identity_hex ||
           !readRecord(platform::records::CONTACT_OUT_PREFIX + identity_hex.toStdString()).empty();
}

bool PlatformService::isEstablished(const QString& identity_hex) const
{
    return readRecord(platform::records::CONTACT_KEY_PREFIX + identity_hex.toStdString()).size() ==
               wallet::COMPACT_XPUB_SIZE &&
           !readRecord(platform::records::CONTACT_OUT_PREFIX + identity_hex.toStdString()).empty();
}

std::optional<int64_t> PlatformService::outgoingRequestTime(const platform::Identifier& identity) const
{
    const auto it{std::find_if(m_outgoing_contacts.begin(), m_outgoing_contacts.end(),
                               [&identity](const auto& request) { return request.to_user_id == identity; })};
    if (it == m_outgoing_contacts.end()) return std::nullopt;
    return static_cast<int64_t>(it->created_at / 1000);
}

void PlatformService::setContactMetadata(const QString& identity_hex, const char* prefix, const QString& value)
{
    const QByteArray bytes{value.toUtf8()};
    writeRecord(prefix + identity_hex.toStdString(), {bytes.begin(), bytes.end()});
}

void PlatformService::hydrateContactMetadata(const platform::Identifier& identity, bool force)
{
    const QString id{QString::fromStdString(HexStr(identity))};
    if (m_contact_metadata_pending.contains(id)) return;
    const auto fetched_at{m_contact_metadata_fetched_at.find(id)};
    if (!force && fetched_at != m_contact_metadata_fetched_at.end() &&
        GetTime() - *fetched_at < CONTACT_METADATA_TTL_SECONDS) {
        return;
    }
    m_contact_metadata_pending.insert(id);

    auto remaining{std::make_shared<int>(2)};
    auto dirty{std::make_shared<bool>(false)};
    auto proved{std::make_shared<bool>(true)};
    QPointer<PlatformService> self{this};
    const auto finished = [self, id, remaining, dirty, proved](bool changed, bool answered) {
        if (!self) return;
        *dirty |= changed;
        *proved &= answered;
        if (--*remaining != 0) return;
        self->m_contact_metadata_pending.remove(id);
        if (*proved) self->m_contact_metadata_fetched_at.insert(id, GetTime());
        // The rows read both from the records just written.
        if (*dirty) self->publishContacts();
    };

    // Both reads are proved before anything is cached: a visible label is
    // never derived from an unverified document.
    m_client->namesOfIdentity(
        identity, platform::Identifier{},
        [self, id, finished](platform::Result<platform::Paged<platform::DpnsName>> result) {
            if (!self) return;
            self->post([self, id, finished, result = std::move(result)] {
                if (!self) return;
                bool changed{false};
                if (result.value && !result.value->items.empty()) {
                    const auto& names{result.value->items};
                    const auto it{std::min_element(names.begin(), names.end(), [](const auto& a, const auto& b) {
                        return a.normalized_label < b.normalized_label;
                    })};
                    const QString label{QString::fromStdString(it->label)};
                    if (self->contactMetadata(id, platform::records::CONTACT_USERNAME_PREFIX) != label) {
                        self->setContactMetadata(id, platform::records::CONTACT_USERNAME_PREFIX, label);
                        changed = true;
                    }
                }
                finished(changed, result.value.has_value() || result.provenAbsent());
            });
        });
    m_client->getProfile(identity, [self, id, finished](platform::Result<platform::Profile> result) {
        if (!self) return;
        self->post([self, id, finished, result = std::move(result)] {
            if (!self) return;
            bool changed{false};
            if (result.value || result.provenAbsent()) {
                const QString display_name{result.value ? QString::fromStdString(result.value->display_name) : QString{}};
                if (self->contactMetadata(id, platform::records::CONTACT_DISPLAY_NAME_PREFIX) != display_name) {
                    self->setContactMetadata(id, platform::records::CONTACT_DISPLAY_NAME_PREFIX, display_name);
                    changed = true;
                }
            }
            finished(changed, result.value.has_value() || result.provenAbsent());
        });
    });
}

void PlatformService::finishContactRequest(const QString& identity_hex, bool ok, const QString& error, const QString& details)
{
    if (m_contact_request_in_flight != identity_hex) return;
    m_contact_request_in_flight.clear();
    m_contact_request_pending = false;
    Q_EMIT contactRequestFinished(identity_hex, ok, error, details);
}

bool PlatformService::sendContactRequest(const QString& identity_hex, QString& error)
{
    const auto identity{ParseIdentifier(identity_hex)};
    if (!identity) {
        error = tr("This is not a DashPay user Dash Core can send a request to.");
        return false;
    }
    error = contactRequestBlocker();
    if (!error.isEmpty()) return false;
    if (!writesAllowed(error)) return false;
    // Asking again is how an ignored person comes back.
    setHidden(identity_hex, false);
    m_contact_request_in_flight = identity_hex;
    QPointer<PlatformService> self{this};
    m_client->getIdentity(*identity, [self, identity_hex](platform::Result<platform::Identity> result) {
        if (!self) return;
        self->post([self, identity_hex, result = std::move(result)] {
            if (!self) return;
            if (result.provenAbsent()) {
                self->finishContactRequest(identity_hex, false,
                                           tr("This DashPay user no longer exists on Dash Platform."), {});
                return;
            }
            if (!result.ok()) {
                const auto error{PlatformUi::Describe(result.status, PlatformUi::Context::CONTACT_REQUEST,
                                                      tr("Read recipient identity"))};
                self->finishContactRequest(identity_hex, false, error.text, error.details);
                return;
            }
            self->recordRecipient(identity_hex, *result.value);
            self->m_contact_flow->sendRequest(*result.value);
        });
    });
    return true;
}

bool PlatformService::acceptContact(const QString& identity_hex, QString& error)
{
    const auto it{std::find_if(m_incoming_contacts.begin(), m_incoming_contacts.end(), [&identity_hex](const auto& request) {
        return QString::fromStdString(HexStr(request.owner_id)) == identity_hex;
    })};
    if (it == m_incoming_contacts.end()) {
        error = tr("This request is no longer available. Refresh your contacts.");
        return false;
    }
    error = contactRequestBlocker();
    if (!error.isEmpty()) return false;
    if (!writesAllowed(error)) return false;
    m_contact_request_in_flight = identity_hex;
    m_contact_flow->accept(*it);
    return true;
}

void PlatformService::resolvePaymentAddress(const QString& username)
{
    if (!networkActive()) {
        Q_EMIT paymentAddressResolved(username, {}, {},
                                      tr("DashPay is paused while network activity is turned off. Turn it on to pay "
                                         "by username."),
                                      {});
        return;
    }
    const std::string normalized{platform::helpers::NormalizeLabel(username.toStdString())};
    QPointer<PlatformService> self{this};
    m_client->resolveName(normalized, [self, username](platform::Result<platform::DpnsName> result) {
        if (!self) return;
        self->post([self, username, result = std::move(result)] {
            if (!self) return;
            if (result.provenAbsent()) {
                Q_EMIT self->paymentAddressResolved(username, {}, {},
                                                    tr("No DashPay user has the username “%1”.").arg(username), {});
                return;
            }
            if (!result.ok()) {
                const auto error{
                    PlatformUi::Describe(result.status, PlatformUi::Context::PAYMENT_LOOKUP, tr("Look up username"))};
                Q_EMIT self->paymentAddressResolved(username, {}, {}, error.text, error.details);
                return;
            }
            const QString label{QString::fromStdString(result.value->label)};
            if (self->myIdentityId() == result.value->identity) {
                Q_EMIT self->paymentAddressResolved(username, label, {},
                                                    tr("“%1” is your own DashPay username. To move Dash within this "
                                                       "wallet, use one of your own receiving addresses.")
                                                        .arg(label),
                                                    {});
                return;
            }
            const std::string id_hex{HexStr(result.value->identity)};
            const auto serialized{self->readRecord(platform::records::CONTACT_KEY_PREFIX + id_hex)};
            if (serialized.size() != wallet::COMPACT_XPUB_SIZE ||
                self->readRecord(platform::records::CONTACT_OUT_PREFIX + id_hex).empty()) {
                Q_EMIT self->paymentAddressResolved(username, label, {},
                                                    tr("You and “%1” are not contacts yet. Add them on the DashPay tab; "
                                                       "once they accept, you can pay them by username.")
                                                        .arg(label),
                                                    {});
                return;
            }
            wallet::CompactXpub compact;
            std::copy(serialized.begin(), serialized.end(), compact.begin());
            const wallet::FriendshipXpub contact_xpub{FriendshipXpubFromCompact(compact)};
            const std::string cursor_key{platform::records::CONTACT_PAY_INDEX_PREFIX + id_hex};
            const uint32_t index{platform::DecodePaymentCursor(self->readRecord(cursor_key))};
            CTxDestination destination;
            if (!wallet::DeriveFriendshipPaymentDestination(contact_xpub, index, destination)) {
                Q_EMIT self->paymentAddressResolved(
                    username, label, {},
                    tr("Dash Core could not create a payment address for this contact. "
                       "Please report this; Show details has more."),
                    QStringLiteral("Operation: Derive contact payment address\nIndex: %1").arg(index));
                return;
            }
            const QString address{QString::fromStdString(EncodeDestination(destination))};
            self->m_payment_reservations.insert(address, {QString::fromStdString(cursor_key), index});
            // Label the derived destination so transaction history shows
            // the contact's username instead of a bare address.
            self->m_wallet_model.wallet().setAddressBook(
                destination, self->contactAddressLabel(QString::fromStdString(id_hex)).toStdString(), "send");
            Q_EMIT self->paymentAddressResolved(username, label, address, {}, {});
        });
    });
}

void PlatformService::commitPaymentAddress(const QString& address)
{
    const auto it{m_payment_reservations.find(address)};
    if (it == m_payment_reservations.end()) return;
    const auto [cursor_key, index]{it.value()};
    const uint32_t current{platform::DecodePaymentCursor(readRecord(cursor_key.toStdString()))};
    if (current == index) writeRecord(cursor_key.toStdString(), platform::EncodePaymentCursor(index + 1));
    m_payment_reservations.erase(it);
}

void PlatformService::finishProfileUpdate(bool ok, const QString& error, const QString& details)
{
    if (!m_profile_update_in_flight) return;
    m_profile_update_in_flight = false;
    Q_EMIT profileUpdated(ok, error, details);
}

void PlatformService::failProfileUpdate(const platform::Status& status, const QString& operation)
{
    const auto error{PlatformUi::Describe(status, PlatformUi::Context::PROFILE, operation)};
    finishProfileUpdate(false, error.text, error.details);
}

bool PlatformService::updateProfile(const QString& display_name, const QString& public_message, QString& error)
{
    const auto my_id{myIdentityId()};
    if (!my_id || m_identity_flow->record().state != IdentityFlow::State::REGISTERED) {
        error = tr("Register a username before creating a profile.");
        return false;
    }
    if (display_name.size() > MAX_DISPLAY_NAME_LENGTH) {
        error = tr("The display name can be at most %1 characters long.").arg(MAX_DISPLAY_NAME_LENGTH);
        return false;
    }
    if (public_message.size() > MAX_PUBLIC_MESSAGE_LENGTH) {
        error = tr("The public message can be at most %1 characters long.").arg(MAX_PUBLIC_MESSAGE_LENGTH);
        return false;
    }
    if (m_profile_update_in_flight) {
        error = tr("Your previous profile change is still being saved.");
        return false;
    }
    // A contact request signs with the same DashPay nonce: one at a time.
    error = contactRequestBlocker();
    if (!error.isEmpty()) return false;
    if (!writesAllowed(error)) return false;
    m_profile_update_in_flight = true;
    platform::ProfileInput input;
    input.display_name = display_name.toStdString();
    input.public_message = public_message.toStdString();

    // A replace carries the whole document, so the current profile is read
    // (proved) first and every field the dialog does not edit is kept.
    QPointer<PlatformService> self{this};
    m_client->getIdentity(*my_id, [self, input](platform::Result<platform::Identity> identity_res) {
        if (!self) return;
        self->post([self, input, identity_res = std::move(identity_res)] {
            if (!self) return;
            if (!identity_res.ok()) {
                self->failProfileUpdate(identity_res.status, tr("Read own identity"));
                return;
            }
            const platform::Identity identity{*identity_res.value};
            self->m_client->getProfile(identity.id, [self, identity, input](platform::Result<platform::Profile> profile_res) {
                if (!self) return;
                self->post([self, identity, input, profile_res = std::move(profile_res)] {
                    if (!self) return;
                    if (!profile_res.ok() && !profile_res.provenAbsent()) {
                        self->failProfileUpdate(profile_res.status, tr("Read current profile"));
                        return;
                    }
                    self->publishProfile(identity, profile_res.ok() ? *profile_res.value : platform::Profile{}, input);
                });
            });
        });
    });
    return true;
}

void PlatformService::publishProfile(const platform::Identity& identity, const platform::Profile& existing,
                                     const platform::ProfileInput& input)
{
    const auto dashpay{platform::helpers::SystemContractId(platform::helpers::SystemContract::DASHPAY)};
    QPointer<PlatformService> self{this};
    m_client->getIdentityContractNonce(identity.id, dashpay, [self, identity, existing, input](platform::Result<uint64_t> nonce_res) {
        if (!self) return;
        self->post([self, identity, existing, input, nonce_res = std::move(nonce_res)] {
            if (!self) return;
            if (!nonce_res.ok() && !nonce_res.provenAbsent()) {
                self->failProfileUpdate(nonce_res.status, tr("Read identity nonce"));
                return;
            }
            const uint64_t nonce{nonce_res.ok() ? *nonce_res.value + 1 : 1};
            const auto key{self->documentSigningKey(identity)};
            if (!key) {
                self->finishProfileUpdate(false, tr("This wallet holds no key that can sign for your identity."), {});
                return;
            }
            auto attempt{self->beginSigningOperation(platform::OperationKind::PROFILE, {key->id}, *key, std::nullopt)};
            if (!attempt.op) {
                self->finishProfileUpdate(false,
                                          attempt.unlock_declined ? tr("The wallet stayed locked, so nothing was sent. "
                                                                       "Try again and enter your passphrase.")
                                                                  : attempt.refusal,
                                          {});
                return;
            }
            auto built{self->m_client->buildProfile(*attempt.op, identity.id, nonce, existing, input)};
            attempt.op.reset();
            if (!built) {
                self->failProfileUpdate({platform::StatusKind::INTERNAL, 0, util::ErrorString(built).original},
                                        tr("Build profile"));
                return;
            }
            const uint64_t revision{existing.revision + 1};
            self->m_client->broadcastStateTransition(built->bytes, [self, identity, revision,
                                                                    input](platform::Status status) {
                if (!self) return;
                self->post([self, identity, revision, input, status = std::move(status)] {
                    if (!self) return;
                    LogPrint(BCLog::PLATFORM, "Platform GUI: profile broadcast: kind=%d code=%u %s\n",
                             static_cast<int>(status.kind), status.consensus_code, status.message);
                    if (status.kind != platform::StatusKind::OK && status.kind != platform::StatusKind::ALREADY_EXISTS) {
                        self->failProfileUpdate(status, tr("Save profile"));
                        return;
                    }
                    self->confirmProfile(identity.id, revision, input, PROFILE_CONFIRM_ATTEMPTS);
                });
            });
        });
    });
}

void PlatformService::confirmProfile(const platform::Identifier& owner, uint64_t revision,
                                     const platform::ProfileInput& input, int attempts_left)
{
    QPointer<PlatformService> self{this};
    m_client->getProfile(owner, [self, owner, revision, input, attempts_left](platform::Result<platform::Profile> proved) {
        if (!self) return;
        self->post([self, owner, revision, input, attempts_left, proved = std::move(proved)] {
            if (!self) return;
            if (proved.ok() && proved.value->revision == revision && proved.value->display_name == input.display_name &&
                proved.value->public_message == input.public_message) {
                self->finishProfileUpdate(true, {}, {});
                return;
            }
            if (attempts_left <= 1) {
                LogPrint(BCLog::PLATFORM, "Platform GUI: profile revision %u not confirmed\n", revision);
                self->finishProfileUpdate(false,
                                          tr("Dash Platform did not confirm your profile change. Reopen your "
                                             "profile in a minute to check whether it was saved."),
                                          {});
                return;
            }
            QTimer::singleShot(PROFILE_CONFIRM_INTERVAL_MS, self, [self, owner, revision, input, attempts_left] {
                if (self) self->confirmProfile(owner, revision, input, attempts_left - 1);
            });
        });
    });
}

void PlatformService::updateNodeContext()
{
    if (m_stopped) return;
    if (m_context_refresh_pending) {
        // The collection under way may have read the network state before
        // this change: collect again once it lands.
        m_context_refresh_again = true;
        return;
    }
    m_context_refresh_pending = true;
    interfaces::Node& node{m_client_model.node()};
    // Evonode DAPI endpoints from the deterministic masternode list; none
    // while network activity is disabled (so no connection is attempted) or
    // the node is syncing (the list at a stale tip is not the network's).
    const bool collect_endpoints{node.getNetworkActive() && !node.isInitialBlockDownload()};
    const auto llmq_type{static_cast<uint8_t>(Params().GetConsensus().llmqTypePlatform)};
    QMetaObject::invokeMethod(m_context_worker, [this, &node, collect_endpoints, llmq_type] {
        NodeContext context;
        context.llmq_type = llmq_type;
        if (collect_endpoints) {
            if (const auto mn_list{node.evo().getListAtChainTip().first}) {
                mn_list->forEachMN(/*only_valid=*/true, [&context](const auto& dmn) {
                    for (const auto& service : dmn->getPlatformHTTPSAddrs()) {
                        context.endpoints.push_back(platform::Endpoint{service});
                    }
                });
            }
        }
        context.chainlock_height = node.llmq().getBestChainLock().m_height;
        for (auto& quorum : node.llmq().getPlatformQuorums(llmq_type)) {
            context.quorum_keys.push_back(platform::QuorumKey{quorum.m_quorum_hash, std::move(quorum.m_pubkey)});
        }
        post([this, context = std::move(context)]() mutable { applyNodeContext(std::move(context)); });
    });
}

void PlatformService::setEndpointsForTesting(std::vector<platform::Endpoint> endpoints)
{
    pushEndpoints(std::move(endpoints));
}

void PlatformService::applyNodeContext(NodeContext context)
{
    m_context_refresh_pending = false;
    if (std::exchange(m_context_refresh_again, false)) {
        // Collected before a change of the network state (activity turned
        // off meanwhile): only the collection made after it is pushed.
        updateNodeContext();
        return;
    }
    m_client->updateCoreChainLockedHeight(context.chainlock_height);
    m_client->updateQuorumKeys(context.llmq_type, std::move(context.quorum_keys));
    pushEndpoints(std::move(context.endpoints));
}

void PlatformService::pushEndpoints(std::vector<platform::Endpoint> endpoints)
{
    // Only the evonodes the client's route reaches.
    const size_t collected{endpoints.size()};
    std::erase_if(endpoints, [this](const platform::Endpoint& endpoint) { return !m_route.allows(endpoint.service); });
    const bool had_endpoints{std::exchange(m_have_endpoints, !endpoints.empty())};
    LogPrint(BCLog::PLATFORM, "Platform GUI: pushing %d evonode HTTPS endpoints (%d not on its route)\n",
             endpoints.size(), collected - endpoints.size());
    m_client->updateEndpoints(std::move(endpoints));
    const bool unreachable{!m_have_endpoints && collected > 0};
    if (std::exchange(m_unreachable, unreachable) != unreachable) Q_EMIT reachabilityChanged();
    // Reads made while there were none can be made again now.
    if (m_have_endpoints && !had_endpoints) {
        Q_EMIT endpointsAvailable();
        if (m_contacts_refresh_again) refreshContacts();
    }

    // Seed-only recovery needs endpoints (and their quorum keys) to issue
    // proof-backed queries, so it is armed from here rather than the
    // constructor. maybeStart() is a cheap no-op once it has run.
    if (m_have_endpoints && !m_network_changed) m_recovery->maybeStart();
}
