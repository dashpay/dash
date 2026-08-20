// Copyright (c) 2011-2021 The Bitcoin Core developers
// Copyright (c) 2014-2025 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#if defined(HAVE_CONFIG_H)
#include <config/bitcoin-config.h>
#endif

#include <qt/sendcoinsentry.h>
#include <qt/forms/ui_sendcoinsentry.h>

#include <qt/addressbookpage.h>
#include <qt/addresstablemodel.h>
#include <qt/bitcoinaddressvalidator.h>
#include <qt/guiutil.h>
#include <qt/optionsmodel.h>
#include <qt/walletmodel.h>
#ifdef ENABLE_PLATFORM_GUI
#include <qt/platform/contactpickerdialog.h>
#include <qt/platform/platformservice.h>
#endif

#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QRegularExpression>
#include <QTimer>

SendCoinsEntry::SendCoinsEntry(QWidget* parent) :
    QWidget(parent),
    ui(new Ui::SendCoinsEntry)
{
    ui->setupUi(this);

    GUIUtil::disableMacFocusRect(this);

    setButtonIcons();

    // normal dash address field
    GUIUtil::setupAddressWidget(ui->payTo, this, true);
#ifdef ENABLE_PLATFORM_GUI
    // Address-only entry validation silently drops DPNS characters that are
    // excluded from Base58 (notably 0, I, O, and lowercase l). Accept username
    // candidates while typing; the unchanged check validator and validate()
    // still require a proof-resolved Dash address before a payment can be sent.
    ui->payTo->setValidator(new DashPayRecipientEntryValidator(this, true));
#endif

    GUIUtil::setFont({ui->payToLabel,
                     ui->labellLabel,
                     ui->amountLabel,
                     ui->messageLabel}, GUIUtil::FontWeight::Normal, 15);

    GUIUtil::updateFonts();

    // Connect signals
    connect(ui->payAmount, &BitcoinAmountField::valueChanged, this, &SendCoinsEntry::payAmountChanged);
    connect(ui->checkboxSubtractFeeFromAmount, &QCheckBox::toggled, this, &SendCoinsEntry::subtractFeeFromAmountChanged);
    connect(ui->deleteButton, &QPushButton::clicked, this, &SendCoinsEntry::deleteClicked);
    connect(ui->useAvailableBalanceButton, &QPushButton::clicked, this, &SendCoinsEntry::useAvailableBalanceClicked);
#ifdef ENABLE_PLATFORM_GUI
    m_username_debounce = new QTimer(this);
    m_username_debounce->setSingleShot(true);
    m_username_debounce->setInterval(500);
    connect(m_username_debounce, &QTimer::timeout, this, [this] {
        if (m_platform_service && !m_pending_username.isEmpty()) {
            showUsernameStatus(tr("Looking up DashPay username \"%1\"…").arg(m_pending_username),
                               UsernameStatus::Progress);
            m_platform_service->resolvePaymentAddress(m_pending_username);
        }
    });
#endif
}

#ifdef ENABLE_PLATFORM_GUI
void SendCoinsEntry::showUsernameStatus(const QString& text, UsernameStatus status)
{
    // Username lookup state lives inside the recipient field as a trailing
    // icon (with the message as its tooltip) so the form never reflows.
    if (m_username_status_action) {
        ui->payTo->removeAction(m_username_status_action);
        delete m_username_status_action;
        m_username_status_action = nullptr;
    }
    if (text.isEmpty()) return;

    QIcon icon;
    switch (status) {
    case UsernameStatus::Progress:
        icon = GUIUtil::getIcon("transaction0", GUIUtil::ThemedColor::ORANGE);
        break;
    case UsernameStatus::Verified:
        icon = GUIUtil::getIcon("synced", GUIUtil::ThemedColor::GREEN);
        break;
    case UsernameStatus::Failed:
        icon = GUIUtil::getIcon("warning", GUIUtil::ThemedColor::RED);
        // Match the field's existing invalid-input treatment.
        ui->payTo->setValid(false);
        break;
    }
    m_username_status_action = ui->payTo->addAction(icon, QLineEdit::TrailingPosition);
    m_username_status_action->setToolTip(text);
}

void SendCoinsEntry::on_contactsButton_clicked()
{
    if (!m_platform_service) return;
    ContactPickerDialog dlg(*m_platform_service, this);
    if (dlg.exec() && !dlg.selectedUsername().isEmpty()) {
        ui->payTo->setText(dlg.selectedUsername());
        ui->payAmount->setFocus();
    }
}
#endif

