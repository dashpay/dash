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

#include <functional>

#include <QApplication>
#include <QCloseEvent>
#include <QPainter>
#include <QScreen>

// Padding around the card
static constexpr int SPLASH_PADDING = 16;

SplashScreen::SplashScreen(const NetworkStyle* networkStyle)
    : QWidget()
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

    // Scope the painter so it releases the pixmap before signal setup
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

        QColor textColor = GUIUtil::getThemedQColor(GUIUtil::ThemedColor::DEFAULT);

        // Title text below logo
        pixPaint.setPen(textColor);

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
        QColor subtleColor(textColor);
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

static void InitMessage(SplashScreen *splash, const std::string &message)
{
    bool invoked = QMetaObject::invokeMethod(splash, "showMessage",
        Qt::QueuedConnection,
        Q_ARG(QString, QString::fromStdString(message)),
        Q_ARG(int, Qt::AlignBottom | Qt::AlignHCenter),
        Q_ARG(QColor, GUIUtil::getThemedQColor(GUIUtil::ThemedColor::DEFAULT)));
    assert(invoked);
}

static void ShowProgress(SplashScreen *splash, const std::string &title, int nProgress, bool resume_possible)
{
    InitMessage(splash, title + std::string("\n") +
            (resume_possible ? SplashScreen::tr("(press q to shutdown and continue later)").toStdString()
                                : SplashScreen::tr("press q to shutdown").toStdString()) +
            strprintf("\n%d", nProgress) + "%");
}

void SplashScreen::subscribeToCoreSignals()
{
    // Connect signals to client
    m_handler_init_message = m_node->handleInitMessage(std::bind(InitMessage, this, std::placeholders::_1));
    m_handler_show_progress = m_node->handleShowProgress(std::bind(ShowProgress, this, std::placeholders::_1, std::placeholders::_2, std::placeholders::_3));
    m_handler_init_wallet = m_node->handleInitWallet([this]() { handleLoadWallet(); });
}

void SplashScreen::handleLoadWallet()
{
#ifdef ENABLE_WALLET
    if (!WalletModel::isWalletEnabled()) return;
    m_handler_load_wallet = m_node->walletLoader().handleLoadWallet([this](std::unique_ptr<interfaces::Wallet> wallet) {
        m_connected_wallet_handlers.emplace_back(wallet->handleShowProgress(std::bind(ShowProgress, this, std::placeholders::_1, std::placeholders::_2, false)));
        m_connected_wallets.emplace_back(std::move(wallet));
    });
#endif
}

void SplashScreen::unsubscribeFromCoreSignals()
{
    // Disconnect signals from client
    m_handler_init_message->disconnect();
    m_handler_show_progress->disconnect();
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
    update();
}

void SplashScreen::paintEvent(QPaintEvent *event)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.drawPixmap(0, 0, pixmap);

    // Draw status message near the bottom of the card
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
}

void SplashScreen::closeEvent(QCloseEvent *event)
{
    shutdown(); // allows an "emergency" shutdown during startup
    event->ignore();
}
