// Copyright (c) 2011-2021 The Bitcoin Core developers
// Copyright (c) 2014-2023 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/signverifymessagedialog.h>
#include <qt/forms/ui_signverifymessagedialog.h>

#include <qt/addressbookpage.h>
#include <qt/guiutil.h>
#include <qt/walletmodel.h>

#include <key_io.h>
#include <util/strencodings.h>
#include <util/message.h> // For MessageSign(), MessageVerify()
#include <validation.h>

#include <vector>

#include <QButtonGroup>
#include <QClipboard>

SignVerifyMessageDialog::SignVerifyMessageDialog(QWidget* parent) :
    QDialog(parent, GUIUtil::dialog_flags),
    ui(new Ui::SignVerifyMessageDialog),
    model(nullptr),
    pageButtons(nullptr)
{
    ui->setupUi(this);

    pageButtons = new QButtonGroup(this);
    pageButtons->addButton(ui->btnSignMessage, pageButtons->buttons().size());
    pageButtons->addButton(ui->btnVerifyMessage, pageButtons->buttons().size());
#if (QT_VERSION >= QT_VERSION_CHECK(5, 15, 0))
    connect(pageButtons, &QButtonGroup::idClicked, this, &SignVerifyMessageDialog::showPage);
#else
    connect(pageButtons, QOverload<int>::of(&QButtonGroup::buttonClicked), this, &SignVerifyMessageDialog::showPage);
#endif

    // These icons are needed on Mac also
    GUIUtil::setIcon(ui->addressBookButton_SM, "address-book");
    GUIUtil::setIcon(ui->pasteButton_SM, "editpaste");
    GUIUtil::setIcon(ui->copySignatureButton_SM, "editcopy");
    GUIUtil::setIcon(ui->addressBookButton_VM, "address-book");

    GUIUtil::setupAddressWidget(ui->addressIn_SM, this);
    GUIUtil::setupAddressWidget(ui->addressIn_VM, this);

    ui->addressIn_SM->installEventFilter(this);
    ui->messageIn_SM->installEventFilter(this);
    ui->signatureOut_SM->installEventFilter(this);
    ui->addressIn_VM->installEventFilter(this);
    ui->messageIn_VM->installEventFilter(this);
    ui->signatureIn_VM->installEventFilter(this);

    GUIUtil::setFont({ui->signatureOut_SM, ui->signatureIn_VM}, GUIUtil::FontWeight::Normal, 11, true);
    GUIUtil::setFont({ui->signatureLabel_SM}, GUIUtil::FontWeight::Bold, 16);
    GUIUtil::setFont({ui->statusLabel_SM, ui->statusLabel_VM}, GUIUtil::FontWeight::Bold);

    GUIUtil::updateFonts();

    GUIUtil::disableMacFocusRect(this);

    GUIUtil::handleCloseWindowShortcut(this);
}

SignVerifyMessageDialog::~SignVerifyMessageDialog()
{
    delete pageButtons;
    delete ui;
}

void SignVerifyMessageDialog::setModel(WalletModel *_model)
{
    this->model = _model;
}

void SignVerifyMessageDialog::setAddress_SM(const QString &address)
{
    ui->addressIn_SM->setText(address);
    ui->messageIn_SM->setFocus();
}

void SignVerifyMessageDialog::setAddress_VM(const QString &address)
{
    ui->addressIn_VM->setText(address);
    ui->messageIn_VM->setFocus();
}

void SignVerifyMessageDialog::showTab_SM(bool fShow)
{
    showPage(0);
    if (fShow)
        this->show();
}

void SignVerifyMessageDialog::showTab_VM(bool fShow)
{
    showPage(1);
    if (fShow)
        this->show();
}

