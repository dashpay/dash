// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/platform/platformoptindialog.h>

#include <qt/guiutil.h>
#include <qt/platform/platformui.h>

#include <QCheckBox>
#include <QDialogButtonBox>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>

PlatformOptInDialog::PlatformOptInDialog(const QString& wallet_name, QWidget* parent) :
    QDialog(parent, GUIUtil::dialog_flags)
{
    setWindowTitle(tr("Enable DashPay"));
    setMinimumWidth(480);

    auto* layout = new QVBoxLayout(this);
    layout->setSpacing(12);

    auto* title = new QLabel(tr("Enable DashPay for wallet “%1”?").arg(wallet_name), this);
    GUIUtil::setFont({title}, GUIUtil::FontWeight::Bold, 16);
    title->setWordWrap(true);
    layout->addWidget(title);

    auto* disclosure = new QLabel(
        tr("DashPay lets you register a username, add contacts and pay them by username. To do this, the wallet "
           "connects to Dash Platform through evonodes.") +
            QStringLiteral("<br><br>") + tr("While DashPay is on, the evonode that answers each request can see:") +
            QStringLiteral("<ul><li>") + tr("your IP address, or your proxy's if you use one,") +
            QStringLiteral("</li><li>") + tr("your DashPay identity and what it looks up,") + QStringLiteral("</li><li>") +
            tr("the usernames you search for.") + QStringLiteral("</li></ul>") +
            tr("Connections are encrypted and use the same network settings and proxy as the rest of Dash Core. Your "
               "username, profile and contact requests are public on Dash Platform. Private keys never leave this "
               "wallet."),
        this);
    disclosure->setWordWrap(true);
    disclosure->setTextFormat(Qt::RichText);
    layout->addWidget(disclosure);

    m_acknowledge = new QCheckBox(tr("I understand what DashPay shares and want to enable it for this wallet"), this);
    layout->addWidget(m_acknowledge);

    m_buttons = new QDialogButtonBox(this);
    auto* enable = m_buttons->addButton(tr("Enable DashPay"), QDialogButtonBox::AcceptRole);
    auto* cancel = m_buttons->addButton(QDialogButtonBox::Cancel);
    // The only filled button, and never the default: a disclosure is not
    // accepted with Enter.
    PlatformUi::makeSecondary(cancel);
    enable->setAutoDefault(false);
    cancel->setDefault(true);
    enable->setEnabled(false);
    layout->addWidget(m_buttons);
    setTabOrder(m_acknowledge, enable);
    setTabOrder(enable, cancel);
    m_acknowledge->setFocus();

    connect(m_acknowledge, &QCheckBox::toggled, enable, &QPushButton::setEnabled);
    connect(m_buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(m_buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    GUIUtil::updateFonts();
    GUIUtil::disableMacFocusRect(this);
}