SendCoinsEntry::~SendCoinsEntry()
{
    delete ui;
}

void SendCoinsEntry::on_pasteButton_clicked()
{
    // Paste text from clipboard into recipient field
    ui->payTo->setText(QApplication::clipboard()->text());
}

void SendCoinsEntry::on_addressBookButton_clicked()
{
    if(!model)
        return;
    AddressBookPage dlg(AddressBookPage::ForSelection, AddressBookPage::SendingTab, this);
    dlg.setModel(model->getAddressTableModel());
    if(dlg.exec())
    {
        ui->payTo->setText(dlg.getReturnValue());
        ui->payAmount->setFocus();
    }
}

void SendCoinsEntry::on_payTo_textChanged(const QString &address)
{
#ifdef ENABLE_PLATFORM_GUI
    m_username_debounce->stop();
    m_pending_username.clear();
    showUsernameStatus({}, UsernameStatus::Progress);
#endif
    SendCoinsRecipient rcp;
    if (GUIUtil::parseBitcoinURI(address, &rcp)) {
        ui->payTo->blockSignals(true);
        setValue(rcp);
        ui->payTo->blockSignals(false);
    } else {
        updateLabel(address);
#ifdef ENABLE_PLATFORM_GUI
        static const QRegularExpression username_re{"^[A-Za-z0-9][A-Za-z0-9-]{1,61}[A-Za-z0-9]$"};
        if (m_platform_service && (!model || !model->validateAddress(address)) &&
            username_re.match(address).hasMatch()) {
            m_pending_username = address;
            showUsernameStatus(tr("Looks like a DashPay username — checking…"), UsernameStatus::Progress);
            m_username_debounce->start();
        }
#endif
    }
}

#ifdef ENABLE_PLATFORM_GUI
void SendCoinsEntry::setPlatformService(PlatformService* service)
{
    if (m_platform_service) disconnect(m_platform_service, nullptr, this, nullptr);
    m_platform_service = service;
    ui->contactsButton->setVisible(service != nullptr);
    if (!service) return;
    ui->payTo->setToolTip(tr("The Dash address or DashPay username to send the payment to"));
    ui->payTo->setPlaceholderText(tr("Enter a Dash address or DashPay username"));
    connect(service, &PlatformService::paymentAddressResolved, this,
            [this](const QString& username, const QString& address, const QString& error) {
        if (username != m_pending_username || ui->payTo->text() != username) return;
        m_pending_username.clear();
        if (!error.isEmpty()) {
            showUsernameStatus(tr("Cannot pay \"%1\": %2").arg(username, error), UsernameStatus::Failed);
            return;
        }
        ui->payTo->blockSignals(true);
        ui->payTo->setText(address);
        ui->payTo->blockSignals(false);
        // Mirror the address-book label the service wrote for this
        // destination so sends and receives from this contact carry the
        // identical label in transaction history. A label the user already
        // typed takes precedence (sendCoins stores whatever is here).
        if (ui->addAsLabel->text().isEmpty() && model) {
            ui->addAsLabel->setText(model->getAddressTableModel()->labelForAddress(address));
        }
        showUsernameStatus(tr("Verified — paying DashPay contact \"%1\"").arg(username),
                           UsernameStatus::Verified);
    });
}
#endif

void SendCoinsEntry::setModel(WalletModel *_model)
{
    this->model = _model;

    if (_model && _model->getOptionsModel())
        connect(_model->getOptionsModel(), &OptionsModel::displayUnitChanged, this, &SendCoinsEntry::updateDisplayUnit);

    clear();
}

void SendCoinsEntry::clear()
{
    // clear UI elements for normal payment
    ui->payTo->clear();
    ui->addAsLabel->clear();
    ui->payAmount->clear();
    if (model && model->getOptionsModel()) {
        ui->checkboxSubtractFeeFromAmount->setChecked(model->getOptionsModel()->getSubFeeFromAmount());
    }
    ui->messageTextLabel->clear();
    ui->messageTextLabel->hide();
    ui->messageLabel->hide();

    // update the display unit, to not use the default ("BTC")
    updateDisplayUnit();
}

void SendCoinsEntry::checkSubtractFeeFromAmount()
{
    ui->checkboxSubtractFeeFromAmount->setChecked(true);
}

void SendCoinsEntry::deleteClicked()
{
    Q_EMIT removeEntry(this);
}

void SendCoinsEntry::useAvailableBalanceClicked()
{
    Q_EMIT useAvailableBalance(this);
}

