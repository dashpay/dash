// Copyright (c) 2011-2022 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_QT_SENDCOINSENTRY_H
#define BITCOIN_QT_SENDCOINSENTRY_H

#if defined(HAVE_CONFIG_H)
#include <config/bitcoin-config.h>
#endif

#include <qt/sendcoinsrecipient.h>

#include <QWidget>

class WalletModel;
#ifdef ENABLE_PLATFORM_GUI
class PlatformService;

QT_BEGIN_NAMESPACE
class QAction;
QT_END_NAMESPACE
#endif

namespace interfaces {
class Node;
} // namespace interfaces

namespace Ui {
    class SendCoinsEntry;
}

/**
 * A single entry in the dialog for sending bitcoins.
 */
class SendCoinsEntry : public QWidget
{
    Q_OBJECT

public:
    explicit SendCoinsEntry(QWidget* parent = nullptr);
    ~SendCoinsEntry();

    void setModel(WalletModel *model);
#ifdef ENABLE_PLATFORM_GUI
    //! The wallet's DashPay service, or nullptr when DashPay is off: with a
    //! service a username may be entered as the recipient and is replaced by
    //! the proof-resolved payment address of that contact.
    void setPlatformService(PlatformService* service);
    //! Why the recipient is a username without a payment address yet (still
    //! being looked up, or refused); empty when it is not a username. Starts
    //! the lookup when none has run.
    QString unresolvedUsername();
#endif
    bool validate(interfaces::Node& node);
    SendCoinsRecipient getValue();

    /** Return whether the entry is still empty and unedited */
    bool isClear();

    void setValue(const SendCoinsRecipient &value);
    void setAddress(const QString &address);
    void setAmount(const CAmount &amount);

    /** Set up the tab chain manually, as Qt messes up the tab chain by default in some cases
     *  (issue https://bugreports.qt-project.org/browse/QTBUG-10907).
     */
    QWidget *setupTabChain(QWidget *prev);

    void setFocus();

public Q_SLOTS:
    void clear();
    void checkSubtractFeeFromAmount();

Q_SIGNALS:
    void removeEntry(SendCoinsEntry *entry);
    void useAvailableBalance(SendCoinsEntry* entry);
    void payAmountChanged();
    void subtractFeeFromAmountChanged();

private Q_SLOTS:
    void deleteClicked();
    void useAvailableBalanceClicked();
    void on_payTo_textChanged(const QString &address);
#ifdef ENABLE_PLATFORM_GUI
    //! A recipient that could be either a username or an address so far is
    //! looked up only when the entry is left.
    void on_payTo_editingFinished();
#endif
    void on_addressBookButton_clicked();
    void on_pasteButton_clicked();
    void updateDisplayUnit();
#ifdef ENABLE_PLATFORM_GUI
    void on_contactsButton_clicked();
#endif

protected:
    void changeEvent(QEvent* e) override;
#ifdef ENABLE_PLATFORM_GUI
    //! Leaving the recipient field looks a username up. editingFinished
    //! alone misses a field that loses focus without its text being
    //! validated (a username the address validator calls intermediate).
    bool eventFilter(QObject* watched, QEvent* event) override;
#endif

private:
    SendCoinsRecipient recipient;
    Ui::SendCoinsEntry *ui;
    WalletModel* model{nullptr};
#ifdef ENABLE_PLATFORM_GUI
    enum class UsernameStatus {
        None,
        Progress,
        Verified,
        Failed
    };
    //! The lookup state: a trailing icon in the field (details in its
    //! tooltip) and the sentence on the line under it.
    void showUsernameStatus(UsernameStatus status, const QString& text = {}, const QString& details = {});
    void applyUsernameStatusStyle();
    //! Resolve the recipient when it is a username that is not being looked
    //! up already.
    void lookUpUsername();
    //! The "@" contact picker opens only while DashPay can reach Platform.
    void updateContactsButton();

    PlatformService* m_platform_service{nullptr};
    QTimer* m_username_debounce{nullptr};
    QAction* m_username_status_action{nullptr};
    QAction* m_contacts_shortcut{nullptr};
    UsernameStatus m_username_status{UsernameStatus::None};
    QString m_pending_username;
    //! The contact label the last resolved username filled in; it is
    //! cleared when the recipient changes unless the user edited it.
    QString m_auto_label;
#endif

    /** Set required icons for buttons inside the dialog */
    void setButtonIcons();
    bool updateLabel(const QString &address);
};

#endif // BITCOIN_QT_SENDCOINSENTRY_H
