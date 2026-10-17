// Copyright (c) 2011-2022 The Bitcoin Core developers
// Copyright (c) 2014-2025 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/sendcoinsentry.h>
#include <qt/forms/ui_sendcoinsentry.h>

#include <base58.h>
#include <qt/addressbookpage.h>
#include <qt/addresstablemodel.h>
#include <qt/bitcoinaddressvalidator.h>
#include <qt/guiutil.h>
#include <qt/optionsmodel.h>
#include <qt/walletmodel.h>

#include <QApplication>
#include <QClipboard>

#include <algorithm>

#ifdef ENABLE_PLATFORM_GUI
namespace {
//! The address validator, which also lets a DashPay username through: a
//! DPNS label has the letters and digits Base58 leaves out, and hyphens.
//! Whether the text is an address is still checked when the entry is sent.
class RecipientEntryValidator : public BitcoinAddressEntryValidator
{
public:
    explicit RecipientEntryValidator(QObject* parent) :
        BitcoinAddressEntryValidator(parent, /*fAllowURI=*/true)
    {
    }

    State validate(QString& input, int& pos) const override
    {
        const State state{BitcoinAddressEntryValidator::validate(input, pos)};
        if (state != Invalid) return state;
        const auto username_char = [](QChar ch) {
            const ushort c{ch.unicode()};
            return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '-';
        };
        return std::all_of(input.begin(), input.end(), username_char) ? Acceptable : Invalid;
    }
};

//! Whether text is what an address looks like, mistyped or of another
//! network included: Base58 of an address's length, which every valid address
//! is. Such text is not sent to an evonode as a username.
bool LooksLikeAddress(const QString& text)
{
    std::vector<unsigned char> data;
    return DecodeBase58(text.toStdString(), data, /*max_ret_len=*/25) && data.size() == 25;
}
} // namespace
#endif

SendCoinsEntry::SendCoinsEntry(QWidget* parent) :
    QWidget(parent),
    ui(new Ui::SendCoinsEntry)
{
    ui->setupUi(this);

    GUIUtil::disableMacFocusRect(this);

    setButtonIcons();

    // normal dash address field
    GUIUtil::setupAddressWidget(ui->payTo, this, true);

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
}

#ifdef ENABLE_PLATFORM_GUI
void SendCoinsEntry::allowUsername()
{
    ui->payTo->setValidator(new RecipientEntryValidator(this));
    // Leaving the field with a username in it announces it, once: a message
    // box opened for the answer takes the focus, and giving it back leaves
    // the field again. Nothing is announced while it is typed, because what
    // is looked up goes to an evonode.
    connect(ui->payTo, &QLineEdit::editingFinished, this, [this] {
        const QString text{ui->payTo->text()};
        if (text.isEmpty() || m_announced || LooksLikeAddress(text)) return;
        m_announced = true;
        Q_EMIT usernameEntered(text);
    });
    connect(ui->payTo, &QLineEdit::textChanged, this, [this] { m_announced = false; });
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
    SendCoinsRecipient rcp;
    if (GUIUtil::parseBitcoinURI(address, &rcp)) {
        ui->payTo->blockSignals(true);
        setValue(rcp);
        ui->payTo->blockSignals(false);
    } else {
        updateLabel(address);
    }
}

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
