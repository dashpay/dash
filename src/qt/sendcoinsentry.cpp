// Copyright (c) 2011-2022 The Bitcoin Core developers
// Copyright (c) 2014-2025 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/sendcoinsentry.h>
#include <qt/forms/ui_sendcoinsentry.h>

#include <qt/addressbookpage.h>
#include <qt/addresstablemodel.h>
#include <qt/bitcoinaddressvalidator.h>
#include <qt/guiutil.h>
#include <qt/optionsmodel.h>
#include <qt/walletmodel.h>
#ifdef ENABLE_PLATFORM_GUI
#include <platform/helpers.h>
#include <qt/platform/contactpickerdialog.h>
#include <qt/platform/platformservice.h>
#endif

#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QFocusEvent>
#include <QKeySequence>
#include <QTimer>
#include <QToolButton>

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
#ifdef ENABLE_PLATFORM_GUI
    // No dedicated glyph exists for DashPay contacts; "@" matches how
    // usernames are communicated to the user.
    ui->contactsButton->setText(QStringLiteral("@"));
    GUIUtil::setFont({ui->contactsButton}, GUIUtil::FontWeight::Bold, 15);
    // Alt+C picks a contact for the entry being edited: with a window-wide
    // shortcut, a second recipient would make it ambiguous.
    m_contacts_shortcut = new QAction(this);
    m_contacts_shortcut->setShortcut(QKeySequence{Qt::ALT | Qt::Key_C});
    m_contacts_shortcut->setShortcutContext(Qt::WidgetWithChildrenShortcut);
    m_contacts_shortcut->setEnabled(false);
    addAction(m_contacts_shortcut);
    connect(m_contacts_shortcut, &QAction::triggered, ui->contactsButton, &QToolButton::click);
    ui->payTo->installEventFilter(this);
#endif

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
            m_platform_service->resolvePaymentAddress(m_pending_username);
        }
    });
#endif
}

#ifdef ENABLE_PLATFORM_GUI
namespace {
//! Whether the text could still become a Dash address as more is typed:
//! Base58 only (no 0, O, I, l), which every DPNS label with one of those is
//! not, and no hyphen.
bool CouldBeAddressPrefix(const QString& text)
{
    BitcoinAddressEntryValidator validator{nullptr};
    QString copy{text};
    int pos{0};
    return validator.validate(copy, pos) != QValidator::Invalid;
}
} // namespace

