// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_QT_PLATFORM_CONTACTFLOW_H
#define BITCOIN_QT_PLATFORM_CONTACTFLOW_H

#include <platform/types.h>
#include <wallet/platformtypes.h>

#include <QObject>
#include <QString>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

class PlatformService;

/**
 * Sends a DashPay contact request and imports the resulting friendship
 * keychains so payments to/from the contact are tracked by the wallet.
 *
 * A contact request carries our DIP-15 receiving xpub in its 69-byte
 * compact form, encrypted by the SDK with the ECDH secret the wallet
 * derives between our ENCRYPTION key and the recipient's DECRYPTION (or
 * ENCRYPTION) key, plus the DIP-15 accountReference masked with the MAC the
 * wallet computes over that xpub. Our receiving chain is imported before
 * the request is sent, so the recipient can pay as soon as they accept.
 * Accepting an incoming request decrypts and stores their xpub for outbound
 * payments, then sends our request back unless ours is already on chain.
 * A request that answers ours establishes the contact without any
 * broadcast, as the mobile wallets do.
 *
 * The key work of an operation (decryption, keychain derivation, MAC, ECDH,
 * signature) runs under one wallet unlock, released before every network
 * wait; the identity keys used are those the proved identity carries at
 * the ids the identity record names.
 *
 * Per-contact state is persisted under the "contact/…" platform data
 * records; the request documents themselves live on Platform.
 */
class ContactFlow : public QObject
{
    Q_OBJECT

public:
    explicit ContactFlow(PlatformService& service, QObject* parent = nullptr);

    //! The one request per sender DashPay acts on, in first-seen order: the
    //! latest, then the highest accountReference (whose top four bits are
    //! the DIP-15 version), as the mobile wallets choose it. A sender sends
    //! again to replace its payment addresses, and Platform keeps every
    //! request.
    static std::vector<platform::ContactRequest> NewestPerSender(const std::vector<platform::ContactRequest>& requests);

    //! Send a contact request to a proved identity. The recipient key is
    //! chosen by the SDK's mint-side policy. Emits requestSent/requestFailed.
    void sendRequest(const platform::Identity& recipient);

    //! Accept an incoming request: verify the sender identity, decrypt the
    //! sender's xpub, import the keychains and send a request back unless
    //! ours is already on chain.
    void accept(const platform::ContactRequest& incoming);

    //! Establish a contact whose request answers ours, without asking for
    //! the passphrase (the wallet must be unlocked already) and without any
    //! broadcast. Emits acceptedCompleted.
    void completeAccepted(const platform::ContactRequest& incoming);

    //! Decrypt the compact xpub of an incoming request with our key at its
    //! recipient_key_index, which must be our ENCRYPTION or DECRYPTION key,
    //! after checking both key purposes against the DIP-15 receive policy.
    //! sender is the proved sender identity.
    std::optional<wallet::CompactXpub> decryptXpub(const platform::ContactRequest& incoming,
                                                   const platform::Identity& sender, QString& error);

    //! Import the contact's sending chain as our receiving keychain and
    //! validate their xpub. birth_time (unix seconds) bounds later rescans;
    //! it is the time of OUR own request, never the counterparty's document
    //! time, which the sender controls.
    bool importKeychains(const platform::Identifier& their_identity, const wallet::CompactXpub& their_xpub,
                         int64_t birth_time, QString& error);

Q_SIGNALS:
    //! Platform accepted the request for broadcast; it is being confirmed.
    void requestPending(const QString& to_identity_hex);
    //! The request is confirmed on Platform (or the contact established
    //! without one).
    void requestSent(const QString& to_identity_hex);
    //! Platform did not confirm the request it accepted within the
    //! confirmation window; the contacts refresh shows it once it does.
    void requestUnconfirmed(const QString& to_identity_hex);
    //! The request was not sent, or Platform refused it.
    void requestFailed(const QString& to_identity_hex, const QString& error, const QString& details);
    //! error is empty once the contact is established; otherwise
    //! `retryable` says whether a later attempt can succeed on its own (an
    //! unanswered read, a wallet locked in the meantime) or the request
    //! itself was refused.
    void acceptedCompleted(const QString& identity_hex, const QString& error, bool retryable);

private:
    //! Send our request; when accepting, their request is established under
    //! the same unlock.
    void sendRequest(const platform::Identity& recipient, std::optional<platform::ContactRequest> accepting);
    //! When our own request to an identity was created, if we sent one.
    std::optional<int64_t> ourRequestTime(const platform::Identifier& to_identity) const;
    //! Decrypt an incoming request, import the keychains and store their xpub.
    bool establish(const platform::ContactRequest& incoming, const platform::Identity& sender, int64_t birth_time,
                   QString& error);
    //! establish() for a request that answers ours, sent at our_request_time.
    bool establishAnswered(const platform::ContactRequest& incoming, const platform::Identity& sender,
                           int64_t our_request_time, QString& error);
    bool prepareReceivingKeychain(const platform::Identifier& their_identity, wallet::FriendshipXpub& xpub,
                                  int64_t birth_time, QString& error);
    //! The account references of our requests to a contact on chain, page
    //! by page: the next request's rotation version is one above the latest
    //! of them (unmasked with our MAC), or 0 for a first request.
    void collectOurRequests(const platform::Identity& recipient, std::optional<platform::ContactRequest> accepting,
                            const platform::Identifier& start_after, std::vector<uint32_t> account_references,
                            int pages_left);
    //! Reads our proved identity and DashPay contract nonce, then signs.
    void buildAndBroadcast(const platform::Identity& recipient, std::optional<platform::ContactRequest> accepting,
                           std::vector<uint32_t> account_references);
    void signAndBroadcast(const platform::Identity& sender, const platform::Identity& recipient,
                          const std::optional<platform::ContactRequest>& accepting,
                          const std::vector<uint32_t>& account_references, uint64_t nonce);
    //! Confirms the request just broadcast by its account reference among
    //! our requests created since since_ms, page by page, repeating the
    //! query with a growing interval until deadline (unix seconds).
    void confirmRequest(const platform::Identifier& to_identity, uint32_t account_reference, uint64_t since_ms,
                        const platform::Identifier& start_after, int pages_left, int64_t deadline, int interval_ms);
    void fail(const platform::Identifier& to_identity, const QString& error, const QString& details = {});
    //! fail() with the plain-language text for a failed client call.
    void fail(const platform::Identifier& to_identity, const platform::Status& status, const QString& operation);

    PlatformService& m_service;
};

#endif // BITCOIN_QT_PLATFORM_CONTACTFLOW_H
