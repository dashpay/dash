// Copyright (c) 2011-2021 The Bitcoin Core developers
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
    void setPlatformService(PlatformService* service);
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
    void on_addressBookButton_clicked();
    void on_pasteButton_clicked();
    void updateDisplayUnit();
#ifdef ENABLE_PLATFORM_GUI
    void on_contactsButton_clicked();
#endif

protected:
    void changeEvent(QEvent* e) override;

private:
    SendCoinsRecipient recipient;
    Ui::SendCoinsEntry *ui;
    WalletModel* model{nullptr};
#ifdef ENABLE_PLATFORM_GUI
    enum class UsernameStatus { Progress, Verified, Failed };
    void showUsernameStatus(const QString& text, UsernameStatus status);

    PlatformService* m_platform_service{nullptr};
    QTimer* m_username_debounce{nullptr};
    QAction* m_username_status_action{nullptr};
    QString m_pending_username;
#endif

    /** Set required icons for buttons inside the dialog */
    void setButtonIcons();
    bool updateLabel(const QString &address);
};

#endif // BITCOIN_QT_SENDCOINSENTRY_H
