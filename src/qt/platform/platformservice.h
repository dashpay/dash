// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_QT_PLATFORM_PLATFORMSERVICE_H
#define BITCOIN_QT_PLATFORM_PLATFORMSERVICE_H

#include <netaddress.h>
#include <platform/client.h>
#include <platform/types.h>

#include <QObject>
#include <QString>

#include <functional>
#include <memory>
#include <optional>
#include <vector>

class ClientModel;
class WalletModel;

namespace interfaces {
class Node;
} // namespace interfaces

//! The node's settings DashPay chooses its route from: the networks its
//! outbound connections may use and the proxies it uses for them.
struct PlatformNetworkSettings {
    bool ipv4{true};
    bool ipv6{true};
    bool onion{false};
    std::optional<platform::ProxyConfig> clearnet_proxy; //!< -proxy (IPv4 and IPv6)
    std::optional<platform::ProxyConfig> onion_proxy;    //!< -onion, -proxy or the Tor controller's

    static PlatformNetworkSettings Read(interfaces::Node& node);
};

//! How DashPay connects to evonodes, as Core connects to its peers: through
//! one proxy or directly, to the networks that proxy reaches.
struct PlatformRoute {
    std::optional<platform::ProxyConfig> proxy; //!< nullopt: connect directly
    bool ipv4{true};
    bool ipv6{true};
    bool onion{false};

    //! Whether an evonode at this address is reached on this route. I2P
    //! (not SOCKS5) and CJDNS evonodes never are.
    bool allows(const CService& endpoint) const;
    //! With IPv4 or IPv6 reachable, -proxy (or none) to those networks, and
    //! to onion evonodes only when the onion proxy is the same one; with
    //! only onion reachable, the onion proxy to onion evonodes. nullopt: no
    //! network DashPay can use under these settings.
    static std::optional<PlatformRoute> Choose(const PlatformNetworkSettings& settings);
};

/**
 * One wallet's connection to Dash Platform. It holds the PlatformClient,
 * which exists only while the wallet has started DashPay and every gate is
 * open; nothing contacts Platform for the wallet before that.
 *
 * The client is built for the valid evonodes in Core's masternode list that
 * the route reaches. When that set or the route changes, new calls pause and
 * the client is rebuilt once no write is in flight. Network activity turned
 * off destroys the client, calls in flight included.
 */
class PlatformService : public QObject
{
    Q_OBJECT

public:
    //! Why DashPay can't run for the wallet now, in the order checked.
    enum class Gate {
        NONE,
        WALLET,             //!< no single HD seed to derive Platform keys from
        OUTBOUND_DISABLED,  //!< -connect or -maxconnections=0
        BROADCAST_DISABLED, //!< -walletbroadcast=0
        NO_ROUTE,           //!< the network settings leave no network to use
        NO_CONSENT,         //!< DashPay was not started for this wallet
        NETWORK_OFF,
        SYNCING,
        NO_EVONODES, //!< no valid evonode on the route
        CLIENT_FAILED,
    };

    //! What the service reads from the node. An empty member uses the node.
    struct Sources {
        //! The SDK client, verifying proofs against the node's quorums.
        std::function<std::unique_ptr<platform::PlatformClient>(const platform::ClientConfig&, platform::Executor)> make_client;
        //! The Platform HTTPS endpoints of the valid evonodes in Core's
        //! masternode list.
        std::function<std::vector<CService>()> evonodes;
        //! PlatformNetworkSettings::Read.
        std::function<PlatformNetworkSettings()> network_settings;
    };

    //! The wallet record the first DashPay click writes.
    static constexpr const char* CONSENT_RECORD{"dashpay/consent"};

    PlatformService(WalletModel& wallet_model, ClientModel& client_model, Sources sources = {});

    WalletModel& walletModel() const { return m_wallet_model; }
    ClientModel& clientModel() const { return m_client_model; }
    Gate gate() const { return m_gate; }
    //! Why DashPay can't run, naming the setting that disables it; empty for
    //! NONE and NO_CONSENT.
    QString reason() const;
    bool consented() const { return m_consent; }
    //! The first DashPay click: record the wallet's consent and start.
    bool start();
    //! Erase the consent and stop contacting Platform for the wallet.
    bool stop();

    //! The client to start a call on; nullptr while a gate is closed or a
    //! rebuild waits for a write. A call whose client is destroyed never
    //! calls back.
    platform::PlatformClient* client() const { return m_paused ? nullptr : m_client.get(); }
    //! Whether a write is in flight. The wallet's flows start one only when
    //! none is, which serializes the wallet's unlocks and its identity's
    //! state transitions.
    bool writing() const { return m_writes > 0; }

    //! Wrap the callback of a write, in the call that starts it: a rebuild
    //! waits until it has run.
    template <typename T>
    platform::PlatformClient::Callback<T> trackWrite(platform::PlatformClient::Callback<T> cb)
    {
        ++m_writes;
        return [this, cb = std::move(cb)](platform::Result<T> result) {
            // First: the callback may destroy the client, which resets the count.
            writeFinished();
            cb(std::move(result));
        };
    }

Q_SIGNALS:
    //! gate(), consented() or client() changed.
    void changed();

private:
    void update();
    //! Re-read the gates and the evonode set; create, rebuild, pause or
    //! destroy the client to match. Returns whether gate() or client() changed.
    bool refresh();
    Gate evaluate(platform::ClientConfig& config) const;
    void writeFinished();

    WalletModel& m_wallet_model;
    ClientModel& m_client_model;
    Sources m_sources;
    bool m_eligible{false};
    bool m_consent{false};
    Gate m_gate{Gate::NO_CONSENT};
    std::unique_ptr<platform::PlatformClient> m_client;
    platform::ClientConfig m_config;
    //! A configuration the SDK refused, not tried again.
    std::optional<platform::ClientConfig> m_refused;
    int m_writes{0};
    //! The evonode set changed while a write was in flight.
    bool m_paused{false};
};

#endif // BITCOIN_QT_PLATFORM_PLATFORMSERVICE_H
