// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_QT_PLATFORM_PROFILEFLOW_H
#define BITCOIN_QT_PLATFORM_PROFILEFLOW_H

#include <platform/types.h>

#include <QObject>
#include <QString>

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <vector>

class ContactFlow;
class IdentityFlow;
class PlatformService;

/**
 * The DashPay profiles of the wallet's identity and its contacts, as the
 * mobile wallets keep them: a display name and a public message, read from
 * Platform and held in memory only.
 *
 * Our own is read once the identity is found, and our contacts' in one
 * batched read once the contacts are. Publishing creates the profile the
 * first time and updates it after that. Avatars are out of scope: no avatar
 * field is ever sent, so one set from another wallet is kept as it is.
 */
class ProfileFlow : public QObject
{
    Q_OBJECT

public:
    //! DIP-15's limits, in characters.
    static constexpr int MAX_DISPLAY_NAME{25};
    static constexpr int MAX_PUBLIC_MESSAGE{140};

    enum class Step {
        IDLE,
        SAVING, //!< from the passphrase to Platform's answer
    };

    ProfileFlow(PlatformService& service, IdentityFlow& identity_flow, const ContactFlow& contact_flow);

    Step step() const { return m_step; }
    //! Whether our own profile was read since the identity was found.
    bool ownRead() const;
    //! The profile Platform holds for owner; nullopt if it was not read yet,
    //! or there is none.
    std::optional<platform::Profile> profile(const platform::Identifier& owner) const;
    //! The profile of the wallet's identity.
    std::optional<platform::Profile> own() const;
    //! Why the last save failed, else why the last read of the profiles did;
    //! empty if neither did.
    const QString& error() const { return m_error.isEmpty() ? m_read_error : m_error; }

    //! Publish the profile, creating it first if the identity has none. An
    //! empty field, or one that is already published, is left as it is:
    //! DashPay profiles can't clear a field.
    void save(const QString& display_name, const QString& public_message);

Q_SIGNALS:
    void changed();

private:
    void update();
    //! Read the profiles not read yet, our own and the contacts', at most
    //! one batch at a time.
    void read();
    void publish();
    void onSaved(platform::Result<platform::Profile> result);
    void finish(const QString& error = {});

    PlatformService& m_service;
    IdentityFlow& m_identity_flow;
    const ContactFlow& m_contact_flow;
    Step m_step{Step::IDLE};
    //! The save's call, and what it publishes.
    std::weak_ptr<void> m_call;
    //! The write was sent.
    bool m_sent{false};
    platform::ProfileFields m_fields;
    //! The profiles read, by owner; nullopt: the owner has none.
    std::map<platform::Identifier, std::optional<platform::Profile>> m_profiles;
    std::weak_ptr<void> m_read_call;
    QString m_error;
    QString m_read_error;
    //! Counts the times the contacts' profiles were dropped to be read again,
    //! so that the answer to an older read is not taken for the new one.
    uint64_t m_generation{0};
    bool m_swept{false};
};

#endif // BITCOIN_QT_PLATFORM_PROFILEFLOW_H
