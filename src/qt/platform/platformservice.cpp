// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/platform/platformservice.h>

#include <interfaces/node.h>
#include <interfaces/wallet.h>
#include <net.h>
#include <netbase.h>
#include <qt/clientfeeds.h>
#include <qt/clientmodel.h>
#include <qt/masternodemodel.h>
#include <qt/walletmodel.h>
#include <util/check.h>
#include <util/system.h>
#include <wallet/wallet.h>

#include <QMetaObject>

#include <algorithm>
#include <utility>

namespace {
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
    PlatformNetworkSettings settings;
    settings.ipv4 = node.isReachable(NET_IPV4);
    settings.ipv6 = node.isReachable(NET_IPV6);
    settings.onion = node.isReachable(NET_ONION);
    // -proxy sets IPv4 and IPv6 together.
    settings.clearnet_proxy = ReadProxy(node, NET_IPV4);
    settings.onion_proxy = ReadProxy(node, NET_ONION);
    return settings;
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
    if (settings.ipv4 || settings.ipv6) {
        return PlatformRoute{settings.clearnet_proxy, settings.ipv4, settings.ipv6,
                             settings.onion && settings.onion_proxy && settings.onion_proxy == settings.clearnet_proxy};
    }
    if (settings.onion && settings.onion_proxy) {
        return PlatformRoute{settings.onion_proxy, /*ipv4=*/false, /*ipv6=*/false, /*onion=*/true};
    }
    return std::nullopt;
}

PlatformService::PlatformService(WalletModel& wallet_model, ClientModel& client_model, Sources sources) :
    m_wallet_model(wallet_model),
    m_client_model(client_model),
    m_sources(std::move(sources))
{
    interfaces::Node& node{m_client_model.node()};
    if (!m_sources.make_client) {
        m_sources.make_client = [&node](const platform::ClientConfig& config, platform::Executor executor) {
            return platform::MakeSdkPlatformClient(config, node.llmq(), std::move(executor));
        };
    }
    if (!m_sources.evonodes) {
        m_sources.evonodes = [feed = m_client_model.feedMasternode()] {
            std::vector<CService> endpoints;
            if (const auto data{feed->data()}) {
                for (const auto& entry : data->m_entries) {
                    if (entry->isBanned()) continue;
                    for (const CService& endpoint : entry->platformHTTPSAddrsRaw()) {
                        endpoints.push_back(endpoint);
                    }
                }
            }
            return endpoints;
        };
    }
    if (!m_sources.masternodes) {
        m_sources.masternodes = [feed = m_client_model.feedMasternode()] {
            std::vector<Masternode> masternodes;
            if (const auto data{feed->data()}) {
                for (const auto& entry : data->m_entries) {
                    if (entry->isBanned()) continue;
                    masternodes.push_back({entry->proTxHashRaw(), entry->keyIdVotingRaw(), entry->type() == MnType::Evo});
                }
            }
            return masternodes;
        };
    }
    if (!m_sources.network_settings) {
        m_sources.network_settings = [&node] { return PlatformNetworkSettings::Read(node); };
    }

    interfaces::Wallet& wallet{m_wallet_model.wallet()};
    m_eligible = wallet.platformKeySourceStatus() == wallet::PlatformKeyStatus::SUCCESS;
    m_consent = wallet.getPlatformData(CONSENT_RECORD).contains(CONSENT_RECORD);

    // Queued: the wallet notifies with cs_wallet held, and update() may
    // destroy a client, which must not happen under it.
    connect(
        &m_wallet_model, &WalletModel::canGetAddressesChanged, this,
        [this] {
            m_eligible = m_wallet_model.wallet().platformKeySourceStatus() == wallet::PlatformKeyStatus::SUCCESS;
            update();
        },
        Qt::QueuedConnection);
    connect(&m_client_model, &ClientModel::networkActiveChanged, this, &PlatformService::update);
    // The masternode feed reads the list again on ClientModel::masternodeListChanged
    // and on every block, off the GUI thread.
    connect(m_client_model.feedMasternode(), &FeedBase::dataReady, this, &PlatformService::update);
    update();
}

