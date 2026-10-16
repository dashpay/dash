// Copyright (c) 2018-2022 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/test/apptests.h>

#include <chainparams.h>
#include <key.h>
#include <qt/bitcoin.h>
#include <qt/bitcoingui.h>
#include <qt/guiutil.h>
#include <qt/networkstyle.h>
#include <qt/rpcconsole.h>
#include <shutdown.h>
#include <test/util/setup_common.h>
#include <univalue.h>
#include <util/system.h>
#include <validation.h>

#include <QAction>
#include <QLineEdit>
#include <QRegularExpression>
#include <QScopedPointer>
#include <QSettings>
#include <QSignalSpy>
#include <QString>
#include <QTest>
#include <QTextEdit>
#include <QtGlobal>
#include <QtTest/QtTestWidgets>
#include <QtTest/QtTestGui>

#include <cstdint>

namespace {
//! Regex find a string group inside of the console output
QString FindInConsole(const QString& output, const QString& pattern)
{
    const QRegularExpression re(pattern);
    return re.match(output).captured(1);
}

//! Call getblockchaininfo RPC and check first field of JSON output.
void TestRpcCommand(RPCConsole* console)
{
    QTextEdit* messagesWidget = console->findChild<QTextEdit*>("messagesWidget");
    QLineEdit* lineEdit = console->findChild<QLineEdit*>("lineEdit");
    QSignalSpy mw_spy(messagesWidget, &QTextEdit::textChanged);
    QVERIFY(mw_spy.isValid());
    QTest::keyClicks(lineEdit, "getblockchaininfo");
    QTest::keyClick(lineEdit, Qt::Key_Return);
    QVERIFY(mw_spy.wait(1000));
    QCOMPARE(mw_spy.count(), 4);
    const QString output = messagesWidget->toPlainText();
    const QString pattern = QStringLiteral("\"chain\": \"(\\w+)\"");
    QCOMPARE(FindInConsole(output, pattern), QString("regtest"));
}

//! Zoom the main window in, out and back to the default, checking the live and persisted font scale.
void TestZoomShortcuts(BitcoinGUI* window, interfaces::Node& node)
{
    QAction* zoom_in{window->findChild<QAction*>("zoomInAction")};
    QAction* zoom_out{window->findChild<QAction*>("zoomOutAction")};
    QAction* zoom_reset{window->findChild<QAction*>("zoomResetAction")};
    QVERIFY(zoom_in && zoom_out && zoom_reset);
    QVERIFY(zoom_in->isEnabled());
    window->activateWindow();
    QVERIFY(QTest::qWaitForWindowActive(window));

    // Without a wallet the RPC console is embedded and keeps the zoom in/out keys for its own font size.
    const bool console_embedded{!window->findChild<RPCConsole*>()->isWindow()};
    QCOMPARE(zoom_in->shortcuts().isEmpty(), console_embedded);
    QCOMPARE(zoom_out->shortcuts().isEmpty(), console_embedded);

    const auto persisted_scale{[&] { return SettingToInt(node.getPersistentSetting("font-scale")).value_or(INT64_MIN); }};

    const auto zoom{[&](QAction* action, QKeySequence::StandardKey key) {
        if (console_embedded) {
            action->trigger();
        } else {
            QTest::keySequence(window, key);
        }
    }};

    const int initial_scale{GUIUtil::fontScale()};
    zoom(zoom_in, QKeySequence::ZoomIn);
    const int zoomed_scale{GUIUtil::fontScale()};
    QVERIFY(zoomed_scale > initial_scale);
    QCOMPARE(persisted_scale(), int64_t{zoomed_scale});

    zoom(zoom_in, QKeySequence::ZoomIn);
    QVERIFY(GUIUtil::fontScale() > zoomed_scale);
    zoom(zoom_out, QKeySequence::ZoomOut);
    QCOMPARE(GUIUtil::fontScale(), zoomed_scale);
    QCOMPARE(persisted_scale(), int64_t{zoomed_scale});

    QTest::keySequence(window, QKeySequence{"Ctrl+0"});
    QCOMPARE(GUIUtil::fontScale(), GUIUtil::defaultFontScale());
    QCOMPARE(persisted_scale(), int64_t{GUIUtil::defaultFontScale()});

    // With -nosettings there is no settings file, so the zoom applies for the session without being saved.
    node.forceSetting("settings", false);
    zoom(zoom_in, QKeySequence::ZoomIn);
    QVERIFY(GUIUtil::fontScale() > GUIUtil::defaultFontScale());
    QCOMPARE(persisted_scale(), int64_t{GUIUtil::defaultFontScale()});
    QTest::keySequence(window, QKeySequence{"Ctrl+0"});
    QCOMPARE(GUIUtil::fontScale(), GUIUtil::defaultFontScale());
    node.forceSetting("settings", {});

    // OptionTests compares the whole settings.json later on.
    node.updateRwSetting("font-scale", {});
}

//! The zoom keys in the RPC console, separate or embedded, resize only the console text.
void TestConsoleZoomShortcuts(RPCConsole* console)
{
    console->activateWindow();
    QVERIFY(QTest::qWaitForWindowActive(console->window()));
    QLineEdit* line_edit{console->findChild<QLineEdit*>("lineEdit")};
    const auto console_font_size{[] { return QSettings().value("consoleFontSize").toInt(); }};

    // Start from a fixed size: without platform fonts (the "minimal" plugin on Linux) the default size is 0,
    // which the console rejects, and the keys cannot move it from there.
    const int initial_size{12};
    console->setFontSize(initial_size);
    QCOMPARE(console_font_size(), initial_size);

    const int app_scale{GUIUtil::fontScale()};
    // The font size buttons animate their click, so the size changes asynchronously.
    QTest::keySequence(line_edit, QKeySequence{"Ctrl++"});
    QTRY_COMPARE(console_font_size(), initial_size + 1);
    QTest::keySequence(line_edit, QKeySequence{"Ctrl+-"});
    QTRY_COMPARE(console_font_size(), initial_size);
    QCOMPARE(GUIUtil::fontScale(), app_scale);
}
} // namespace

