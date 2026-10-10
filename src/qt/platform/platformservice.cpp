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
#include <platform/walletrecords.h>
#include <qt/clientmodel.h>
#include <util/threadnames.h>

#include <QMetaObject>

#include <utility>

namespace {
//! Endpoint + quorum key refresh cadence.
constexpr int CONTEXT_INTERVAL_MS{60'000};

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

bool PlatformService::WipeRecords(interfaces::Wallet& wallet)
{
    bool ok{true};
    for (const char* prefix : {"platform/", "identity/", "contact/"}) {
        for (const auto& [key, value] : wallet.getPlatformData(prefix)) {
            ok &= wallet.writePlatformData(key, {});
        }
    }
    return ok;
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
    m_client(std::move(client)),
    m_route(std::move(route))
{
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

    updateNodeContext();
}

PlatformService::~PlatformService()
{
    m_chainlock_handler.reset();
    // A collection still running on the worker finishes before the object
    // it reports to goes away; its queued report is dropped with the object.
    m_context_thread->quit();
    m_context_thread->wait();
    delete m_context_worker;
    m_client->shutdown();
}

std::optional<platform::SigningOperation> PlatformService::beginSigningOperation(
    platform::OperationKind kind, std::vector<uint32_t> key_ids,
    std::optional<platform::IdentityPublicKey> document_key, std::optional<wallet::RegistrationFundingKey> funding_key)
{
    if (m_unsupported_version) return std::nullopt;
    WalletModel::UnlockContext unlock{m_wallet_model.requestUnlock()};
    if (!unlock.isValid()) return std::nullopt;
    return platform::SigningOperation(m_wallet_model.wallet(), kind, std::move(key_ids), std::move(document_key),
                                      funding_key, std::make_unique<UnlockScope>(std::move(unlock)));
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

void PlatformService::observeStatus(const platform::Status& status)
{
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
