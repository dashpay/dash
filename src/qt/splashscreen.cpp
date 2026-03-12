// Copyright (c) 2011-2021 The Bitcoin Core developers
// Copyright (c) 2014-2025 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#if defined(HAVE_CONFIG_H)
#include <config/bitcoin-config.h>
#endif

#include <qt/splashscreen.h>


#include <chainparams.h>
#include <clientversion.h>
#include <interfaces/handler.h>
#include <interfaces/node.h>
#include <interfaces/wallet.h>
#include <qt/guiutil.h>
#include <qt/guiutil_font.h>
#include <qt/networkstyle.h>
#include <qt/walletmodel.h>
#include <util/system.h>
#include <util/translation.h>

#include <cmath>

#include <QApplication>
#include <QCloseEvent>
#include <QPainter>
#include <QScreen>

// Padding around the card
static constexpr int SPLASH_PADDING = 16;

// Cap for time-based exponential fill: prevents bar from appearing complete
// before the phase actually finishes (bar fills to 95% of range, then waits)
static constexpr qreal EXPONENTIAL_FILL_CAP = 0.95;

/** Phase weight table: maps init message keys to overall progress ranges.
 *  Keys are untranslated strings passed through _() at lookup time so that
 *  phase matching works correctly regardless of the active locale.
 *  Phases without ShowProgress use time-based exponential fill.
 *  Phases with ShowProgress interpolate linearly within their range.
 *  Phases with resetsBar=true reset the bar to 0 and use the full range,
 *  since they can take an arbitrarily long time (e.g. rescanning, wallet loading). */
struct PhaseInfo {
    const char* msg_key;    // untranslated init message (translated at lookup time)
    qreal start;            // overall progress at phase start (0.0-1.0)
    qreal end;              // overall progress at phase end (0.0-1.0)
    bool resetsBar;         // if true, resets the bar to show this phase's own 0-100%
    bool snapsToEnd;        // if true, instantly completes the bar (for "Done loading")
};

static const PhaseInfo PHASE_TABLE[] = {
    {"Loading P2P addresses…",    0.00, 0.02, false, false},
    {"Loading banlist…",          0.02, 0.03, false, false},
    {"Loading block index…",      0.03, 0.75, false, false},
    {"Verifying blocks…",         0.75, 0.88, false, false},
    {"Replaying blocks…",         0.75, 0.88, false, false},
    {"Pruning blockstore…",       0.88, 0.90, false, false},
    {"Starting network threads…", 0.90, 0.94, false, false},
    {"Rescanning…",               0.00, 1.00, true,  false},
    {"Verifying wallet(s)…",      0.00, 0.20, true,  false},
    {"Loading wallet…",           0.20, 1.00, false, false},
    {"Done loading",              0.94, 1.00, false, true},
};

/** Look up phase by translating each key and checking if the message contains it.
 *  Uses contains() because ShowProgress may prepend the wallet name. */
static const PhaseInfo* LookupPhase(const QString& message)
{
    for (const auto& phase : PHASE_TABLE) {
        QString translated = QString::fromStdString(_(phase.msg_key).translated);
        if (message.contains(translated, Qt::CaseInsensitive)) {
            return &phase;
        }
    }
    return nullptr;
}

