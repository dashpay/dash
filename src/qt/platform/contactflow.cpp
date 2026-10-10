// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/platform/contactflow.h>

#include <interfaces/wallet.h>
#include <logging.h>
#include <platform/client.h>
#include <platform/helpers.h>
#include <platform/signer.h>
#include <platform/walletrecords.h>
#include <qt/platform/identityflow.h>
#include <qt/platform/platformservice.h>
#include <qt/platform/platformui.h>
#include <qt/walletmodel.h>
#include <support/cleanse.h>
#include <util/strencodings.h>
#include <util/time.h>
#include <wallet/platformtypes.h>

#include <QPointer>
#include <QTimer>

#include <algorithm>
#include <map>
#include <utility>

using interfaces::Wallet;
using platform::StatusKind;

namespace {
//! DIP-15 account of every friendship keychain in this version.
constexpr uint32_t ACCOUNT{0};
//! Addresses of a new receiving chain labelled for transaction history.
constexpr uint32_t LABELLED_ADDRESSES{20};
//! How long a request Platform accepted for broadcast is confirmed by proved
//! re-reads (Platform took up to 68 s on testnet), with a backoff from the
//! first interval to the last; after that the contacts refresh reconciles it.
constexpr int64_t CONFIRM_WINDOW_SECONDS{180};
constexpr int FIRST_CONFIRM_INTERVAL_MS{2'500};
constexpr int MAX_CONFIRM_INTERVAL_MS{20'000};
//! Pages of our own sent requests examined for the rotation version, and
//! for the confirmation of a request just broadcast.
constexpr int MAX_REQUEST_PAGES{10};
//! Slack, in seconds, between this node's clock and the block time Platform
//! stamps a document with, for the confirmation query's lower bound.
constexpr int64_t CONFIRM_CLOCK_SLACK{600};

QString Hex(const platform::Identifier& id) { return QString::fromStdString(HexStr(id)); }

std::string ContactKey(const char* prefix, const platform::Identifier& id) { return prefix + HexStr(id); }

//! The identity went while a request was prepared (the DashPay data was
//! rebuilt meanwhile).
QString IdentityGoneText()
{
    return ContactFlow::tr("Your DashPay data changed while the request was prepared, so nothing was sent. Try again "
                           "once your username shows on the DashPay tab.");
}

QString UnlockDeclinedText()
{
    return ContactFlow::tr("The wallet stayed locked, so nothing was sent. Try again and enter your passphrase.");
}

//! A key-work failure the user cannot act on: plain text, the reason in the details.
QString KeyWorkText()
{
    return ContactFlow::tr(
        "Dash Core could not prepare the contact request keys. Please report this; Show details has more.");
}

const platform::IdentityPublicKey* FindKey(const platform::Identity& identity, uint32_t id)
{
    for (const auto& key : identity.public_keys) {
        if (key.id == id) return &key;
    }
    return nullptr;
}
} // namespace

ContactFlow::ContactFlow(PlatformService& service, QObject* parent) :
    QObject(parent),
    m_service(service)
{
}

std::vector<platform::ContactRequest> ContactFlow::NewestPerSender(const std::vector<platform::ContactRequest>& requests)
{
    // The mobile wallets' order, so both ends pay the same chain: the latest,
    // then the higher reference (its top four bits are the version).
    const auto rank = [](const platform::ContactRequest& request) {
        return std::pair{request.created_at, request.account_reference};
    };
    std::vector<platform::ContactRequest> newest;
    std::map<platform::Identifier, size_t> kept;
    for (const auto& request : requests) {
        const auto [it, added]{kept.try_emplace(request.owner_id, newest.size())};
        if (added) {
            newest.push_back(request);
        } else if (rank(request) > rank(newest[it->second])) {
            newest[it->second] = request;
        }
    }
    return newest;
}

void ContactFlow::fail(const platform::Identifier& to_identity, const QString& error, const QString& details)
{
    Q_EMIT requestFailed(Hex(to_identity), error, details);
}

void ContactFlow::fail(const platform::Identifier& to_identity, const platform::Status& status, const QString& operation)
{
    const auto error{PlatformUi::Describe(status, PlatformUi::Context::CONTACT_REQUEST, operation,
                                          m_service.contactDisplayString(Hex(to_identity)))};
    fail(to_identity, error.text, error.details);
}

void ContactFlow::sendRequest(const platform::Identity& recipient) { sendRequest(recipient, std::nullopt); }

void ContactFlow::sendRequest(const platform::Identity& recipient, std::optional<platform::ContactRequest> accepting)
{
    if (!m_service.myIdentityId()) {
        fail(recipient.id, tr("Register a username before sending contact requests."));
        return;
    }
    const auto recipient_key_id{platform::helpers::Dip15SelectRecipientKey(recipient)};
    if (!recipient_key_id || !FindKey(recipient, *recipient_key_id)) {
        fail(recipient.id, tr("This DashPay user's identity has no key a contact request can be sent to."));
        return;
    }
    collectOurRequests(recipient, std::move(accepting), platform::Identifier{}, {}, MAX_REQUEST_PAGES);
}

void ContactFlow::collectOurRequests(const platform::Identity& recipient, std::optional<platform::ContactRequest> accepting,
                                     const platform::Identifier& start_after, std::vector<uint32_t> account_references,
                                     int pages_left)
{
    // The unique index (ownerId, toUserId, accountReference) rejects a
    // resend at a version already on chain, and a local counter is lost on
    // seed recovery, so the chain is the source of truth: the references of
    // our earlier requests to this contact are unmasked once the MAC is
    // known, under the signing step's unlock.
    const auto my_id{m_service.myIdentityId()};
    if (!my_id) {
        fail(recipient.id, IdentityGoneText());
        return;
    }
    QPointer<ContactFlow> self{this};
    m_service.client().getContactRequests(
        *my_id, /*to_me=*/false, /*since_ms=*/0, start_after,
        [self, recipient, accepting, account_references = std::move(account_references),
         pages_left](platform::Result<platform::Paged<platform::ContactRequest>> res) mutable {
            if (!self) return;
            self->m_service.post([self, recipient, accepting = std::move(accepting),
                                  account_references = std::move(account_references), pages_left,
                                  res = std::move(res)]() mutable {
                if (!self) return;
                if (!res.ok() && !res.provenAbsent()) {
                    self->fail(recipient.id, res.status, tr("Read sent requests"));
                    return;
                }
                if (res.ok()) {
                    for (const auto& request : res.value->items) {
                        if (request.to_user_id == recipient.id) account_references.push_back(request.account_reference);
                    }
                    if (res.value->has_more) {
                        if (pages_left > 1) {
                            self->collectOurRequests(recipient, std::move(accepting), res.value->next_start_after,
                                                     std::move(account_references), pages_left - 1);
                            return;
                        }
                        // An unread request may hold the latest version: a
                        // guess could reuse one already on chain.
                        self->fail(recipient.id, tr("You have sent too many contact requests for Dash Core to find "
                                                    "the next one to this person."));
                        return;
                    }
                }
                self->buildAndBroadcast(recipient, std::move(accepting), std::move(account_references));
            });
        });
}

void ContactFlow::buildAndBroadcast(const platform::Identity& recipient, std::optional<platform::ContactRequest> accepting,
                                    std::vector<uint32_t> account_references)
{
    const auto my_id{m_service.myIdentityId()};
    if (!my_id) {
        fail(recipient.id, IdentityGoneText());
        return;
    }
    QPointer<ContactFlow> self{this};
    const auto dashpay{platform::helpers::SystemContractId(platform::helpers::SystemContract::DASHPAY)};
    m_service.client().getIdentity(*my_id, [self, recipient, accepting, account_references,
                                            dashpay](platform::Result<platform::Identity> sender_res) {
        if (!self) return;
        self->m_service.post([self, recipient, accepting, account_references, dashpay, sender_res = std::move(sender_res)] {
            if (!self) return;
            if (!sender_res.ok()) {
                self->fail(recipient.id, sender_res.status, tr("Read own identity"));
                return;
            }
            const platform::Identity sender{*sender_res.value};
            self->m_service.client().getIdentityContractNonce(
                sender.id, dashpay,
                [self, recipient, accepting, account_references, sender](platform::Result<uint64_t> nonce_res) {
                    if (!self) return;
                    self->m_service.post(
                        [self, recipient, accepting, account_references, sender, nonce_res = std::move(nonce_res)] {
                            if (!self) return;
                            if (!nonce_res.ok() && !nonce_res.provenAbsent()) {
                                self->fail(recipient.id, nonce_res.status, tr("Read identity nonce"));
                                return;
                            }
                            self->signAndBroadcast(sender, recipient, accepting, account_references,
                                                   nonce_res.ok() ? *nonce_res.value + 1 : 1);
                        });
                });
        });
    });
}

void ContactFlow::signAndBroadcast(const platform::Identity& sender, const platform::Identity& recipient,
                                   const std::optional<platform::ContactRequest>& accepting,
                                   const std::vector<uint32_t>& account_references, uint64_t nonce)
{
    const auto recipient_key_id{platform::helpers::Dip15SelectRecipientKey(recipient)};
    const auto* recipient_key{recipient_key_id ? FindKey(recipient, *recipient_key_id) : nullptr};
    if (!recipient_key) {
        fail(recipient.id, tr("This DashPay user's identity has no key a contact request can be sent to."));
        return;
    }
    // Our keys come from the proved identity: the one documents are signed
    // with, and the ENCRYPTION key the request references and the ECDH
    // secret is derived from.
    const auto document_key{m_service.documentSigningKey(sender)};
    if (!document_key) {
        fail(recipient.id, tr("This wallet holds no key that can sign for your identity."));
        return;
    }
    const uint32_t encryption_key_id{m_service.identityFlow().record().encryption_key_id};
    const auto* encryption_key{FindKey(sender, encryption_key_id)};
    if (!encryption_key || encryption_key->purpose != platform::IdentityPublicKey::Purpose::ENCRYPTION) {
        fail(recipient.id, tr("This wallet holds no encryption key of your identity."));
        return;
    }

    // One unlock for all of the key work: accepting their request, our
    // receiving keychain, the accountReference MAC, the ECDH secret and the
    // signature. It is released before the network wait.
    auto attempt{m_service.beginSigningOperation(platform::OperationKind::CONTACT_REQUEST, {document_key->id},
                                                 *document_key, std::nullopt)};
    if (!attempt.op) {
        fail(recipient.id, attempt.unlock_declined ? UnlockDeclinedText() : attempt.refusal);
        return;
    }
    QString error;
    if (accepting && !establish(*accepting, recipient, GetTime(), error)) {
        fail(recipient.id, error);
        return;
    }

    // Our receiving keychain for this contact, in the compact form the
    // request carries. Its rescan birth time is the time of our own earlier
    // request to this contact when there was one, otherwise now: nobody can
    // have paid a chain that did not exist yet.
    const auto earlier{platform::DecodeContactOutRecord(
        m_service.readRecord(ContactKey(platform::records::CONTACT_OUT_PREFIX, recipient.id)))};
    wallet::FriendshipXpub xpub;
    if (!prepareReceivingKeychain(recipient.id, xpub, earlier.value_or(GetTime()), error)) {
        fail(recipient.id, error);
        return;
    }
    wallet::CompactXpub compact_xpub;
    if (!wallet::CompactXpubBytes(xpub, compact_xpub)) {
        fail(recipient.id, KeyWorkText(), QStringLiteral("Operation: Serialize friendship key"));
        return;
    }
    // The accountReference MAC over that xpub, keyed by our ENCRYPTION key
    // inside the wallet; only the 32-byte MAC leaves it.
    Wallet& wallet{m_service.walletModel().wallet()};
    const auto mac{wallet.platformAccountReferenceMac(wallet::IdentityAuthKey{0, encryption_key_id}, compact_xpub)};
    if (!mac) {
        fail(recipient.id, KeyWorkText(), QStringLiteral("Operation: Derive accountReference MAC"));
        return;
    }
    std::array<uint8_t, 32> mac_bytes;
    std::copy(mac.value.begin(), mac.value.end(), mac_bytes.begin());
    // One version above our latest request to this contact on chain.
    std::optional<uint32_t> highest_version;
    for (const uint32_t reference : account_references) {
        const auto unmasked{platform::helpers::Dip15UnmaskAccountReference(mac_bytes, reference)};
        highest_version = std::max(highest_version.value_or(0), unmasked.version);
    }
    const uint32_t version{highest_version ? *highest_version + 1 : 0};
    if (version > 0x0F) {
        fail(recipient.id, tr("You have sent this person the most contact requests DashPay allows."));
        return;
    }
    const uint32_t account_reference{platform::helpers::Dip15AccountReferenceFromMac(mac_bytes, ACCOUNT, version)};

    platform::ContactRequestInput input;
    input.to_user_id = recipient.id;
    input.sender_key_index = encryption_key_id;
    input.recipient_key_index = recipient_key->id;
    input.recipient_pubkey = CPubKey{recipient_key->data.begin(), recipient_key->data.end()};
    input.account_reference = account_reference;
    input.compact_xpub = compact_xpub;
    // The wallet computes the secret between our ENCRYPTION key and the
    // recipient key; the SDK encrypts with it and zeroizes its copy.
    const auto secret{wallet.platformECDHSecret(wallet::IdentityAuthKey{0, encryption_key_id}, input.recipient_pubkey)};
    if (!secret || secret.value.size() != input.shared_secret.size()) {
        fail(recipient.id, KeyWorkText(), QStringLiteral("Operation: Derive ECDH secret"));
        return;
    }
    std::copy(secret.value.begin(), secret.value.end(), input.shared_secret.begin());
    auto built{m_service.client().buildContactRequest(*attempt.op, sender, recipient, nonce, input)};
    attempt.op.reset();
    memory_cleanse(input.shared_secret.data(), input.shared_secret.size());
    if (!built) {
        fail(recipient.id, {StatusKind::INTERNAL, 0, util::ErrorString(built).original}, tr("Build contact request"));
        return;
    }
    // Platform stamps the document with its block time; a lower bound a few
    // minutes back keeps the confirmation query to this request's page.
    const uint64_t since_ms{static_cast<uint64_t>(std::max<int64_t>(0, GetTime() - CONFIRM_CLOCK_SLACK)) * 1000};
    QPointer<ContactFlow> self{this};
    m_service.client().broadcastStateTransition(built->bytes, [self, recipient, account_reference,
                                                               since_ms](platform::Status status) {
        if (!self) return;
        self->m_service.post([self, recipient, account_reference, since_ms, status = std::move(status)] {
            if (!self) return;
            if (status.kind == StatusKind::OK || status.kind == StatusKind::ALREADY_EXISTS) {
                // Sent: from here on only a proved read or the contacts
                // refresh says what became of it, never a failure.
                Q_EMIT self->requestPending(Hex(recipient.id));
                self->confirmRequest(recipient.id, account_reference, since_ms, platform::Identifier{},
                                     MAX_REQUEST_PAGES, GetTime() + CONFIRM_WINDOW_SECONDS, FIRST_CONFIRM_INTERVAL_MS);
            } else {
                self->fail(recipient.id, status, tr("Send contact request"));
            }
        });
    });
}

std::optional<int64_t> ContactFlow::ourRequestTime(const platform::Identifier& to_identity) const
{
    const auto recorded{platform::DecodeContactOutRecord(
        m_service.readRecord(ContactKey(platform::records::CONTACT_OUT_PREFIX, to_identity)))};
    return recorded ? recorded : m_service.outgoingRequestTime(to_identity);
}

bool ContactFlow::establish(const platform::ContactRequest& incoming, const platform::Identity& sender,
                            int64_t birth_time, QString& error)
{
    const auto their_xpub{decryptXpub(incoming, sender, error)};
    if (!their_xpub || !importKeychains(incoming.owner_id, *their_xpub, birth_time, error)) return false;
    const std::string key{ContactKey(platform::records::CONTACT_KEY_PREFIX, incoming.owner_id)};
    const std::vector<unsigned char> xpub{their_xpub->begin(), their_xpub->end()};
    // A newer request with other payment addresses starts their new chain
    // from its first address.
    const auto previous{m_service.readRecord(key)};
    if (!previous.empty() && previous != xpub) {
        m_service.writeRecord(ContactKey(platform::records::CONTACT_PAY_INDEX_PREFIX, incoming.owner_id), {});
    }
    m_service.writeRecord(key, xpub);
    m_service.writeRecord(ContactKey(platform::records::CONTACT_IN_PREFIX, incoming.owner_id),
                          {incoming.document_id.begin(), incoming.document_id.end()});
    return true;
}

bool ContactFlow::establishAnswered(const platform::ContactRequest& incoming, const platform::Identity& sender,
                                    int64_t our_request_time, QString& error)
{
    // The keychain's rescan birth is the time of our own request, already on
    // chain: a second request would only rotate the account reference.
    if (!establish(incoming, sender, our_request_time, error)) return false;
    m_service.writeRecord(ContactKey(platform::records::CONTACT_OUT_PREFIX, incoming.owner_id),
                          platform::EncodeContactOutRecord(our_request_time));
    return true;
}

void ContactFlow::accept(const platform::ContactRequest& incoming)
{
    QPointer<ContactFlow> self{this};
    m_service.client().getIdentity(incoming.owner_id, [self, incoming](platform::Result<platform::Identity> result) {
        if (!self) return;
        self->m_service.post([self, incoming, result = std::move(result)] {
            if (!self) return;
            if (!result.ok()) {
                self->fail(incoming.owner_id, tr("Dash Core could not verify who sent this request. Try again later."),
                           PlatformUi::Describe(result.status, PlatformUi::Context::CONTACT_ACCEPT,
                                                tr("Read sender identity"))
                               .details);
                return;
            }
            const auto our_request_time{self->ourRequestTime(incoming.owner_id)};
            if (!our_request_time) {
                // Our request goes back under the same unlock as the import.
                self->sendRequest(*result.value, incoming);
                return;
            }
            const WalletModel::UnlockContext unlock{self->m_service.walletModel().requestUnlock()};
            if (!unlock.isValid()) {
                self->fail(incoming.owner_id, UnlockDeclinedText());
                return;
            }
            QString error;
            if (!self->establishAnswered(incoming, *result.value, *our_request_time, error)) {
                self->fail(incoming.owner_id, error);
                return;
            }
            Q_EMIT self->requestSent(Hex(incoming.owner_id));
        });
    });
}

void ContactFlow::completeAccepted(const platform::ContactRequest& incoming)
{
    QPointer<ContactFlow> self{this};
    m_service.client().getIdentity(incoming.owner_id, [self, incoming](platform::Result<platform::Identity> result) {
        if (!self) return;
        self->m_service.post([self, incoming, result = std::move(result)] {
            if (!self) return;
            const auto our_request_time{self->ourRequestTime(incoming.owner_id)};
            QString error;
            bool retryable{true};
            if (!result.ok()) {
                error = result.provenAbsent()
                            ? tr("This DashPay user no longer exists on Dash Platform.")
                            : PlatformUi::Describe(result.status, PlatformUi::Context::CONTACT_ACCEPT, {}).text;
                retryable = !result.provenAbsent();
            } else if (!our_request_time) {
                error = tr("Your own request to this contact was not found yet. Refresh your contacts in a minute.");
            } else if (!self->establishAnswered(incoming, *result.value, *our_request_time, error)) {
                // No passphrase prompt: a wallet locked in the meantime
                // leaves the contact accepted for the user to finish; any
                // other refusal repeats on every attempt.
                const auto status{self->m_service.walletModel().getEncryptionStatus()};
                retryable = status == WalletModel::Locked || status == WalletModel::UnlockedForMixingOnly;
            }
            if (error.isEmpty()) {
                LogPrint(BCLog::PLATFORM, "Platform GUI: contact %s accepted our request\n", HexStr(incoming.owner_id));
            } else {
                LogPrint(BCLog::PLATFORM, "Platform GUI: contact %s accepted our request; not established: %s\n",
                         HexStr(incoming.owner_id), result.ok() ? error.toStdString() : result.status.message);
            }
            Q_EMIT self->acceptedCompleted(Hex(incoming.owner_id), error, retryable);
        });
    });
}

std::optional<wallet::CompactXpub> ContactFlow::decryptXpub(const platform::ContactRequest& incoming,
                                                            const platform::Identity& sender, QString& error)
{
    const auto* sender_key{FindKey(sender, incoming.sender_key_index)};
    if (!sender_key || sender_key->type != platform::IdentityPublicKey::Type::ECDSA_SECP256K1) {
        error = tr("The sender's identity has no key this request can be read with.");
        return std::nullopt;
    }
    // Only the two keys this wallet registered for contact requests run
    // ECDH; a request encrypted to any other key of ours (the MASTER key by
    // an older wallet, a signing key) is refused before the policy check.
    const auto& record{m_service.identityFlow().record()};
    std::optional<platform::IdentityPublicKey::Purpose> our_purpose;
    if (incoming.recipient_key_index == record.encryption_key_id) {
        our_purpose = platform::IdentityPublicKey::Purpose::ENCRYPTION;
    } else if (incoming.recipient_key_index == record.decryption_key_id) {
        our_purpose = platform::IdentityPublicKey::Purpose::DECRYPTION;
    }
    if (!our_purpose ||
        !platform::helpers::Dip15ReceiveKeysAcceptable(sender_key->purpose, *our_purpose, incoming.recipient_key_index)) {
        error = tr("This request was made by a wallet too old to exchange contacts with. Ask the sender to update it.");
        return std::nullopt;
    }
    const CPubKey sender_pubkey{sender_key->data.begin(), sender_key->data.end()};
    const auto secret{
        m_service.walletModel().wallet().platformECDHSecret(wallet::IdentityAuthKey{0, incoming.recipient_key_index},
                                                            sender_pubkey)};
    if (!secret || secret.value.size() != 32) {
        error = secret.status == wallet::PlatformKeyStatus::WALLET_LOCKED
                    ? tr("Unlock the wallet to accept the request.")
                    : tr("Dash Core could not read this contact request.");
        return std::nullopt;
    }
    std::array<uint8_t, 32> shared_secret;
    std::copy(secret.value.begin(), secret.value.end(), shared_secret.begin());
    auto decrypted{platform::helpers::Dip15DecryptXpub(shared_secret, incoming.encrypted_public_key)};
    memory_cleanse(shared_secret.data(), shared_secret.size());
    if (!decrypted) {
        error = tr("Dash Core could not read this contact request.");
        return std::nullopt;
    }
    return *decrypted;
}

bool ContactFlow::importKeychains(const platform::Identifier& their_identity, const wallet::CompactXpub& their_xpub,
                                  int64_t birth_time, QString& error)
{
    // Validate the decrypted material before it is persisted; the contact's
    // chain itself never enters the wallet (see ensureFriendshipReceivingKeychain).
    const CPubKey pubkey{their_xpub.begin() + 36, their_xpub.end()};
    if (!pubkey.IsFullyValid() || !pubkey.IsCompressed()) {
        error = tr("This contact request carries an invalid payment key.");
        return false;
    }
    wallet::FriendshipXpub our_xpub;
    return prepareReceivingKeychain(their_identity, our_xpub, birth_time, error);
}

bool ContactFlow::prepareReceivingKeychain(const platform::Identifier& their_identity, wallet::FriendshipXpub& xpub,
                                           int64_t birth_time, QString& error)
{
    const auto my_id{m_service.myIdentityId()};
    if (!my_id) {
        error = tr("Register a username before adding contacts.");
        return false;
    }
    const uint256 my_hash{Span{my_id->data(), my_id->size()}};
    const uint256 their_hash{Span{their_identity.data(), their_identity.size()}};
    Wallet& wallet{m_service.walletModel().wallet()};
    const auto keychain{wallet.ensureFriendshipReceivingKeychain(
        wallet::FriendshipKeychainRequest{ACCOUNT, my_hash, their_hash, birth_time})};
    if (!keychain) {
        error = keychain.status == wallet::PlatformKeyStatus::WALLET_LOCKED
                    ? tr("Unlock the wallet to finish adding this contact.")
                    : tr("Dash Core could not add this contact's payment keys to the wallet.");
        return false;
    }
    xpub = keychain.value;

    // Ranged descriptors cannot carry an address-book label, so label the
    // receiving chain explicitly for transaction history.
    const std::string label{m_service.contactAddressLabel(Hex(their_identity)).toStdString()};
    for (uint32_t i = 0; i < LABELLED_ADDRESSES; ++i) {
        CTxDestination destination;
        if (!wallet::DeriveFriendshipPaymentDestination(keychain.value, i, destination)) break;
        wallet.setAddressBook(destination, label, "receive");
    }
    return true;
}

void ContactFlow::confirmRequest(const platform::Identifier& to_identity, uint32_t account_reference, uint64_t since_ms,
                                 const platform::Identifier& start_after, int pages_left, int64_t deadline, int interval_ms)
{
    const auto my_id{m_service.myIdentityId()};
    if (!my_id) {
        // Sent, so never a failure: the contacts refresh reconciles it.
        Q_EMIT requestUnconfirmed(Hex(to_identity));
        return;
    }
    QPointer<ContactFlow> self{this};
    m_service.client().getContactRequests(
        *my_id, /*to_me=*/false, since_ms, start_after,
        [self, to_identity, account_reference, since_ms, pages_left, deadline,
         interval_ms](platform::Result<platform::Paged<platform::ContactRequest>> result) {
            if (!self) return;
            self->m_service.post([self, to_identity, account_reference, since_ms, pages_left, deadline, interval_ms,
                                  result = std::move(result)] {
                if (!self) return;
                // The request just built, not an earlier one to the same
                // contact: the account reference is unique per version.
                if (result.ok() &&
                    std::any_of(result.value->items.begin(), result.value->items.end(), [&](const auto& request) {
                        return request.to_user_id == to_identity && request.account_reference == account_reference;
                    })) {
                    // Keep the time of the first request we confirmed: it is
                    // the keychain's birth and a resend must not move it.
                    const std::string key{ContactKey(platform::records::CONTACT_OUT_PREFIX, to_identity)};
                    if (!platform::DecodeContactOutRecord(self->m_service.readRecord(key))) {
                        self->m_service.writeRecord(key, platform::EncodeContactOutRecord(GetTime()));
                    }
                    Q_EMIT self->requestSent(Hex(to_identity));
                    return;
                }
                if (result.ok() && result.value->has_more && pages_left > 1) {
                    self->confirmRequest(to_identity, account_reference, since_ms, result.value->next_start_after,
                                         pages_left - 1, deadline, interval_ms);
                    return;
                }
                if (GetTime() >= deadline) {
                    LogPrint(BCLog::PLATFORM, "Platform GUI: contact request to %s not confirmed yet (%s)\n",
                             HexStr(to_identity), result.ok() || result.provenAbsent() ? "absent" : result.status.message);
                    Q_EMIT self->requestUnconfirmed(Hex(to_identity));
                    return;
                }
                const int next_interval{std::min(interval_ms * 3 / 2, MAX_CONFIRM_INTERVAL_MS)};
                QTimer::singleShot(interval_ms, self,
                                   [self, to_identity, account_reference, since_ms, deadline, next_interval] {
                                       if (self) {
                                           self->confirmRequest(to_identity, account_reference, since_ms,
                                                                platform::Identifier{}, MAX_REQUEST_PAGES, deadline,
                                                                next_interval);
                                       }
                                   });
            });
        });
}
