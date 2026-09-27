// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/platform/platformservice.h>

#include <chainparams.h>
#include <interfaces/handler.h>
#include <interfaces/node.h>
#include <interfaces/wallet.h>
#include <logging.h>
#include <netbase.h>
#include <platform/helpers.h>
#include <platform/walletrecords.h>
#include <qt/clientmodel.h>
#include <qt/platform/identityflow.h>
#include <qt/platform/platformui.h>
#include <util/strencodings.h>
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
//! Base duffs an identity registration locks, before any vote reserve.
//! Matches the mobile wallets' default for an uncontested registration.
constexpr CAmount IDENTITY_FUNDING_DUFFS{1000000};

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
//! before the flow sees it, so a protocol-version signal on any read
//! freezes writes no matter which flow issued it.
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

//! Why writes are refused while the service is frozen.
QString RefusalText(platform::StatusKind kind)
{
    platform::Status status;
    status.kind = kind;
    return PlatformUi::Describe(status, PlatformUi::Context::READ, {}).text;
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

PlatformAvailability PlatformService::Availability(WalletModel& wallet_model, ClientModel& client_model,
                                                   const PlatformService* service)
{
    using Gate = PlatformAvailability::Gate;
    PlatformAvailability out;
    interfaces::Wallet& wallet{wallet_model.wallet()};
    out.enabled = IsEnabled(wallet);
    if (wallet.isLegacy()) {
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
    m_route(std::move(route))
{
    // Records of another layout are never migrated: wipe them and let
    // recovery rebuild the state from chain.
    const bool have_records{!m_wallet_model.wallet().getPlatformData("identity/").empty() ||
                            !m_wallet_model.wallet().getPlatformData("contact/").empty()};
    if (!platform::IsRecordSetCurrent(readRecord(platform::records::VERSION), have_records)) {
        LogPrintf("Platform GUI: wallet records use another layout version; discarding them for recovery\n");
        DiscardRecords(m_wallet_model.wallet());
    }

    m_identity_flow = std::make_unique<IdentityFlow>(*this);
    connect(m_identity_flow.get(), &IdentityFlow::stateChanged, this, &PlatformService::identityStateChanged);
    connect(m_identity_flow.get(), &IdentityFlow::failed, this, &PlatformService::flowFailed);

    m_tick_timer = new QTimer(this);
    m_tick_timer->setInterval(TICK_INTERVAL_MS);
    connect(m_tick_timer, &QTimer::timeout, this, [this] { m_identity_flow->advance(); });
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
    // A wallet unlock is the user action a parked flow waits for.
    connect(&m_wallet_model, &WalletModel::encryptionStatusChanged, this, [this] {
        if (m_wallet_model.getEncryptionStatus() == WalletModel::Unlocked) m_identity_flow->retryAfterUnlock();
    });

    updateNodeContext();
    m_identity_flow->advance();
}

PlatformService::~PlatformService()
{
    m_chainlock_handler.reset();
    // The flows hold callbacks into the client; stop it before they go.
    // A collection still running on the worker finishes before the object
    // it reports to goes away; its queued report is dropped with the object.
    m_context_thread->quit();
    m_context_thread->wait();
    delete m_context_worker;
    m_client->shutdown();
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

bool PlatformService::writesAllowed(QString& error) const
{
    if (m_unsupported_version) {
        error = RefusalText(platform::StatusKind::UNSUPPORTED_PROTOCOL_VERSION);
        return false;
    }
    return true;
}

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
            if (res.value) {
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
            if (res.value) {
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
                    } else if (vote_res.value) {
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
            if (res.value) {
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

bool PlatformService::networkActive() const { return m_client_model.node().getNetworkActive(); }

void PlatformService::observeStatus(const platform::Status& status, bool verified_read, uint32_t protocol_version)
{
    // What a verified read showed, as the SDK ratchets it: never down.
    const bool verified{verified_read && (status.kind == platform::StatusKind::OK ||
                                          status.kind == platform::StatusKind::PROVEN_ABSENT)};
    if (verified) {
        m_protocol_version = std::max(m_protocol_version, protocol_version);
    }
    switch (status.kind) {
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

void PlatformService::updateNodeContext()
{
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

void PlatformService::setEndpointsForTesting(std::vector<platform::Endpoint> endpoints)
{
    pushEndpoints(std::move(endpoints));
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
    if (m_have_endpoints && !had_endpoints) Q_EMIT endpointsAvailable();
}