bool SendCoinsEntry::validate(interfaces::Node& node)
{
    if (!model)
        return false;

    // Check input validity
    bool retval = true;

    if (!model->validateAddress(ui->payTo->text()))
    {
        ui->payTo->setValid(false);
        retval = false;
    }

    if (!ui->payAmount->validate())
    {
        retval = false;
    }

    // Sending a zero amount is invalid
    if (ui->payAmount->value(nullptr) <= 0)
    {
        ui->payAmount->setValid(false);
        retval = false;
    }

    // Reject dust outputs:
    if (retval && GUIUtil::isDust(node, ui->payTo->text(), ui->payAmount->value())) {
        ui->payAmount->setValid(false);
        retval = false;
    }

    return retval;
}

SendCoinsRecipient SendCoinsEntry::getValue()
{
    // Normal payment
    recipient.address = ui->payTo->text();
    recipient.label = ui->addAsLabel->text();
    recipient.amount = ui->payAmount->value();
    recipient.message = ui->messageTextLabel->text();
    recipient.fSubtractFeeFromAmount = (ui->checkboxSubtractFeeFromAmount->checkState() == Qt::Checked);

    return recipient;
}

QWidget *SendCoinsEntry::setupTabChain(QWidget *prev)
{
    QWidget::setTabOrder(prev, ui->payTo);
    QWidget::setTabOrder(ui->payTo, ui->addAsLabel);
    QWidget *w = ui->payAmount->setupTabChain(ui->addAsLabel);
    QWidget::setTabOrder(w, ui->checkboxSubtractFeeFromAmount);
    QWidget::setTabOrder(ui->checkboxSubtractFeeFromAmount, ui->addressBookButton);
    QWidget::setTabOrder(ui->addressBookButton, ui->pasteButton);
    QWidget::setTabOrder(ui->pasteButton, ui->deleteButton);
    return ui->deleteButton;
}

void SendCoinsEntry::setValue(const SendCoinsRecipient &value)
{
    recipient = value;
    {
        // message
        ui->messageTextLabel->setText(recipient.message);
        ui->messageTextLabel->setVisible(!recipient.message.isEmpty());
        ui->messageLabel->setVisible(!recipient.message.isEmpty());

        ui->payTo->setText(recipient.address);
        ui->addAsLabel->setText(recipient.label);
        ui->payAmount->setValue(recipient.amount);
    }

    updateLabel(recipient.address);
}

void SendCoinsEntry::setAddress(const QString &address)
{
    ui->payTo->setText(address);
    ui->payAmount->setFocus();
}

void SendCoinsEntry::setAmount(const CAmount &amount)
{
    ui->payAmount->setValue(amount);
}

bool SendCoinsEntry::isClear()
{
    return ui->payTo->text().isEmpty();
}

void SendCoinsEntry::setFocus()
{
    ui->payTo->setFocus();
}

void SendCoinsEntry::updateDisplayUnit()
{
    if (model && model->getOptionsModel()) {
        ui->payAmount->setDisplayUnit(model->getOptionsModel()->getDisplayUnit());
    }
}

void SendCoinsEntry::changeEvent(QEvent* e)
{
    QWidget::changeEvent(e);
    if (e->type() == QEvent::StyleChange) {
        // Adjust button icon colors on theme changes
        setButtonIcons();
    }
}

void SendCoinsEntry::setButtonIcons()
{
    GUIUtil::setIcon(ui->addressBookButton, "address-book");
    GUIUtil::setIcon(ui->pasteButton, "editpaste");
    GUIUtil::setIcon(ui->deleteButton, "remove", GUIUtil::ThemedColor::RED);
#ifdef ENABLE_PLATFORM_GUI
    // No dedicated glyph exists for DashPay contacts; "@" matches how
    // usernames are communicated to the user.
    QFont at_font{ui->contactsButton->font()};
    at_font.setBold(true);
    at_font.setPointSize(at_font.pointSize() + 2);
    ui->contactsButton->setFont(at_font);
    ui->contactsButton->setText(QStringLiteral("@"));
#endif
}

bool SendCoinsEntry::updateLabel(const QString &address)
{
    if(!model)
        return false;

    // Fill in label from address book, if address has an associated label
    QString associatedLabel = model->getAddressTableModel()->labelForAddress(address);
    if(!associatedLabel.isEmpty())
    {
        ui->addAsLabel->setText(associatedLabel);
        return true;
    }

    return false;
}
