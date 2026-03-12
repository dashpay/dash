// Copyright (c) 2011-2021 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_QT_SPLASHSCREEN_H
#define BITCOIN_QT_SPLASHSCREEN_H

#include <QElapsedTimer>
#include <QTimer>
#include <QWidget>

#include <memory>

class NetworkStyle;

namespace interfaces {
class Handler;
class Node;
class Wallet;
};

/** Class for the splashscreen with information of the running client.
 *
 * @note this is intentionally not a QSplashScreen. Dash Core initialization
 * can take a long time, and in that case a progress window that cannot be
 * moved around and minimized has turned out to be frustrating to the user.
 */
class SplashScreen : public QWidget
{
    Q_OBJECT

public:
    explicit SplashScreen(const NetworkStyle *networkStyle);
    ~SplashScreen();
    void setNode(interfaces::Node& node);

protected:
    void paintEvent(QPaintEvent *event) override;
    void closeEvent(QCloseEvent *event) override;

public Q_SLOTS:
    /** Show message and progress */
    void showMessage(const QString &message, int alignment, const QColor &color);

    /** Set progress bar value (-1 = no sub-progress, 0-100 = phase sub-progress) */
    void setProgress(int value);

    /** Handle wallet load notifications. */
    void handleLoadWallet();

protected:
    bool eventFilter(QObject * obj, QEvent * ev) override;

private:
    /** Connect core signals to splash screen */
    void subscribeToCoreSignals();
    /** Disconnect core signals to splash screen */
    void unsubscribeFromCoreSignals();
    /** Initiate shutdown */
    void shutdown();
    /** Calculate overall progress (0.0-1.0) based on current phase and sub-progress */
    qreal calcOverallProgress() const;

    QPixmap pixmap;
    /** Cached on GUI thread at construction for thread-safe use in cross-thread callbacks.
     *  Const after construction — no synchronization needed. */
    const QColor messageColor;
    QString curMessage;
    QColor curColor;
    int curAlignment{0};
    int curProgress{-1};

    // Phase-based progress tracking
    qreal phaseStart{0.0};      // Overall progress at start of current phase
    qreal phaseEnd{0.0};        // Overall progress at end of current phase
    bool phaseIsLong{false};    // True for long independent phases (rescan, wallet load)
    QElapsedTimer phaseTimer;    // Time since current phase started
    qreal displayProgress{0.0}; // Smoothly animated display value (0.0-1.0)
    QTimer animTimer;

    interfaces::Node* m_node = nullptr;
    bool m_shutdown = false;
    std::unique_ptr<interfaces::Handler> m_handler_init_message;
    std::unique_ptr<interfaces::Handler> m_handler_show_progress;
    std::unique_ptr<interfaces::Handler> m_handler_init_wallet;
    std::unique_ptr<interfaces::Handler> m_handler_load_wallet;
    std::list<std::unique_ptr<interfaces::Wallet>> m_connected_wallets;
    std::list<std::unique_ptr<interfaces::Handler>> m_connected_wallet_handlers;
};

#endif // BITCOIN_QT_SPLASHSCREEN_H
