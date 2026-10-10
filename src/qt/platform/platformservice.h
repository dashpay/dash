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
#include <wallet/platformtypes.h>

#include <QObject>
#include <QPair>
#include <QSet>
#include <QString>
#include <QThread>
#include <QTimer>
#include <QVector>

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

class ClientModel;
class ContactFlow;
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
 * only object GUI pages talk to. It owns the PlatformClient and the flows
 * (identity/username registration, contacts), marshals
 * client callbacks onto the GUI thread, persists state through the wallet's
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
    //! Remove every Platform record of the wallet but `keep` (opt-out and
    //! record-version mismatch); recovery rebuilds them from chain.
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

    //! Stop for good while the wallet and client models still exist (the
    //! page is detached at shutdown): no timer, ChainLock, network or wallet
    //! notification and no client callback reaches them afterwards, the
    //! client is shut down, and a profile update in flight ends as refused.
    //! The object stays usable for whoever still holds it: records read
    //! empty, writes are refused and reads go nowhere. (A passphrase prompt
    //! already open when it stops is the wallet model's own.)
    void stop();
    bool stopped() const { return m_stopped; }

    WalletModel& walletModel() { return m_wallet_model; }
    ClientModel& clientModel() { return m_client_model; }
    platform::PlatformClient& client() { return *m_client; }

    IdentityFlow& identityFlow() { return *m_identity_flow; }
    ContactFlow& contactFlow() { return *m_contact_flow; }

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
    //! Usernames one search returns at most.
    static constexpr uint32_t SEARCH_PAGE_SIZE{25};
    //! Async prefix search (one page); emits searchResults(), then
    //! searchProfileLoaded() for each result with a profile display name.
    void searchNames(const QString& prefix);
    //! Whether an identity can receive a contact request: its proved
    //! identity carries a key one can be encrypted to (the send path's
    //! platform::helpers::Dip15SelectRecipientKey). Known once checkRecipient()
    //! or a send has read it this session; nullopt before.
    std::optional<bool> canReceiveContactRequests(const QString& identity_hex) const;
    //! Read the identity (once a session); emits recipientChecked().
    void checkRecipient(const QString& identity_hex);
    //! Async profile fetch; emits profileLoaded().
    void loadProfile(const platform::Identifier& identity);

    //! Fetch this identity's incoming + outgoing contact requests, one page
    //! per request continued from the cursor; emits contactsUpdated().
    void refreshContacts();
    //! Send a contact request to an identity (hex id); accept an incoming one.
    bool sendContactRequest(const QString& identity_hex, QString& error);
    bool acceptContact(const QString& identity_hex, QString& error);
    //! Whether a contact request has been established (their key imported).
    bool isEstablished(const QString& identity_hex) const;
    //! Whether we sent this identity a contact request: on chain, recorded
    //! by this wallet (or seed recovery), or being confirmed.
    bool sentRequestTo(const QString& identity_hex) const;
    //! The identity our last contact request (or reply) went to while Dash
    //! Platform is confirming it; empty otherwise.
    QString pendingContactRequest() const
    {
        return m_contact_request_pending ? m_contact_request_in_flight : QString{};
    }
    //! Whether the contact has answered our request with theirs, but its
    //! keychain is not imported yet (the wallet was locked, or finishing
    //! failed).
    bool isAccepted(const QString& identity_hex) const;
    //! Why finishing an accepted contact failed the last time, other than
    //! a locked wallet; empty when it did not fail.
    QString acceptedError(const QString& identity_hex) const { return m_accepted_errors.value(identity_hex); }
    //! When our own request to an identity was created, from the last
    //! fetched outgoing requests (seconds); nullopt when we sent none.
    std::optional<int64_t> outgoingRequestTime(const platform::Identifier& identity) const;
    //! Whether the user chose to ignore this identity's request (or to hide
    //! this contact). Local to this wallet: a contact request can be neither
    //! withdrawn nor rejected on Platform, and the sender is never told.
    bool isHidden(const QString& identity_hex) const;
    void setHidden(const QString& identity_hex, bool hidden);


    //! Publish a DashPay profile (create or replace). Emits profileUpdated().
    bool updateProfile(const QString& display_name, const QString& public_message, QString& error);

    //! Wallet platform-data record helpers (used by the flows).
    bool writeRecord(const std::string& key, const std::vector<unsigned char>& value);
    std::vector<unsigned char> readRecord(const std::string& key) const;

    //! Proof-verified contact metadata cached in wallet platform data, or
    //! for identities that are not contacts, in memory from a search.
    QString contactMetadata(const QString& identity_hex, const char* prefix) const;
    //! Human-readable name for a contact, best first: username, profile
    //! display name (marked untrusted), shortened identity id. Plain text:
    //! the display name is counterparty-authored.
    QString contactDisplayString(const QString& identity_hex) const;
    //! Address-book label applied to DIP-15 friendship addresses so
    //! transaction history attributes payments to the contact.
    QString contactAddressLabel(const QString& identity_hex) const;

    //! Run a callback on the GUI thread (safe from client threads; dropped
    //! if the service is stopped or destroyed first).
    void post(std::function<void()> fn);

    //! Every client result passes through here (the client the flows see is
    //! wrapped to do so) so UnsupportedProtocolVersion drives its
    //! user-visible state; protocol_version is the one a verified read's
    //! metadata carries (0 for a broadcast).
    void observeStatus(const platform::Status& status, bool verified_read, uint32_t protocol_version);

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
    void nameAvailability(const QString& normalized_label, bool available, bool contested);
    void nameAvailabilityFailed(const QString& normalized_label, const QString& error, const QString& details);
    //! The name is registered to this wallet's own identity.
    void nameIsOurs(const QString& normalized_label);
    //! Proof-verified contested vote state for a label; error is empty on
    //! success.
    void contestedNameState(const QString& normalized_label, const platform::ContestedNameState& state,
                            const QString& error);
    void searchResults(const QString& prefix, const QVector<QPair<QString, QString>>& results); //!< (label, identity hex)
    //! The proved profile display name of a search result.
    void searchProfileLoaded(const QString& identity_hex, const QString& display_name);
    void searchFailed(const QString& prefix, const QString& error, const QString& details);
    void recipientChecked(const QString& identity_hex, bool can_receive);
    //! Emitted with empty fields (and revision 0) when the identity
    //! verifiably has no profile.
    void profileLoaded(const QString& identity_hex, const QString& display_name, const QString& public_message,
                       quint64 revision);
    void profileLoadFailed(const QString& identity_hex, const QString& error, const QString& details);
    void identityStateChanged();
    void identityBalanceLoaded(quint64 credits);
    void identityBalanceFailed(const QString& error, const QString& details);
    void flowFailed(const QString& step, const QString& error, const QString& details);
    //! (identity hex, username) pairs for incoming and outgoing requests.
    void contactsUpdated(const QVector<QPair<QString, QString>>& incoming,
                         const QVector<QPair<QString, QString>>& outgoing);
    void contactsRefreshFailed(const QString& error, const QString& details);
    //! A contacts refresh read the whole list (after its contactsUpdated()).
    void contactsRefreshed();
    //! A contact was ignored or shown again.
    void hiddenContactsChanged();
    void profileUpdated(bool ok, const QString& error, const QString& details);
    //! Dash Platform accepted the contact request (or the reply to an
    //! accepted one) for broadcast; its confirmation continues in the
    //! background and ends in contactRequestFinished(ok) or, when it takes
    //! too long, in the next contacts refresh.
    void contactRequestPending(const QString& identity_hex);
    //! ok: the request was confirmed on Platform. Otherwise it was not sent
    //! (or refused), and error says why.
    void contactRequestFinished(const QString& identity_hex, bool ok, const QString& error, const QString& details);

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
    //! Emit contactsUpdated() from the last fetched requests.
    void publishContacts();
    //! Import the keychain of every contact that answered our request, when
    //! the wallet can do so without asking for its passphrase.
    void completeAcceptedContacts();
    void finishContactRequest(const QString& identity_hex, bool ok, const QString& error, const QString& details);
    //! Why a contact request or a profile update cannot start now: both sign
    //! with the identity's DashPay nonce, so another request or profile
    //! update in flight or being confirmed, or the profile chosen at
    //! registration, holds it. Empty when one can.
    QString contactRequestBlocker() const;
    void finishProfileUpdate(bool ok, const QString& error, const QString& details);
    void failProfileUpdate(const platform::Status& status, const QString& operation);
    //! Read a contact's username and profile again unless read within the
    //! TTL; force reads them regardless (their request changed).
    void hydrateContactMetadata(const platform::Identifier& identity, bool force);
    //! Proved profile display name of a search result, kept in memory.
    void loadSearchProfile(const platform::Identifier& identity);
    //! Remember whether a proved identity can receive a contact request.
    void recordRecipient(const QString& identity_hex, const platform::Identity& identity);
    void setContactMetadata(const QString& identity_hex, const char* prefix, const QString& value);
    void collectContactRequests(bool to_me, const platform::Identifier& start_after,
                                std::vector<platform::ContactRequest> collected);
    //! The contacts refresh ended; run the one asked for meanwhile.
    void finishContactsRefresh();
    void publishProfile(const platform::Identity& identity, const platform::Profile& existing,
                        const platform::ProfileInput& input);
    void confirmProfile(const platform::Identifier& owner, uint64_t revision, const platform::ProfileInput& input,
                        int attempts_left);

    WalletModel& m_wallet_model;
    ClientModel& m_client_model;
    std::unique_ptr<platform::PlatformClient> m_client;
    const PlatformRoute m_route;
    bool m_stopped{false};
    bool m_unsupported_version{false};
    uint32_t m_protocol_version{0};
    bool m_have_endpoints{false};
    bool m_unreachable{false};
    std::unique_ptr<interfaces::Handler> m_chainlock_handler;

    std::unique_ptr<IdentityFlow> m_identity_flow;
    std::unique_ptr<ContactFlow> m_contact_flow;
    //! The newest request of each sender (ContactFlow::NewestPerSender).
    std::vector<platform::ContactRequest> m_incoming_contacts;
    std::vector<platform::ContactRequest> m_outgoing_contacts;
    //! Document id of each sender's request at the last refresh.
    QHash<QString, platform::Identifier> m_incoming_documents;
    //! Identity hex of every sender of m_incoming_contacts and recipient of
    //! m_outgoing_contacts.
    QSet<QString> m_incoming_ids;
    QSet<QString> m_outgoing_ids;
    bool m_contacts_refreshing{false};
    //! A contacts refresh was asked for while one ran or while there were
    //! no endpoints: it runs when the current one ends or they arrive.
    bool m_contacts_refresh_again{false};
    QSet<QString> m_contact_metadata_pending;
    //! Contact metadata re-read no more than once per TTL, by identity hex.
    QHash<QString, int64_t> m_contact_metadata_fetched_at;
    //! Usernames and profile display names of search results (identity hex
    //! -> text), never persisted.
    QHash<QString, QString> m_searched_usernames;
    QHash<QString, QString> m_searched_display_names;
    //! Whether a search result can receive a contact request, once read.
    QHash<QString, bool> m_recipients_checked;
    QSet<QString> m_recipients_checking;
    QString m_contact_request_in_flight;
    //! The request in flight was accepted for broadcast and is being confirmed.
    bool m_contact_request_pending{false};
    //! Accepted contacts whose keychain import is running.
    QSet<QString> m_completing_contacts;
    //! Why an accepted contact could not be finished, by identity hex.
    QHash<QString, QString> m_accepted_errors;
    //! Accepted contacts whose request was refused: finishing them again
    //! waits for the user, not the next refresh or unlock.
    QSet<QString> m_accepted_refused;
    bool m_profile_update_in_flight{false};
    //! The contacts refresh under way stopped at MAX_CONTACT_REQUESTS with
    //! more requests left unread.
    bool m_contacts_partial{false};
    QTimer* m_tick_timer{nullptr};    //!< drives flow advance/retry
    QTimer* m_context_timer{nullptr}; //!< refreshes endpoints/quorum keys
    QThread* m_context_thread{nullptr};
    QObject* m_context_worker{nullptr}; //!< lives on m_context_thread
    bool m_context_refresh_pending{false};
    //! A context change came while one was collected: collect again.
    bool m_context_refresh_again{false};
};

#endif // BITCOIN_QT_PLATFORM_PLATFORMSERVICE_H
