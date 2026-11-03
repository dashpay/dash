// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_QT_PLATFORM_PLATFORMPAGE_H
#define BITCOIN_QT_PLATFORM_PLATFORMPAGE_H

#include <qt/platform/platformservice.h>

#include <QWidget>

#include <memory>

QT_BEGIN_NAMESPACE
class QLabel;
class QPushButton;
QT_END_NAMESPACE

/** The DashPay tab of one wallet. Only built with --enable-platform-gui.
 *
 *  It owns the wallet's PlatformService while both models are set, offers
 *  the first DashPay click with a one-line privacy notice, and says why
 *  DashPay can't run when a gate is closed. */
class PlatformPage : public QWidget
{
    Q_OBJECT

public:
    explicit PlatformPage(PlatformService::Sources sources = {}, QWidget* parent = nullptr);
    ~PlatformPage() override;

    void setWalletModel(WalletModel* wallet_model);
    void setClientModel(ClientModel* client_model);
    PlatformService* service() const { return m_service.get(); }

Q_SIGNALS:
    //! A service was created, or is about to be destroyed (nullptr) while
    //! the models it holds and the page's widgets still exist.
    void serviceChanged(PlatformService* service);

private:
    void resetService();
    void refresh();

    const PlatformService::Sources m_sources;
    WalletModel* m_wallet_model{nullptr};
    ClientModel* m_client_model{nullptr};
    std::unique_ptr<PlatformService> m_service;

    QLabel* m_status{nullptr};
    QPushButton* m_start{nullptr};
    QPushButton* m_stop{nullptr};
};

#endif // BITCOIN_QT_PLATFORM_PLATFORMPAGE_H