SplashScreen::SplashScreen(const NetworkStyle* networkStyle)
    : QWidget(),
      messageColor(GUIUtil::getThemedQColor(GUIUtil::ThemedColor::DEFAULT))
{

    // transparent background
    setAttribute(Qt::WA_TranslucentBackground);
    setStyleSheet("background:transparent;");

    // no window decorations
    setWindowFlags(Qt::FramelessWindowHint);

    // Modern splash dimensions — wider and shorter for a contemporary feel
    int width = 440;
    int height = 360;
    int logoSize = 140;
    int cornerRadius = 16;

    float fontFactor            = 1.0;
    float scale = qApp->devicePixelRatio();

    // define text to place
    QString titleText       = PACKAGE_NAME;
    QString versionText = QString::fromStdString(FormatFullVersion()).remove(0, 1);
    const QString& titleAddText = networkStyle->getTitleAddText();

    QFont fontNormal = GUIUtil::getFontNormal();
    QFont fontBold = GUIUtil::getFontBold();

    QPixmap pixmapLogo = networkStyle->getSplashImage();

    // Adjust logo color based on the current theme
    QImage imgLogo = pixmapLogo.toImage().convertToFormat(QImage::Format_ARGB32);
    QColor logoColor = GUIUtil::getThemedQColor(GUIUtil::ThemedColor::BLUE);
    for (int x = 0; x < imgLogo.width(); ++x) {
        for (int y = 0; y < imgLogo.height(); ++y) {
            const QRgb rgb = imgLogo.pixel(x, y);
            imgLogo.setPixel(x, y, qRgba(logoColor.red(), logoColor.green(), logoColor.blue(), qAlpha(rgb)));
        }
    }
    pixmapLogo.convertFromImage(imgLogo);
    pixmapLogo.setDevicePixelRatio(scale);

    int canvasWidth = width + SPLASH_PADDING * 2;
    int canvasHeight = height + SPLASH_PADDING * 2;

    pixmap = QPixmap(canvasWidth * scale, canvasHeight * scale);
    pixmap.setDevicePixelRatio(scale);
    pixmap.fill(Qt::transparent);

    // Scope the painter so it releases the pixmap before timer/signal setup
    {
        QPainter pixPaint(&pixmap);
        pixPaint.setRenderHint(QPainter::Antialiasing, true);
        pixPaint.setRenderHint(QPainter::SmoothPixmapTransform, true);

        QColor bgColor = GUIUtil::getThemedQColor(GUIUtil::ThemedColor::BACKGROUND_WIDGET);

        // Draw rounded background card with a subtle border
        QRectF cardRect(SPLASH_PADDING, SPLASH_PADDING, width, height);
        pixPaint.setPen(QPen(QColor(128, 128, 128, 40), 0.5));
        pixPaint.setBrush(bgColor);
        pixPaint.drawRoundedRect(cardRect, cornerRadius, cornerRadius);

        // Offsets relative to card origin
        int cardX = SPLASH_PADDING;
        int cardY = SPLASH_PADDING;
        int contentTop = cardY + 48;

        // Draw logo centered horizontally, near the top
        int logoX = cardX + (width - logoSize) / 2;
        int logoY = contentTop;
        pixPaint.drawPixmap(logoX, logoY, logoSize, logoSize, pixmapLogo);

        // Title text below logo
        pixPaint.setPen(messageColor);

        fontBold.setPointSize(28 * fontFactor);
        pixPaint.setFont(fontBold);
        QFontMetrics fm = pixPaint.fontMetrics();
        int titleTextWidth = GUIUtil::TextWidth(fm, titleText);
        if (titleTextWidth > width * 0.8) {
            fontFactor = 0.75;
            fontBold.setPointSize(28 * fontFactor);
            pixPaint.setFont(fontBold);
            fm = pixPaint.fontMetrics();
            titleTextWidth = GUIUtil::TextWidth(fm, titleText);
        }
        int titleBaseline = logoY + logoSize + 36 + fm.ascent();
        pixPaint.drawText(cardX + (width - titleTextWidth) / 2, titleBaseline, titleText);

        // Version text — subtle, below title
        QColor subtleColor(messageColor);
        subtleColor.setAlpha(130);
        pixPaint.setPen(subtleColor);
        fontNormal.setPointSize(12 * fontFactor);
        pixPaint.setFont(fontNormal);
        fm = pixPaint.fontMetrics();
        int versionTextWidth = GUIUtil::TextWidth(fm, versionText);
        int versionBaseline = titleBaseline + fm.height() + 4;
        pixPaint.drawText(cardX + (width - versionTextWidth) / 2, versionBaseline, versionText);

        // Draw network badge if special network (testnet, devnet, regtest)
        if(!titleAddText.isEmpty()) {
            fontBold.setPointSize(9 * fontFactor);
            pixPaint.setFont(fontBold);
            fm = pixPaint.fontMetrics();
            int titleAddTextWidth = GUIUtil::TextWidth(fm, titleAddText);
            int badgePadH = 10;
            int badgePadV = 4;
            int badgeW = titleAddTextWidth + badgePadH * 2;
            int badgeH = fm.height() + badgePadV * 2;
            int badgeX = cardX + width - badgeW - 16;
            int badgeY = cardY + 14;
            // Rounded badge
            pixPaint.setPen(Qt::NoPen);
            pixPaint.setBrush(networkStyle->getBadgeColor());
            pixPaint.drawRoundedRect(badgeX, badgeY, badgeW, badgeH, badgeH / 2, badgeH / 2);
            // Badge text
            pixPaint.setPen(QColor(255, 255, 255));
            pixPaint.drawText(badgeX + badgePadH, badgeY + badgePadV + fm.ascent(), titleAddText);
        }
    } // QPainter released here

    // Resize window and move to center of desktop, disallow resizing
    QRect r(QPoint(), QSize(canvasWidth, canvasHeight));
    resize(r.size());
    setFixedSize(r.size());
    move(QGuiApplication::primaryScreen()->geometry().center() - r.center());

    installEventFilter(this);

    // Animation timer: smoothly advance displayProgress toward target and repaint.
    // Timer is started on first message to avoid wasteful repaints before init begins.
    // Note: calcOverallProgress() is safe to call here because animTimer is only
    // started after phaseTimer.start() in showMessage().
    connect(&animTimer, &QTimer::timeout, this, [this]() {
        qreal target = calcOverallProgress();
        // Smoothly animate toward target (never go backwards)
        if (target > displayProgress) {
            qreal gap = target - displayProgress;
            // Faster easing for larger gaps so fast phases don't lag behind
            qreal ease = gap > 0.1 ? 0.3 : 0.15;
            displayProgress += gap * ease;
            // Snap if very close
            if (target - displayProgress < 0.002) displayProgress = target;
        }
        update();
        // Stop timer once progress is complete
        if (displayProgress >= 0.999) animTimer.stop();
    });

    GUIUtil::handleCloseWindowShortcut(this);
}