QString PlatformService::reason() const
{
    switch (m_gate) {
    case Gate::NONE:
    case Gate::NO_CONSENT:
        return {};
    case Gate::WALLET:
        return tr("This wallet can't use DashPay: DashPay needs a descriptor wallet that holds its own keys, from "
                  "one recovery phrase.");
    case Gate::OUTBOUND_DISABLED:
        return tr("DashPay is off because Dash Core makes no automatic outbound connections (-connect or "
                  "-maxconnections=0). Remove that setting to use DashPay.");
    case Gate::BROADCAST_DISABLED:
        return tr("DashPay is off because this wallet does not broadcast its transactions (-walletbroadcast=0). "
                  "Remove that setting to use DashPay.");
    case Gate::NO_ROUTE:
        return tr("Your network settings leave DashPay no network to use. DashPay needs IPv4 or IPv6, directly or "
                  "through your proxy, or Tor with an onion proxy.");
    case Gate::NETWORK_OFF:
        return tr("DashPay is paused while network activity is off.");
    case Gate::SYNCING:
        return tr("DashPay starts once Dash Core has finished syncing.");
    case Gate::NO_EVONODES:
        return tr("DashPay is waiting for an evonode it can reach with your network settings.");
    case Gate::CLIENT_FAILED:
        return tr("DashPay couldn't start with your network settings. The debug log has details.");
    }
    return {};
}

bool PlatformService::start()
{
    if (!m_wallet_model.wallet().writePlatformData(CONSENT_RECORD, {1})) return false;
    m_consent = true;
    refresh();
    Q_EMIT changed();
    return true;
}

bool PlatformService::stop()
{
    if (!m_wallet_model.wallet().writePlatformData(CONSENT_RECORD, {})) return false;
    m_consent = false;
    refresh();
    Q_EMIT changed();
    return true;
}

PlatformService::Gate PlatformService::evaluate(platform::ClientConfig& config) const
{
    if (!m_eligible) return Gate::WALLET;
    // As init.cpp's parameter interaction: no automatic outbound connections.
    if (gArgs.IsArgSet("-connect") || gArgs.GetIntArg("-maxconnections", DEFAULT_MAX_PEER_CONNECTIONS) <= 0) {
        return Gate::OUTBOUND_DISABLED;
    }
    if (!gArgs.GetBoolArg("-walletbroadcast", wallet::DEFAULT_WALLETBROADCAST)) return Gate::BROADCAST_DISABLED;
    const auto route{PlatformRoute::Choose(m_sources.network_settings())};
    if (!route) return Gate::NO_ROUTE;
    if (!m_consent) return Gate::NO_CONSENT;
    interfaces::Node& node{m_client_model.node()};
    if (!node.getNetworkActive()) return Gate::NETWORK_OFF;
    if (node.isInitialBlockDownload()) return Gate::SYNCING;

    config.proxy = route->proxy;
    for (const CService& endpoint : m_sources.evonodes()) {
        if (route->allows(endpoint)) config.endpoints.push_back(endpoint);
    }
    std::sort(config.endpoints.begin(), config.endpoints.end());
    config.endpoints.erase(std::unique(config.endpoints.begin(), config.endpoints.end()), config.endpoints.end());
    if (config.endpoints.empty()) return Gate::NO_EVONODES;
    if (config == m_refused) return Gate::CLIENT_FAILED;
    return Gate::NONE;
}

void PlatformService::update()
{
    if (refresh()) Q_EMIT changed();
}

bool PlatformService::refresh()
{
    platform::ClientConfig config;
    const Gate gate{evaluate(config)};
    if (gate == Gate::NONE && m_client && config == m_config) {
        // Back to the set the client has: whatever paused it is moot.
        return std::exchange(m_paused, false);
    }
    if (m_writes > 0 && (gate == Gate::NONE || gate == Gate::NO_EVONODES || gate == Gate::CLIENT_FAILED)) {
        // A changed evonode set: no new call starts, and the write in flight
        // finishes on the old client (writeFinished() comes back here).
        return !std::exchange(m_paused, true);
    }
    const bool had_client{m_client != nullptr};
    const Gate old_gate{std::exchange(m_gate, gate)};
    m_client.reset();
    m_writes = 0;
    m_paused = false;
    if (gate == Gate::NONE) {
        m_client = m_sources.make_client(config, [this](std::function<void()> task) {
            QMetaObject::invokeMethod(this, std::move(task), Qt::QueuedConnection);
        });
        if (m_client) {
            m_config = std::move(config);
        } else {
            m_refused = std::move(config);
            m_gate = Gate::CLIENT_FAILED;
        }
    }
    return had_client || m_client || m_gate != old_gate;
}

bool PlatformService::othersIdle(const QObject& flow) const
{
    return !writing() && std::all_of(m_writers.begin(), m_writers.end(), [&flow](const auto& writer) {
        return writer.first == &flow || writer.second();
    });
}

void PlatformService::writeFinished()
{
    Assume(m_writes > 0);
    // Queued: the client delivering this callback is the one a rebuild destroys.
    if (--m_writes == 0 && m_paused) QMetaObject::invokeMethod(this, &PlatformService::update, Qt::QueuedConnection);
}
