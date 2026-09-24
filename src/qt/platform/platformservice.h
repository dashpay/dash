// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_QT_PLATFORM_PLATFORMSERVICE_H
#define BITCOIN_QT_PLATFORM_PLATFORMSERVICE_H

#include <netaddress.h>
#include <platform/client.h>
#include <platform/signer.h>
#include <platform/types.h>
#include <qt/walletmodel.h>

#include <QObject>
#include <QString>
#include <QThread>
#include <QTimer>

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

class ClientModel;

namespace interfaces {
class Handler;
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
    //! Test-only: what Read() returns instead of the node's settings, which
    //! a test cannot restore (a proxy once set cannot be unset); nullopt
    //! reads the node again.
    static void SetForTesting(std::optional<PlatformNetworkSettings> settings);
};

//! How DashPay connects to evonodes, as Core connects to its peers: through
//! one proxy or directly, to the networks that proxy reaches. The default
//! route is Core's default: IPv4 and IPv6, no proxy.
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
    //! only onion reachable, the onion proxy to onion evonodes. nullopt:
    //! no network DashPay can use under these settings.
    static std::optional<PlatformRoute> Choose(const PlatformNetworkSettings& settings);
};

//! Why DashPay cannot run for a wallet right now; Gate::NONE when it can.
struct PlatformAvailability {
    enum class Gate {
        NONE,
        LEGACY_WALLET,
        NO_PRIVATE_KEYS,
        NETWORK_SETTINGS,
        //! The service runs, but no evonode is on a network its route reaches.
        UNREACHABLE,
        NETWORK_INACTIVE,
        SYNCING,
        NO_CHAINLOCK,
    };
    bool enabled{false}; //!< the wallet opted in
    Gate gate{Gate::NONE};
    QString title;  //!< short heading of the reason
    QString reason; //!< the reason in a sentence
    bool available() const { return enabled && gate == Gate::NONE; }
};

/**
 * Per-wallet orchestrator for all Dash Platform interactions. This is the
 * only object GUI pages talk to. It owns the PlatformClient, marshals its
 * callbacks onto the GUI thread, persists state through the wallet's
 * platform data records, mints the SigningOperations every write signs
 * under, and feeds node-local context (evonode endpoints, quorum keys, the
 * ChainLock height) into the client.
 */
class PlatformService : public QObject
{
    Q_OBJECT

public:
    //! Whether DashPay can run for the wallet and why not otherwise: opt-in
    //! record, descriptor wallet, a network DashPay can use under the node's
    //! settings and, for a running `service`, an evonode on its route,
    //! network active, initial block download finished, a local ChainLock.
    static PlatformAvailability Availability(WalletModel& wallet_model, ClientModel& client_model,
                                             const PlatformService* service = nullptr);
    //! Whether the wallet has opted in (record "platform/enabled").
    static bool IsEnabled(interfaces::Wallet& wallet);
    //! Record the opt-in.
    static bool Enable(interfaces::Wallet& wallet);
    //! Remove every Platform record of the wallet (opt-out and
    //! record-version mismatch); recovery rebuilds them from chain.
    static bool WipeRecords(interfaces::Wallet& wallet);

    //! `route` is the one the client was configured for; only evonodes on
    //! it are pushed to the client.
    PlatformService(WalletModel& wallet_model, ClientModel& client_model, std::unique_ptr<platform::PlatformClient> client,
                    PlatformRoute route = {}, QObject* parent = nullptr);
    ~PlatformService() override;

    WalletModel& walletModel() { return m_wallet_model; }
    ClientModel& clientModel() { return m_client_model; }
    platform::PlatformClient& client() { return *m_client; }

    //! The client holds evonode endpoints to send requests to. None while
    //! network activity is off or the node is syncing.
    bool haveEndpoints() const { return m_have_endpoints; }
    //! The masternode list has evonodes, but none on this service's route
    //! (for example only IPv4 ones under -onlynet=onion).
    bool unreachable() const { return m_unreachable; }
    //! Test-only: push `endpoints` to the client now, as a collection that
    //! just landed would (a test chain has no evonodes to collect); the next
    //! collection replaces them.
    void setEndpointsForTesting(std::vector<platform::Endpoint> endpoints);
    //! Whether network activity is on; Platform lookups wait while it is off.
    bool networkActive() const;

    //! Mint the operation a write signs under. Asks the user to unlock an
    //! encrypted wallet; nullopt when they decline, so the caller parks.
    std::optional<platform::SigningOperation> beginSigningOperation(
        platform::OperationKind kind, std::vector<uint32_t> key_ids,
        std::optional<platform::IdentityPublicKey> document_key,
        std::optional<wallet::RegistrationFundingKey> funding_key);

    //! Wallet platform-data record helpers.
    bool writeRecord(const std::string& key, const std::vector<unsigned char>& value);
    std::vector<unsigned char> readRecord(const std::string& key) const;

    //! Run a callback on the GUI thread (safe from client threads; dropped
    //! if the service is destroyed first).
    void post(std::function<void()> fn);

Q_SIGNALS:
    //! A verified response reported a protocol version this build does not
    //! know: writes are frozen until Dash Core is updated.
    void unsupportedProtocolVersion();
    //! Endpoints reached the client after it had none (network activity
    //! turned on, sync finished): reads made meanwhile can be made again.
    void endpointsAvailable();
    //! Network activity was turned on or off.
    void networkActiveChanged(bool active);
    //! unreachable() changed.
    void reachabilityChanged();

private Q_SLOTS:
    //! Collect the evonode endpoints, ChainLock height and Platform quorum
    //! keys on the worker thread (the masternode list walk takes cs_main)
    //! and push them into the client from the GUI thread.
    void updateNodeContext();

private:
    struct NodeContext {
        std::vector<platform::Endpoint> endpoints;
        int32_t chainlock_height{0};
        uint8_t llmq_type{0};
        std::vector<platform::QuorumKey> quorum_keys;
    };
    void applyNodeContext(NodeContext context);
    //! Hand the endpoints on the route to the client and tell who waits for
    //! them.
    void pushEndpoints(std::vector<platform::Endpoint> endpoints);

    //! Every client callback passes through here so UnsupportedProtocolVersion
    //! drives its user-visible state.
    void observeStatus(const platform::Status& status);

    WalletModel& m_wallet_model;
    ClientModel& m_client_model;
    std::unique_ptr<platform::PlatformClient> m_client;
    const PlatformRoute m_route;
    bool m_unsupported_version{false};
    bool m_have_endpoints{false};
    bool m_unreachable{false};
    std::unique_ptr<interfaces::Handler> m_chainlock_handler;
    QTimer* m_context_timer{nullptr}; //!< refreshes endpoints/quorum keys
    QThread* m_context_thread{nullptr};
    QObject* m_context_worker{nullptr}; //!< lives on m_context_thread
    bool m_context_refresh_pending{false};
    //! A context change came while one was collected: collect again.
    bool m_context_refresh_again{false};
};

#endif // BITCOIN_QT_PLATFORM_PLATFORMSERVICE_H