SplashScreen::~SplashScreen()
{
    if (m_node) unsubscribeFromCoreSignals();
}

void SplashScreen::setNode(interfaces::Node& node)
{
    assert(!m_node);
    m_node = &node;
    subscribeToCoreSignals();
    if (m_shutdown) m_node->startShutdown();
}

void SplashScreen::shutdown()
{
    m_shutdown = true;
    if (m_node) m_node->startShutdown();
}

bool SplashScreen::eventFilter(QObject * obj, QEvent * ev) {
    if (ev->type() == QEvent::KeyPress) {
        QKeyEvent *keyEvent = static_cast<QKeyEvent *>(ev);
        if (keyEvent->key() == Qt::Key_Q) {
            shutdown();
        }
    }
    return QObject::eventFilter(obj, ev);
}

qreal SplashScreen::calcOverallProgress() const
{
    if (curProgress >= 0) {
        // Phase with real sub-progress: interpolate linearly within phase range
        return phaseStart + (phaseEnd - phaseStart) * (curProgress / 100.0);
    }
    // Phase without sub-progress: exponential approach toward phaseEnd
    qreal elapsed = phaseTimer.elapsed() / 1000.0;
    // Long phases (rescan, wallet load) can take minutes/hours — use a very slow curve
    // Normal phases: reaches ~90% of range in ~15s
    // Long phases: reaches ~50% in ~2min, ~75% in ~5min
    qreal tau = phaseIsLong ? 120.0 : 5.0;
    qreal fraction = 1.0 - std::exp(-elapsed / tau);
    return phaseStart + (phaseEnd - phaseStart) * fraction * EXPONENTIAL_FILL_CAP;
}

/** Thread-safe helper: queue a message and/or progress update to the GUI thread.
 *  @param color  Text color, captured by value from the GUI thread at signal
 *                connection time to avoid cross-thread access to member fields. */
static void PostMessageAndProgress(SplashScreen *splash, const QColor &color,
                                   const std::string &message, int progress)
{
    if (!message.empty()) {
        bool invoked = QMetaObject::invokeMethod(splash, "showMessage",
            Qt::QueuedConnection,
            Q_ARG(QString, QString::fromStdString(message)),
            Q_ARG(int, Qt::AlignBottom | Qt::AlignHCenter),
            Q_ARG(QColor, color));
        assert(invoked);
    }
    bool invoked = QMetaObject::invokeMethod(splash, "setProgress",
        Qt::QueuedConnection, Q_ARG(int, progress));
    assert(invoked);
}

void SplashScreen::subscribeToCoreSignals()
{
    // Capture messageColor by value for thread-safe use in non-GUI thread callbacks
    const QColor color = messageColor;
    m_handler_init_message = m_node->handleInitMessage([this, color](const std::string& msg) {
        PostMessageAndProgress(this, color, msg, /*progress=*/-1);
    });
    m_handler_show_progress = m_node->handleShowProgress([this, color](const std::string& title, int nProgress, bool /*resume_possible*/) {
        PostMessageAndProgress(this, color, title, nProgress);
    });
    m_handler_init_wallet = m_node->handleInitWallet([this]() { handleLoadWallet(); });
}

