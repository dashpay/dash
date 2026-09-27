// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_QT_PLATFORM_PLATFORMSERVICE_H
#define BITCOIN_QT_PLATFORM_PLATFORMSERVICE_H

#include <consensus/amount.h>
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
class IdentityFlow;

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
        NO_PLATFORM,
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
 * only object GUI pages talk to. It owns the PlatformClient and the
 * identity/username registration flow, marshals
 * client callbacks onto the GUI thread, persists state through the wallet's
 * platform data records, mints the SigningOperations every write signs
 * under, and feeds node-local context (evonode endpoints, quorum keys, the
 * ChainLock height) into the client.
 */
class PlatformService : public QObject
{
    Q_OBJECT

public:
    //! The chain id a service on the current network verifies against: the
    //! CChainParams value, overridden by -platformchainid off mainnet.
    static std::string EffectiveChainId();
    //! Whether DashPay can run for the wallet and why not otherwise: opt-in
    //! record, descriptor wallet, a network DashPay can use under the node's
    //! settings and, for a running `service`, an evonode on its route,
    //! network active, initial block download finished, a local ChainLock.
    static PlatformAvailability Availability(WalletModel& wallet_model, ClientModel& client_model,
                                             const PlatformService* service = nullptr);
    //! Whether the wallet has opted in (record "platform/enabled").
    static bool IsEnabled(interfaces::Wallet& wallet);
    //! Record the opt-in. The chain id the records belong to is stamped by
    //! the first verified Platform response, never taken from configuration.
    static bool Enable(interfaces::Wallet& wallet);
    //! Remove every Platform record of the wallet but `keep` (opt-out,
    //! network change and record-version mismatch); recovery rebuilds them
    //! from chain.
    static bool WipeRecords(interfaces::Wallet& wallet, const std::string& keep = {});
    //! Whether the wallet's registration record holds an asset lock no
    //! identity consumed yet (IdentityFlow::HoldsUnconsumedFunding), read
    //! from the wallet so it also holds while no service runs.
    static bool HoldsUnconsumedFunding(interfaces::Wallet& wallet);
    //! Wipe the records for recovery to rebuild, keeping the opt-in and a
    //! registration record that holds unconsumed funding: seed recovery
    //! finds identities, not asset locks.
    static bool DiscardRecords(interfaces::Wallet& wallet);

    //! `route` is the one the client was configured for; only evonodes on
    //! it are pushed to the client.
    PlatformService(WalletModel& wallet_model, ClientModel& client_model, std::unique_ptr<platform::PlatformClient> client,
                    PlatformRoute route = {}, QObject* parent = nullptr);
    ~PlatformService() override;

    WalletModel& walletModel() { return m_wallet_model; }
    ClientModel& clientModel() { return m_client_model; }
    platform::PlatformClient& client() { return *m_client; }

    IdentityFlow& identityFlow() { return *m_identity_flow; }

    //! True when the wallet's records were written for another chain: the
    //! page offers to discard local Platform state and re-scan.
    bool networkChanged() const { return m_network_changed; }
    //! Discard local Platform state after a network change and start over.
    void discardStateAfterNetworkChange();
    //! The client holds evonode endpoints to send requests to. None while
    //! network activity is off or the node is syncing.
    bool haveEndpoints() const { return m_have_endpoints; }
    //! The masternode list has evonodes, but none on this service's route
    //! (for example only IPv4 ones under -onlynet=onion).
    bool unreachable() const { return m_unreachable; }
    //! Collect the evonode endpoints, ChainLock height and quorum keys again
    //! now (after a pause) instead of at the next refresh; emits
    //! endpointsAvailable() once endpoints reach the client again.
    void refreshNodeContext() { updateNodeContext(); }
    //! Test-only: push `endpoints` to the client now, as a collection that
    //! just landed would (a test chain has no evonodes to collect); the next
    //! collection replaces them.
    void setEndpointsForTesting(std::vector<platform::Endpoint> endpoints);
    //! Whether network activity is on; Platform lookups wait while it is off.
    bool networkActive() const;
    //! True once a verified response reported a protocol version this build
    //! does not know; writes are refused until Dash Core is updated.
    bool writesFrozen() const { return m_unsupported_version; }
    //! The highest protocol version a verified read has shown Platform to
    //! run this session, which the client builds under; 0 before any.
    uint32_t protocolVersion() const { return m_protocol_version; }
    //! Why Platform cannot be used while its responses are signed for
    //! another chain than this node verifies; empty otherwise.
    QString networkMismatch() const;
    //! The chain this node verifies and what the last mismatch reported.
    QString networkMismatchDetails() const;

    //! Outcome of minting a signing operation: the operation, or why not.
    struct SigningAttempt {
        std::optional<platform::SigningOperation> op;
        //! The user declined to unlock the wallet (the caller parks until
        //! the next user action); otherwise `refusal` says why writes are
        //! refused right now (protocol version ahead, network changed).
        bool unlock_declined{false};
        QString refusal;
    };
    //! Mint the operation a write signs under. Asks the user to unlock an
    //! encrypted wallet.
    SigningAttempt beginSigningOperation(platform::OperationKind kind, std::vector<uint32_t> key_ids,
                                         std::optional<platform::IdentityPublicKey> document_key,
                                         std::optional<wallet::RegistrationFundingKey> funding_key);

    //! Whether a write may start now; error names the reason otherwise
    //! (protocol version ahead of this build, network changed).
    bool writesAllowed(QString& error) const;