void SignVerifyMessageDialog::showPage(int index)
{
    std::vector<QWidget*> vecNormal;
    QAbstractButton* btnActive = pageButtons->button(index);
    for (QAbstractButton* button : pageButtons->buttons()) {
        if (button != btnActive) {
            vecNormal.push_back(button);
        }
    }

    GUIUtil::setFont({btnActive}, GUIUtil::FontWeight::Bold, 16);
    GUIUtil::setFont(vecNormal, GUIUtil::FontWeight::Normal, 16);
    GUIUtil::updateFonts();

    ui->stackedWidgetSig->setCurrentIndex(index);
    btnActive->setChecked(true);
}

void SignVerifyMessageDialog::on_addressBookButton_SM_clicked()
{
    if (model && model->getAddressTableModel())
    {
        AddressBookPage dlg(AddressBookPage::ForSelection, AddressBookPage::ReceivingTab, this);
        dlg.setModel(model->getAddressTableModel());
        if (dlg.exec())
        {
            setAddress_SM(dlg.getReturnValue());
        }
    }
}

void SignVerifyMessageDialog::on_pasteButton_SM_clicked()
{
    setAddress_SM(QApplication::clipboard()->text());
}

void SignVerifyMessageDialog::on_signMessageButton_SM_clicked()
{
    if (!model)
        return;

    /* Clear old signature to ensure users don't get confused on error with an old signature displayed */
    ui->signatureOut_SM->clear();

    CTxDestination destination = DecodeDestination(ui->addressIn_SM->text().toStdString());
    if (!IsValidDestination(destination)) {
        ui->statusLabel_SM->setStyleSheet(GUIUtil::getThemedStyleQString(GUIUtil::ThemedStyle::TS_ERROR));
        ui->statusLabel_SM->setText(tr("The entered address is invalid.") + QString(" ") + tr("Please check the address and try again."));
        return;
    }
    const PKHash* pkhash = std::get_if<PKHash>(&destination);
    if (!pkhash) {
        ui->addressIn_SM->setValid(false);
        ui->statusLabel_SM->setStyleSheet(GUIUtil::getThemedStyleQString(GUIUtil::ThemedStyle::TS_ERROR));
        ui->statusLabel_SM->setText(tr("The entered address does not refer to a key.") + QString(" ") + tr("Please check the address and try again."));
        return;
    }

    WalletModel::UnlockContext ctx(model->requestUnlock());
    if (!ctx.isValid())
    {
        ui->statusLabel_SM->setStyleSheet(GUIUtil::getThemedStyleQString(GUIUtil::ThemedStyle::TS_ERROR));
        ui->statusLabel_SM->setText(tr("Wallet unlock was cancelled."));
        return;
    }

    const std::string& message = ui->messageIn_SM->document()->toPlainText().toStdString();
    std::string signature;
    SigningResult res = model->wallet().signMessage(message, *pkhash, signature);

    QString error;
    switch (res) {
        case SigningResult::OK:
            error = tr("No error");
            break;
        case SigningResult::PRIVATE_KEY_NOT_AVAILABLE:
            error = tr("Private key for the entered address is not available.");
            break;
        case SigningResult::SIGNING_FAILED:
            error = tr("Message signing failed.");
            break;
        // no default case, so the compiler can warn about missing cases
    }

    if (res != SigningResult::OK) {
        ui->statusLabel_SM->setStyleSheet(GUIUtil::getThemedStyleQString(GUIUtil::ThemedStyle::TS_ERROR));
        ui->statusLabel_SM->setText(QString("<nobr>") + error + QString("</nobr>"));
        return;
    }

    ui->statusLabel_SM->setStyleSheet(GUIUtil::getThemedStyleQString(GUIUtil::ThemedStyle::TS_SUCCESS));
    ui->statusLabel_SM->setText(QString("<nobr>") + tr("Message signed.") + QString("</nobr>"));

    ui->signatureOut_SM->setText(QString::fromStdString(signature));
}

void SignVerifyMessageDialog::on_copySignatureButton_SM_clicked()
{
    GUIUtil::setClipboard(ui->signatureOut_SM->text());
}

void SignVerifyMessageDialog::on_clearButton_SM_clicked()
{
    ui->addressIn_SM->clear();
    ui->messageIn_SM->clear();
    ui->signatureOut_SM->clear();
    ui->statusLabel_SM->clear();

    ui->addressIn_SM->setFocus();
}