void SplashScreen::handleLoadWallet()
{
#ifdef ENABLE_WALLET
    if (!WalletModel::isWalletEnabled()) return;
    const QColor color = messageColor;
    m_handler_load_wallet = m_node->walletLoader().handleLoadWallet([this, color](std::unique_ptr<interfaces::Wallet> wallet) {
        m_connected_wallet_handlers.emplace_back(wallet->handleShowProgress([this, color](const std::string& title, int nProgress) {
            PostMessageAndProgress(this, color, title, nProgress);
        }));
        m_connected_wallets.emplace_back(std::move(wallet));
    });
#endif
}

void SplashScreen::unsubscribeFromCoreSignals()
{
    // Disconnect signals from client
    m_handler_init_message->disconnect();
    m_handler_show_progress->disconnect();
    m_handler_init_wallet->disconnect();
#ifdef ENABLE_WALLET
    if (m_handler_load_wallet != nullptr) {
        m_handler_load_wallet->disconnect();
    }
#endif // ENABLE_WALLET
    for (const auto& handler : m_connected_wallet_handlers) {
        handler->disconnect();
    }
    m_connected_wallet_handlers.clear();
    m_connected_wallets.clear();
}

void SplashScreen::showMessage(const QString &message, int alignment, const QColor &color)
{
    curMessage = message;
    curAlignment = alignment;
    curColor = color;

    // Start animation timer on first message (avoids wasteful repaints before init begins)
    if (!animTimer.isActive()) {
        phaseTimer.start();
        animTimer.start(30);
    }

    // Look up the phase range for this message; only reinitialize when the
    // phase actually changes so that repeated progress callbacks (e.g. during
    // wallet rescans) don't reset displayProgress and phaseTimer each time.
    // For resetsBar phases, also reinitialize when the message text changes
    // (e.g. different wallet name prefix during multi-wallet rescans).
    const PhaseInfo* phase = LookupPhase(message);
    const bool phase_changed = phase != nullptr &&
        (phase != m_current_phase || (phase->resetsBar && message != m_current_phase_message));
    if (phase_changed) {
        m_current_phase = phase;
        m_current_phase_message = message;
        if (phase->snapsToEnd) {
            // Final phase: snap to 100% immediately since the splash
            // will be destroyed moments after "Done loading" arrives
            displayProgress = 1.0;
            phaseStart = 1.0;
            phaseEnd = 1.0;
            animTimer.stop();
        } else if (phase->resetsBar) {
            // Long independent phases get their own full bar
            displayProgress = 0.0;
            phaseStart = phase->start;
            phaseEnd = phase->end;
            phaseIsLong = true;
            animTimer.start(30);
        } else {
            // Normal phase: ensure we never jump backwards
            phaseIsLong = false;
            phaseStart = std::max(phase->start, displayProgress);
            phaseEnd = std::max(phase->end, phaseStart);
        }
        phaseTimer.restart();
    }

    update();
}

void SplashScreen::setProgress(int value)
{
    curProgress = value;
}

void SplashScreen::paintEvent(QPaintEvent *event)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.drawPixmap(0, 0, pixmap);

    int barMargin = 32;
    int barHeight = 4;
    int barRadius = barHeight / 2;
    int barLeft = SPLASH_PADDING + barMargin;
    int barWidth = width() - SPLASH_PADDING * 2 - barMargin * 2;

    // Draw status message above the progress bar area
    QFont messageFont = GUIUtil::getFontNormal();
    messageFont.setPointSize(12);
    painter.setFont(messageFont);

    QRect messageRect = rect().adjusted(
        SPLASH_PADDING + 24, 0,
        -SPLASH_PADDING - 24,
        -SPLASH_PADDING - 28);
    QColor msgColor(curColor);
    msgColor.setAlpha(160);
    painter.setPen(msgColor);
    painter.drawText(messageRect, curAlignment, curMessage);

    // Draw unified progress bar at the bottom of the card
    int barY = height() - SPLASH_PADDING - 18;

    // Track background (always visible once we have a message)
    if (!curMessage.isEmpty()) {
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(128, 128, 128, 40));
        painter.drawRoundedRect(QRectF(barLeft, barY, barWidth, barHeight), barRadius, barRadius);

        // Fill based on overall progress
        int fillWidth = static_cast<int>(barWidth * std::min(displayProgress, 1.0));
        if (fillWidth > 0) {
            painter.setBrush(GUIUtil::getThemedQColor(GUIUtil::ThemedColor::BLUE));
            painter.drawRoundedRect(QRectF(barLeft, barY, fillWidth, barHeight), barRadius, barRadius);
        }
    }
}

void SplashScreen::closeEvent(QCloseEvent *event)
{
    shutdown(); // allows an "emergency" shutdown during startup
    event->ignore();
}