void SendCoinsEntry::showUsernameStatus(UsernameStatus status, const QString& text, const QString& details)
{
    if (m_username_status_action) {
        ui->payTo->removeAction(m_username_status_action);
        delete m_username_status_action;
        m_username_status_action = nullptr;
    }
    m_username_status = status;
    // The line keeps its height while empty, so the form never reflows as
    // the user types.
    ui->payToStatus->setText(text);
    applyUsernameStatusStyle();
    if (status == UsernameStatus::None) return;

    QIcon icon;
    switch (status) {
    case UsernameStatus::None:
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
    m_username_status_action->setToolTip(details.isEmpty() ? text : text + QStringLiteral("\n\n") + details);
}

void SendCoinsEntry::applyUsernameStatusStyle()
{
    GUIUtil::ThemedStyle style{GUIUtil::ThemedStyle::TS_SECONDARY};
    if (m_username_status == UsernameStatus::Verified) style = GUIUtil::ThemedStyle::TS_SUCCESS;
    if (m_username_status == UsernameStatus::Failed) style = GUIUtil::ThemedStyle::TS_ERROR;
    ui->payToStatus->setStyleSheet(GUIUtil::getThemedStyleQString(style));
}

void SendCoinsEntry::on_contactsButton_clicked()
{
    if (!m_platform_service) return;
    ContactPickerDialog dlg(*m_platform_service, this);
    if (dlg.exec() && !dlg.selectedUsername().isEmpty()) {
        ui->payTo->setText(dlg.selectedUsername());
        lookUpUsername();
        ui->payAmount->setFocus();
    }
}

QString SendCoinsEntry::unresolvedUsername()
{
    const QString text{ui->payTo->text()};
    if (!m_platform_service || text.isEmpty() || (model && model->validateAddress(text)) ||
        !platform::helpers::IsValidUsername(text.toStdString())) {
        return {};
    }
    if (m_username_status != UsernameStatus::Failed) lookUpUsername();
    return ui->payToStatus->text();
}

bool SendCoinsEntry::eventFilter(QObject* watched, QEvent* event)
{
    // Not when a popup (a context menu, the completer) takes the focus.
    if (watched == ui->payTo && event->type() == QEvent::FocusOut &&
        static_cast<QFocusEvent*>(event)->reason() != Qt::PopupFocusReason) {
        lookUpUsername();
    }
    return QWidget::eventFilter(watched, event);
}

void SendCoinsEntry::updateContactsButton()
{
    const bool active{m_platform_service && m_platform_service->networkActive()};
    ui->contactsButton->setEnabled(active);
    m_contacts_shortcut->setEnabled(active);
    ui->contactsButton->setToolTip(active ? tr("Choose a DashPay contact to pay (Alt+C)")
                                          : tr("DashPay is paused while network activity is turned off."));
}

void SendCoinsEntry::setPlatformService(PlatformService* service)
{
    if (m_platform_service) disconnect(m_platform_service, nullptr, this, nullptr);
    m_platform_service = service;
    ui->contactsButton->setVisible(service != nullptr);
    updateContactsButton();
    // One line is reserved for the lookup state while DashPay is on; with
    // it off the form is unchanged.
    ui->payToStatus->setVisible(service != nullptr);
    ui->payToStatus->setMinimumHeight(ui->payToStatus->fontMetrics().lineSpacing());
    if (!service) return;
    connect(service, &PlatformService::networkActiveChanged, this, &SendCoinsEntry::updateContactsButton);
    ui->payTo->setToolTip(tr("The Dash address or DashPay username to send the payment to"));
    ui->payTo->setPlaceholderText(tr("Enter a Dash address or DashPay username"));
    connect(service, &PlatformService::paymentAddressResolved, this,
            [this](const QString& username, const QString& label, const QString& address, const QString& error,
                   const QString& details) {
                if (username != m_pending_username || ui->payTo->text() != username) return;
                m_pending_username.clear();
                if (!error.isEmpty()) {
                    showUsernameStatus(UsernameStatus::Failed, error, details);
                    return;
                }
                ui->payTo->blockSignals(true);
                ui->payTo->setText(address);
                ui->payTo->blockSignals(false);
                // Mirror the address-book label the service wrote for this
                // destination so sends and receives from this contact carry
                // the identical label in transaction history. A label the
                // user already typed takes precedence.
                if (ui->addAsLabel->text().isEmpty() && model) {
                    m_auto_label = model->getAddressTableModel()->labelForAddress(address);
                    ui->addAsLabel->setText(m_auto_label);
                }
                // The proved DPNS label, as registered rather than in its
                // homograph-safe form, is what was verified; the derived
                // address is what the payment goes to. Both are shown; a
                // profile display name never is.
                showUsernameStatus(UsernameStatus::Verified,
                                   tr("Paying your DashPay contact “%1” at a fresh address only you two can link.").arg(label),
                                   address);
            });
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
    showUsernameStatus(UsernameStatus::None);
    // A contact's label must not follow the entry to another recipient.
    if (!m_auto_label.isEmpty() && ui->addAsLabel->text() == m_auto_label) ui->addAsLabel->clear();
    m_auto_label.clear();
#endif
    SendCoinsRecipient rcp;
    if (GUIUtil::parseBitcoinURI(address, &rcp)) {
        ui->payTo->blockSignals(true);
        setValue(rcp);
        ui->payTo->blockSignals(false);
    } else {
        updateLabel(address);
#ifdef ENABLE_PLATFORM_GUI
        // A Platform lookup discloses what was typed to an evonode, so only
        // what cannot be the start of a Dash address is looked up while
        // typing; anything else is looked up when the entry is left.
        if (m_platform_service && (!model || !model->validateAddress(address)) &&
            platform::helpers::IsValidUsername(address.toStdString())) {
            if (CouldBeAddressPrefix(address)) {
                showUsernameStatus(UsernameStatus::Progress, tr("Looks like a DashPay username. It is looked up when "
                                                                "you leave the field."));
            } else {
                m_pending_username = address;
                showUsernameStatus(UsernameStatus::Progress, tr("Looking up “%1” on DashPay…").arg(address));
                m_username_debounce->start();
            }
        }
#endif
    }
}

#ifdef ENABLE_PLATFORM_GUI
void SendCoinsEntry::on_payTo_editingFinished() { lookUpUsername(); }

void SendCoinsEntry::lookUpUsername()
{
    const QString address{ui->payTo->text()};
    if (!m_platform_service || address.isEmpty() || (model && model->validateAddress(address)) ||
        !platform::helpers::IsValidUsername(address.toStdString())) {
        return;
    }
    // A lookup of this text is on its way already; one still waiting for
    // the typing pause runs now.
    if (m_pending_username == address && !m_username_debounce->isActive()) return;
    m_username_debounce->stop();
    m_pending_username = address;
    showUsernameStatus(UsernameStatus::Progress, tr("Looking up “%1” on DashPay…").arg(address));
    m_platform_service->resolvePaymentAddress(address);
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
#ifdef ENABLE_PLATFORM_GUI
    m_auto_label.clear();
#endif
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
#ifdef ENABLE_PLATFORM_GUI
    lookUpUsername();
#endif
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
#ifdef ENABLE_PLATFORM_GUI
        applyUsernameStatusStyle();
#endif
    }
}

void SendCoinsEntry::setButtonIcons()
{
    GUIUtil::setIcon(ui->addressBookButton, "address-book");
    GUIUtil::setIcon(ui->pasteButton, "editpaste");
    GUIUtil::setIcon(ui->deleteButton, "remove", GUIUtil::ThemedColor::RED);
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