    //! The key of a proved identity this wallet signs documents with: the
    //! enabled ECDSA AUTHENTICATION key at the record's auth key id, checked
    //! against the key the wallet derives there. nullopt when the identity
    //! carries no such key.
    std::optional<platform::IdentityPublicKey> documentSigningKey(const platform::Identity& identity) const;

    //! The registered username of this wallet's identity, if any.
    QString myUsername() const;
    std::optional<platform::Identifier> myIdentityId() const;

    //! Duffs an identity registration locks: the base amount plus, for a
    //! contested name, the vote reserve the network's protocol version
    //! requires. nullopt before a verified read has shown that version.
    std::optional<CAmount> identityFundingAmount(bool contested) const;
    //! Credits a contested name locks for the masternode vote on the
    //! network's protocol version; nullopt before a verified read.
    std::optional<uint64_t> contestedNameCredits() const;
    //! Upper bound on the fee of one DPNS document transition (0.001 DASH).
    //! Observed fees are well under half of it; the Platform wallet reserves
    //! the same amount per document transition.
    static constexpr uint64_t DOCUMENT_FEE_RESERVE_CREDITS{100'000'000};
    //! Credits an existing identity needs for a contested name: the vote
    //! reserve plus the fees of the preorder and the domain transition, the
    //! first of which is paid before the second locks the reserve. nullopt
    //! before a verified read.
    std::optional<uint64_t> contestedNameRequiredCredits() const;
    //! DashPay profile field limits (the contract's maxLength).
    static constexpr int MAX_DISPLAY_NAME_LENGTH{25};
    static constexpr int MAX_PUBLIC_MESSAGE_LENGTH{140};

    //! Proof-verified credit balance of this wallet's identity; emits
    //! identityBalanceLoaded().
    void refreshIdentityBalance();

    //! Async name availability probe (proof-backed absence check). Emits
    //! nameAvailability().
    void checkNameAvailability(const QString& name);
    //! Async proof-verified contested-name vote state. Emits contestedNameState().
    void checkContestedNameState(const QString& normalized_label);

    //! Wallet platform-data record helpers (used by the flows).
    bool writeRecord(const std::string& key, const std::vector<unsigned char>& value);
    std::vector<unsigned char> readRecord(const std::string& key) const;

    //! Run a callback on the GUI thread (safe from client threads; dropped
    //! if the service is destroyed first).
    void post(std::function<void()> fn);

    //! Every client result passes through here (the client the flows see is
    //! wrapped to do so) so ChainIdMismatch and UnsupportedProtocolVersion
    //! drive their user-visible states. The first verified read stamps the
    //! records with the chain id it was verified for; protocol_version is
    //! the one its metadata carries (0 for a broadcast).
    void observeStatus(const platform::Status& status, bool verified_read, uint32_t protocol_version);

Q_SIGNALS:
    //! The wallet's records belong to another Platform chain.
    void platformNetworkChanged();
    //! A verified response reported a protocol version this build does not
    //! know: writes are frozen until Dash Core is updated.
    void unsupportedProtocolVersion();
    //! networkMismatch() changed.
    void networkMismatchChanged();
    //! Endpoints reached the client after it had none (network activity
    //! turned on, sync finished): reads made meanwhile can be made again.
    void endpointsAvailable();
    //! Network activity was turned on or off.
    void networkActiveChanged(bool active);
    //! unreachable() changed.
    void reachabilityChanged();
    void nameAvailability(const QString& normalized_label, bool available, bool contested);
    void nameAvailabilityFailed(const QString& normalized_label, const QString& error, const QString& details);
    //! The name is registered to this wallet's own identity.
    void nameIsOurs(const QString& normalized_label);
    //! Proof-verified contested vote state for a label; error is empty on
    //! success.
    void contestedNameState(const QString& normalized_label, const platform::ContestedNameState& state,
                            const QString& error);
    void identityStateChanged();
    void identityBalanceLoaded(quint64 credits);
    void identityBalanceFailed(const QString& error, const QString& details);
    void flowFailed(const QString& step, const QString& error, const QString& details);

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

    WalletModel& m_wallet_model;
    ClientModel& m_client_model;
    std::unique_ptr<platform::PlatformClient> m_client;
    const PlatformRoute m_route;
    const std::string m_chain_id;
    bool m_network_changed{false};
    //! The records carry the chain id of a verified response.
    bool m_chain_id_stamped{false};
    //! The last verified read failed for another chain id; cleared by the
    //! next verified read.
    bool m_network_mismatch{false};
    //! What the last chain-id mismatch reported, for its details.
    std::string m_network_mismatch_message;
    bool m_unsupported_version{false};
    uint32_t m_protocol_version{0};
    bool m_have_endpoints{false};
    bool m_unreachable{false};
    std::unique_ptr<interfaces::Handler> m_chainlock_handler;

    std::unique_ptr<IdentityFlow> m_identity_flow;
    QTimer* m_tick_timer{nullptr};    //!< drives flow advance/retry
    QTimer* m_context_timer{nullptr}; //!< refreshes endpoints/quorum keys
    QThread* m_context_thread{nullptr};
    QObject* m_context_worker{nullptr}; //!< lives on m_context_thread
    bool m_context_refresh_pending{false};
    //! A context change came while one was collected: collect again.
    bool m_context_refresh_again{false};
};

#endif // BITCOIN_QT_PLATFORM_PLATFORMSERVICE_H
