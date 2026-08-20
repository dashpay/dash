// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_QT_PLATFORM_PLATFORMSERVICE_H
#define BITCOIN_QT_PLATFORM_PLATFORMSERVICE_H

#include <consensus/amount.h>
#include <platform/client.h>
#include <platform/params.h>
#include <platform/types.h>
#include <qt/walletmodel.h>
#include <uint256.h>

#include <QObject>
#include <QSet>
#include <QString>
#include <QTimer>

#include <functional>
#include <memory>
#include <optional>
#include <vector>

class ClientModel;
class ContactFlow;
class IdentityFlow;
class PlatformRecovery;
/**
 * Per-wallet orchestrator for all Dash Platform interactions. This is the
 * only object GUI pages talk to. It owns the flows (identity/username
 * registration, contacts), marshals PlatformClient callbacks onto the GUI
 * thread, persists flow state through the wallet's platform data records,
 * and feeds node-local context (evonode endpoints, quorum keys) into the
 * client.
 */
class PlatformService : public QObject
{
    Q_OBJECT

public:
    PlatformService(WalletModel& wallet_model, ClientModel& client_model,
                    std::shared_ptr<platform::PlatformClient> client, QObject* parent = nullptr);
    ~PlatformService() override;

    WalletModel& walletModel() { return m_wallet_model; }
    ClientModel& clientModel() { return m_client_model; }
    platform::PlatformClient& client() { return *m_client; }
    const platform::Params& params() const { return m_params; }

    IdentityFlow& identityFlow() { return *m_identity_flow; }
    ContactFlow& contactFlow() { return *m_contact_flow; }

    //! Fetch this identity's incoming + outgoing contact requests; emits
    //! contactsUpdated().
    void refreshContacts();

    //! Resolve an identity proof and send/accept a DashPay contact request.
    bool sendContactRequest(const QString& identity_hex, QString& error);
    bool acceptContact(const QString& identity_hex, QString& error);

    //! Resolve an established contact username to the next DIP-15 payment
    //! address. Emits paymentAddressResolved().
    void resolvePaymentAddress(const QString& username);

    //! Broadcast a DashPay profile create/update.
    bool updateProfile(const QString& display_name, const QString& public_message, const QString& avatar_url,
                       QString& error);

    //! The registered username of this wallet's identity, if any.
    QString myUsername() const;
    std::optional<platform::Identifier> myIdentityId() const;

    //! Proof-verified credit balance of this wallet's identity; emits
    //! identityBalanceLoaded().
    void refreshIdentityBalance();

    //! Async name availability probe (proof-backed absence check).
    //! Emits nameAvailability().
    void checkNameAvailability(const QString& name);

    //! Async proof-verified contested-name vote state
    //! (getContestedResourceVoteState). Emits contestedNameState().
    void checkContestedNameState(const QString& normalized_label);

    //! Async prefix search; emits searchResults().
    void searchNames(const QString& prefix);

    //! Async profile fetch; emits profileLoaded().
    void loadProfile(const platform::Identifier& identity);

    //! Wallet platform-data record helpers (used by the flows).
    bool writeRecord(const std::string& key, const std::vector<unsigned char>& value);
    std::vector<unsigned char> readRecord(const std::string& key) const;

    //! Return proof-verified contact metadata cached in wallet platform data.
    QString contactMetadata(const QString& identity_hex, const char* field) const;

    //! Human-readable name for a contact, best first: username, profile
    //! display name, shortened identity id.
    QString contactDisplayString(const QString& identity_hex) const;

    //! Address-book label applied to DIP-15 friendship addresses so
    //! transaction history attributes payments to the contact.
    QString contactAddressLabel(const QString& identity_hex) const;

    //! Run a callback on the GUI thread (safe from client threads; dropped
    //! if the service is destroyed first).
    void post(std::function<void()> fn);

Q_SIGNALS:
    void nameAvailability(const QString& normalized_label, bool available, bool contested);
    void nameAvailabilityFailed(const QString& normalized_label, const QString& error);
    //! Proof-verified contested vote state for a label; error is empty on
    //! success. Emitted on the GUI thread.
    void contestedNameState(const QString& normalized_label,
                            const platform::ContestedNameState& state, const QString& error);
    void searchResults(const QString& prefix, const QVector<QPair<QString, QString>>& results); //!< (label, identity id hex)
    //! Emitted with empty fields when the identity verifiably has no profile.
    void profileLoaded(const QString& identity_hex, const QString& display_name, const QString& public_message, const QString& avatar_url);
    void profileLoadFailed(const QString& identity_hex, const QString& error);
    void identityStateChanged();
    void identityBalanceLoaded(quint64 credits);
    void flowFailed(const QString& step, const QString& error);
    //! (identity hex, username) pairs for incoming and outgoing requests.
    void contactsUpdated(const QVector<QPair<QString, QString>>& incoming,
                         const QVector<QPair<QString, QString>>& outgoing);
    void profileUpdated(bool ok, const QString& error);
    void contactRequestFinished(const QString& identity_hex, bool ok, const QString& error);
    void paymentAddressResolved(const QString& username, const QString& address, const QString& error);

private Q_SLOTS:
    void updateNodeContext();

private:
    enum class SigningOperation {
        NONE,
        CONTACT,
        PROFILE,
    };

    bool beginSigningOperation(SigningOperation operation, const QString& identity_hex, QString& error);
    void finishContactRequest(const QString& identity_hex, bool ok, const QString& error);
    void finishProfileUpdate(bool ok, const QString& error);
    void hydrateContactMetadata(const platform::Identifier& identity);
    void setContactMetadata(const QString& identity_hex, const char* field, const QString& value);
    void publishProfile(platform::Profile profile);
    WalletModel& m_wallet_model;
    ClientModel& m_client_model;
    std::shared_ptr<platform::PlatformClient> m_client;
    platform::Params m_params;

    std::unique_ptr<IdentityFlow> m_identity_flow;
    std::unique_ptr<ContactFlow> m_contact_flow;
    std::unique_ptr<PlatformRecovery> m_recovery;
    std::vector<platform::ContactRequest> m_incoming_contacts;
    std::vector<platform::ContactRequest> m_outgoing_contacts;
    QSet<QString> m_contact_metadata_pending;
    SigningOperation m_signing_operation{SigningOperation::NONE};
    QString m_signing_identity;
    std::unique_ptr<WalletModel::UnlockContext> m_signing_unlock;
    QTimer* m_tick_timer{nullptr};         //!< drives flow advance/retry
    QTimer* m_context_timer{nullptr};      //!< refreshes endpoints/quorum keys
};

#endif // BITCOIN_QT_PLATFORM_PLATFORMSERVICE_H
