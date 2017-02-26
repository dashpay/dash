// Copyright (c) 2014-2017 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef DARKSENDCONFIG_H
#define DARKSENDCONFIG_H

#include <QDialog>

namespace Ui {
    class DarksendConfig;
}
class WalletModel;

/** Multifunctional dialog to ask for passphrases. Used for encryption, unlocking, and changing the passphrase.
 */
class DarksendConfig : public QDialog
{
    Q_OBJECT

public:

    DarksendConfig(QWidget *parent = 0);
    ~DarksendConfig();

    void setModel(WalletModel *model);


private:
    Ui::DarksendConfig *ui;
    WalletModel *model;
    void configure(bool enabled, int coins, int rounds);

private Q_SLOTS:

    void clickBasic();
    void clickHigh();
    void clickMax();
};

#endif // DARKSENDCONFIG_H
