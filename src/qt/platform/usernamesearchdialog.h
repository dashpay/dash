// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_QT_PLATFORM_USERNAMESEARCHDIALOG_H
#define BITCOIN_QT_PLATFORM_USERNAMESEARCHDIALOG_H

#include <QDialog>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

class ContactFlow;
class PlatformService;

QT_BEGIN_NAMESPACE
class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
QT_END_NAMESPACE

/**
 * Finds the DashPay usernames that start with a prefix, as the mobile
 * wallets' add-contact screen does, and adds the chosen one as a contact
 * through the contact flow.
 *
 * The prefix is searched when the user asks (Enter or Search), not as it is
 * typed. The SDK never says whether more usernames exist than the ones it
 * returned, so the list claims nothing about its end.
 */
class UsernameSearchDialog : public QDialog
{
    Q_OBJECT

public:
    //! The most usernames asked for: the SDK's own cap, which the mobile
    //! wallets use.
    static constexpr uint32_t LIMIT{100};
    //! Shorter prefixes are not searched.
    static constexpr size_t MIN_PREFIX_LENGTH{2};

    //! Both must outlive the dialog.
    UsernameSearchDialog(PlatformService& service, ContactFlow& flow, QWidget* parent = nullptr);

private:
    void search();
    //! Start the search call on the current client, if there is one.
    void run();
    //! A rebuilt client never answers a call of the one before it.
    void update();
    void updateAdd();
    void add();

    PlatformService& m_service;
    ContactFlow& m_flow;
    //! The prefix being searched; empty once the search is answered.
    std::string m_prefix;
    std::weak_ptr<void> m_call;

    QLineEdit* m_input{nullptr};
    QPushButton* m_search{nullptr};
    QLabel* m_status{nullptr};
    QListWidget* m_results{nullptr};
    QPushButton* m_add{nullptr};
};

#endif // BITCOIN_QT_PLATFORM_USERNAMESEARCHDIALOG_H