//! Entry point for BitcoinApplication tests.
void AppTests::appTests()
{
#ifdef Q_OS_MACOS
    if (QApplication::platformName() == "minimal") {
        // Disable for mac on "minimal" platform to avoid crashes inside the Qt
        // framework when it tries to look up unimplemented cocoa functions,
        // and fails to handle returned nulls
        // (https://bugreports.qt.io/browse/QTBUG-49686).
        QWARN("Skipping AppTests on mac build with 'minimal' platform set due to Qt bugs. To run AppTests, invoke "
              "with 'QT_QPA_PLATFORM=cocoa test_dash-qt' on mac, or else use a linux or windows build.");
        return;
    }
#endif

    qRegisterMetaType<interfaces::BlockAndHeaderTipInfo>("interfaces::BlockAndHeaderTipInfo");
    m_app.parameterSetup();
    GUIUtil::loadFonts();
    GUIUtil::setApplicationFont();
    QVERIFY(m_app.createOptionsModel(/*resetSettings=*/true));
    QScopedPointer<const NetworkStyle> style(
        NetworkStyle::instantiate(Params().NetworkIDString()));
    m_app.createWindow(style.data());
    connect(&m_app, &BitcoinApplication::windowShown, this, &AppTests::guiTests);
    expectCallback("guiTests");
    QSettings().setValue("fAppearanceSetupDone", true); // skip appearance setup
    m_app.baseInitialize();
    m_app.requestInitialize();
    m_app.exec();
    m_app.requestShutdown();
    m_app.exec();

    // Reset global state to avoid interfering with later tests.
    LogInstance().DisconnectTestLogger();
    AbortShutdown();
}

//! Entry point for BitcoinGUI tests.
void AppTests::guiTests(BitcoinGUI* window)
{
    HandleCallback callback{"guiTests", *this};
    TestZoomShortcuts(window, m_app.node());
    connect(window, &BitcoinGUI::consoleShown, this, &AppTests::consoleTests);
    expectCallback("consoleTests");
    QAction* action = window->findChild<QAction*>("openRPCConsoleAction");
    action->activate(QAction::Trigger);
}

//! Entry point for RPCConsole tests.
void AppTests::consoleTests(RPCConsole* console)
{
    HandleCallback callback{"consoleTests", *this};
    TestRpcCommand(console);
    TestConsoleZoomShortcuts(console);
}

//! Destructor to shut down after the last expected callback completes.
AppTests::HandleCallback::~HandleCallback()
{
    auto& callbacks = m_app_tests.m_callbacks;
    auto it = callbacks.find(m_callback);
    assert(it != callbacks.end());
    callbacks.erase(it);
    if (callbacks.empty()) {
        m_app_tests.m_app.exit(0);
    }
}
