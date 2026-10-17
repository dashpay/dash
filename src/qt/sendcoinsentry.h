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
    bool validate(interfaces::Node& node);
    SendCoinsRecipient getValue();

    /** Return whether the entry is still empty and unedited */
    bool isClear();

    void setValue(const SendCoinsRecipient &value);
    void setAddress(const QString &address);
    void setAmount(const CAmount &amount);
#ifdef ENABLE_PLATFORM_GUI
    /** Take a DashPay username in the pay-to field as well as an address,
     *  and announce it when the field is left. */
    void allowUsername();
#endif

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
#ifdef ENABLE_PLATFORM_GUI
    /** The user left the pay-to field holding something that is not an address. */
    void usernameEntered(const QString& username);
#endif

private Q_SLOTS:
    void deleteClicked();
    void useAvailableBalanceClicked();
    void on_payTo_textChanged(const QString &address);
    void on_addressBookButton_clicked();
    void on_pasteButton_clicked();
    void updateDisplayUnit();

protected:
    void changeEvent(QEvent* e) override;

private:
    SendCoinsRecipient recipient;
    Ui::SendCoinsEntry *ui;
    WalletModel* model{nullptr};
#ifdef ENABLE_PLATFORM_GUI
    /** The text of the pay-to field was announced as a username. */
    bool m_announced{false};
#endif

    /** Set required icons for buttons inside the dialog */
    void setButtonIcons();
    bool updateLabel(const QString &address);
};

#endif // BITCOIN_QT_SENDCOINSENTRY_H