void SignVerifyMessageDialog::on_addressBookButton_VM_clicked()
{
    if (model && model->getAddressTableModel())
    {
        AddressBookPage dlg(AddressBookPage::ForSelection, AddressBookPage::SendingTab, this);
        dlg.setModel(model->getAddressTableModel());
        if (dlg.exec())
        {
            setAddress_VM(dlg.getReturnValue());
        }
    }
}

void SignVerifyMessageDialog::on_verifyMessageButton_VM_clicked()
{
    const std::string& address = ui->addressIn_VM->text().toStdString();
    const std::string& signature = ui->signatureIn_VM->text().toStdString();
    const std::string& message = ui->messageIn_VM->document()->toPlainText().toStdString();

    const auto result = MessageVerify(address, signature, message);

    if (result == MessageVerificationResult::OK) {
        ui->statusLabel_VM->setStyleSheet(GUIUtil::getThemedStyleQString(GUIUtil::ThemedStyle::TS_SUCCESS));
    } else {
        ui->statusLabel_VM->setStyleSheet(GUIUtil::getThemedStyleQString(GUIUtil::ThemedStyle::TS_ERROR));
    }

    switch (result) {
    case MessageVerificationResult::OK:
        ui->statusLabel_VM->setText(
            QString("<nobr>") + tr("Message verified.") + QString("</nobr>")
        );
        return;
    case MessageVerificationResult::ERR_INVALID_ADDRESS:
        ui->statusLabel_VM->setText(
            tr("The entered address is invalid.") + QString(" ") +
            tr("Please check the address and try again.")
        );
        return;
    case MessageVerificationResult::ERR_ADDRESS_NO_KEY:
        ui->addressIn_VM->setValid(false);
        ui->statusLabel_VM->setText(
            tr("The entered address does not refer to a key.") + QString(" ") +
            tr("Please check the address and try again.")
        );
        return;
    case MessageVerificationResult::ERR_MALFORMED_SIGNATURE:
        ui->signatureIn_VM->setValid(false);
        ui->statusLabel_VM->setText(
            tr("The signature could not be decoded.") + QString(" ") +
            tr("Please check the signature and try again.")
        );
        return;
    case MessageVerificationResult::ERR_PUBKEY_NOT_RECOVERED:
        ui->signatureIn_VM->setValid(false);
        ui->statusLabel_VM->setText(
            tr("The signature did not match the message digest.") + QString(" ") +
            tr("Please check the signature and try again.")
        );
        return;
    case MessageVerificationResult::ERR_NOT_SIGNED:
        ui->statusLabel_VM->setText(
            QString("<nobr>") + tr("Message verification failed.") + QString("</nobr>")
        );
        return;
    }
}

void SignVerifyMessageDialog::on_clearButton_VM_clicked()
{
    ui->addressIn_VM->clear();
    ui->signatureIn_VM->clear();
    ui->messageIn_VM->clear();
    ui->statusLabel_VM->clear();

    ui->addressIn_VM->setFocus();
}

bool SignVerifyMessageDialog::eventFilter(QObject *object, QEvent *event)
{
    if (event->type() == QEvent::MouseButtonPress || event->type() == QEvent::FocusIn)
    {
        if (ui->stackedWidgetSig->currentIndex() == 0) {
            /* Clear status message on focus change */
            ui->statusLabel_SM->clear();

            /* Select generated signature */
            if (object == ui->signatureOut_SM)
            {
                ui->signatureOut_SM->selectAll();
                return true;
            }
        } else if (ui->stackedWidgetSig->currentIndex() == 1) {
            /* Clear status message on focus change */
            ui->statusLabel_VM->clear();
        }
    }
    return QDialog::eventFilter(object, event);
}

void SignVerifyMessageDialog::showEvent(QShowEvent* event)
{
    if (!event->spontaneous()) {
        GUIUtil::updateButtonGroupShortcuts(pageButtons);
    }
    QDialog::showEvent(event);
}
