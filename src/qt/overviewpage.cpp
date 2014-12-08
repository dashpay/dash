#include "overviewpage.h"
#include "ui_overviewpage.h"

#include "init.h"
#include "clientmodel.h"
#include "walletmodel.h"
#include "bitcoinunits.h"
#include "darksendconfig.h"
#include "optionsmodel.h"
#include "transactiontablemodel.h"
#include "transactionfilterproxy.h"
#include "guiutil.h"
#include "guiconstants.h"

#include <QAbstractItemDelegate>
#include <QPainter>
#include <QTimer>

#define DECORATION_SIZE 64
#define NUM_ITEMS 3

class TxViewDelegate : public QAbstractItemDelegate
{
    Q_OBJECT
public:
    TxViewDelegate(): QAbstractItemDelegate(), unit(BitcoinUnits::BTC)
    {

    }

    inline void paint(QPainter *painter, const QStyleOptionViewItem &option,
                      const QModelIndex &index ) const
    {
        painter->save();

        QIcon icon = qvariant_cast<QIcon>(index.data(Qt::DecorationRole));
        QRect mainRect = option.rect;
        QRect decorationRect(mainRect.topLeft(), QSize(DECORATION_SIZE, DECORATION_SIZE));
        int xspace = DECORATION_SIZE + 8;
        int ypad = 6;
        int halfheight = (mainRect.height() - 2*ypad)/2;
        QRect amountRect(mainRect.left() + xspace, mainRect.top()+ypad, mainRect.width() - xspace, halfheight);
        QRect addressRect(mainRect.left() + xspace, mainRect.top()+ypad+halfheight, mainRect.width() - xspace, halfheight);
        icon.paint(painter, decorationRect);

        QDateTime date = index.data(TransactionTableModel::DateRole).toDateTime();
        QString address = index.data(Qt::DisplayRole).toString();
        qint64 amount = index.data(TransactionTableModel::AmountRole).toLongLong();
        bool confirmed = index.data(TransactionTableModel::ConfirmedRole).toBool();
        QVariant value = index.data(Qt::ForegroundRole);
        QColor foreground = option.palette.color(QPalette::Text);
        if(value.canConvert<QBrush>())
        {
            QBrush brush = qvariant_cast<QBrush>(value);
            foreground = brush.color();
        }

        painter->setPen(foreground);
        painter->drawText(addressRect, Qt::AlignLeft|Qt::AlignVCenter, address);

        if(amount < 0)
        {
            foreground = COLOR_NEGATIVE;
        }
        else if(!confirmed)
        {
            foreground = COLOR_UNCONFIRMED;
        }
        else
        {
            foreground = option.palette.color(QPalette::Text);
        }
        painter->setPen(foreground);
        QString amountText = BitcoinUnits::formatWithUnit(unit, amount, true);
        if(!confirmed)
        {
            amountText = QString("[") + amountText + QString("]");
        }
        painter->drawText(amountRect, Qt::AlignRight|Qt::AlignVCenter, amountText);

        painter->setPen(option.palette.color(QPalette::Text));
        painter->drawText(amountRect, Qt::AlignLeft|Qt::AlignVCenter, GUIUtil::dateTimeStr(date));

        painter->restore();
    }

    inline QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const
    {
        return QSize(DECORATION_SIZE, DECORATION_SIZE);
    }

    int unit;

};
#include "overviewpage.moc"

OverviewPage::OverviewPage(QWidget *parent) :
    QWidget(parent),
    ui(new Ui::OverviewPage),
    clientModel(0),
    walletModel(0),
    currentBalance(-1),
    currentUnconfirmedBalance(-1),
    currentImmatureBalance(-1),
    txdelegate(new TxViewDelegate()),
    filter(0)
{
    ui->setupUi(this);

    // Recent transactions
    ui->listTransactions->setItemDelegate(txdelegate);
    ui->listTransactions->setIconSize(QSize(DECORATION_SIZE, DECORATION_SIZE));
    ui->listTransactions->setMinimumHeight(NUM_ITEMS * (DECORATION_SIZE + 2));
    ui->listTransactions->setAttribute(Qt::WA_MacShowFocusRect, false);

    connect(ui->listTransactions, SIGNAL(clicked(QModelIndex)), this, SLOT(handleTransactionClicked(QModelIndex)));

    timer = new QTimer(this);
    connect(timer, SIGNAL(timeout()), this, SLOT(darkSendStatus()));
    timer->start(333);

    // init "out of sync" warning labels
    ui->labelWalletStatus->setText("(" + tr("out of sync") + ")");
    ui->labelTransactionsStatus->setText("(" + tr("out of sync") + ")");

    showingDarkSendMessage = 0;
    darksendActionCheck = 0;

    if(fMasterNode){
        ui->toggleDarksend->setText("(Disabled)");
        ui->toggleDarksend->setEnabled(false);
    }else if(!fEnableDarksend){
        ui->toggleDarksend->setText("Start Darksend Mixing");
    } else {
        ui->toggleDarksend->setText("Stop Darksend Mixing");
    }

    // start with displaying the "out of sync" warnings
    showOutOfSyncWarning(true);
}

void OverviewPage::handleTransactionClicked(const QModelIndex &index)
{
    if(filter)
        emit transactionClicked(filter->mapToSource(index));
}

OverviewPage::~OverviewPage()
{
    delete ui;
}

void OverviewPage::setBalance(qint64 balance, qint64 unconfirmedBalance, qint64 immatureBalance, qint64 anonymizedBalance)
{
    int unit = walletModel->getOptionsModel()->getDisplayUnit();
    currentBalance = balance;
    currentUnconfirmedBalance = unconfirmedBalance;
    currentImmatureBalance = immatureBalance;
    currentAnonymizedBalance = anonymizedBalance;
    ui->labelBalance->setText(BitcoinUnits::formatWithUnit(unit, balance));
    ui->labelUnconfirmed->setText(BitcoinUnits::formatWithUnit(unit, unconfirmedBalance));
    ui->labelImmature->setText(BitcoinUnits::formatWithUnit(unit, immatureBalance));
    ui->labelAnonymized->setText(BitcoinUnits::formatWithUnit(unit, anonymizedBalance));

    // only show immature (newly mined) balance if it's non-zero, so as not to complicate things
    // for the non-mining users
    bool showImmature = immatureBalance != 0;
    ui->labelImmature->setVisible(showImmature);
    ui->labelImmatureText->setVisible(showImmature);
}

void OverviewPage::setClientModel(ClientModel *model)
{
    this->clientModel = model;
    if(model)
    {
        // Show warning if this is a prerelease version
        connect(model, SIGNAL(alertsChanged(QString)), this, SLOT(updateAlerts(QString)));
        updateAlerts(model->getStatusBarWarnings());
    }
}

void OverviewPage::setWalletModel(WalletModel *model)
{
    this->walletModel = model;
    if(model && model->getOptionsModel())
    {
        // Set up transaction list
        filter = new TransactionFilterProxy();
        filter->setSourceModel(model->getTransactionTableModel());
        filter->setLimit(NUM_ITEMS);
        filter->setDynamicSortFilter(true);
        filter->setSortRole(Qt::EditRole);
        filter->sort(TransactionTableModel::Status, Qt::DescendingOrder);

        ui->listTransactions->setModel(filter);
        ui->listTransactions->setModelColumn(TransactionTableModel::ToAddress);

        // Keep up to date with wallet
        setBalance(model->getBalance(), model->getUnconfirmedBalance(), model->getImmatureBalance(), model->getAnonymizedBalance());
        connect(model, SIGNAL(balanceChanged(qint64, qint64, qint64, qint64)), this, SLOT(setBalance(qint64, qint64, qint64, qint64)));

        connect(model->getOptionsModel(), SIGNAL(displayUnitChanged(int)), this, SLOT(updateDisplayUnit()));

        connect(ui->runAutoDenom, SIGNAL(clicked()), this, SLOT(runDoAutomaticDenomination()));
        connect(ui->toggleDarksend, SIGNAL(clicked()), this, SLOT(toggleDarksend()));
    }

    // update the display unit, to not use the default ("BTC")
    updateDisplayUnit();
}

void OverviewPage::updateDisplayUnit()
{
    if(walletModel && walletModel->getOptionsModel())
    {
        if(currentBalance != -1)
            setBalance(currentBalance, currentUnconfirmedBalance, currentImmatureBalance, currentAnonymizedBalance);

        // Update txdelegate->unit with the current unit
        txdelegate->unit = walletModel->getOptionsModel()->getDisplayUnit();

        ui->listTransactions->update();
    }
}

void OverviewPage::updateAlerts(const QString &warnings)
{
    this->ui->labelAlerts->setVisible(!warnings.isEmpty());
    this->ui->labelAlerts->setText(warnings);
}

void OverviewPage::showOutOfSyncWarning(bool fShow)
{
    ui->labelWalletStatus->setVisible(fShow);
    ui->labelTransactionsStatus->setVisible(fShow);
}

void OverviewPage::updateDarksendProgress(){
    int64 balance = pwalletMain->GetBalance();
    if(balance == 0){
        ui->darksendProgress->setValue(0);
        QString s("No inputs detected");
        ui->darksendProgress->setToolTip(s);        
        return;
    }

    std::ostringstream convert;

    // Get the anon threshold
    int64 max = nAnonymizeDarkcoinAmount*COIN;

    // If it's more than the wallet amount, limit to that.
    if(max > balance) max = balance;

    if(max == 0) return;

    // calculate progress
    int progress = 100 * pwalletMain->GetNormalizedAnonymizedBalance() / max;

    if(progress > 100) progress = 100;

    ui->darksendProgress->setValue(progress);
    convert << "Inputs have an average of " << pwalletMain->GetAverageAnonymizedRounds() << " of " << nDarksendRounds << " rounds";
    QString s(convert.str().c_str());
    ui->darksendProgress->setToolTip(s);
}


void OverviewPage::darkSendStatus()
{
    if(nBestHeight != darkSendPool.cachedNumBlocks)
    {
        updateDarksendProgress();

        std::ostringstream convert2;
        convert2 << nAnonymizeDarkcoinAmount << " DRK / " << nDarksendRounds << " Rounds";
        QString s2(convert2.str().c_str());
        ui->labelAmountRounds->setText(s2);
    }

    if(!fEnableDarksend) {
        if(nBestHeight != darkSendPool.cachedNumBlocks)
        {
            darkSendPool.cachedNumBlocks = nBestHeight;

            ui->darksendEnabled->setText("Disabled");
            ui->darksendStatus->setText("");
            ui->toggleDarksend->setText("Start Darksend Mixing");
        }

        return;
    }

    // check darksend status and unlock if needed
    if(nBestHeight != darkSendPool.cachedNumBlocks)
    {
        // Balance and number of transactions might have changed
        darkSendPool.cachedNumBlocks = nBestHeight;

        if (pwalletMain->GetBalance() - pwalletMain->GetAnonymizedBalance() > 2*COIN){
            if (walletModel->getEncryptionStatus() != WalletModel::Unencrypted){
                if((nAnonymizeDarkcoinAmount*COIN)-pwalletMain->GetAnonymizedBalance() > 1.1*COIN &&
                    walletModel->getEncryptionStatus() == WalletModel::Locked){

                    WalletModel::UnlockContext ctx(walletModel->requestUnlock(false));
                    if(!ctx.isValid()){
                        //unlock was cancelled
                        fEnableDarksend = false;
                        darkSendPool.cachedNumBlocks = 0;
                        LogPrintf("Wallet is locked and user declined to unlock. Disabling Darksend.\n");
                    }
                }
                if((nAnonymizeDarkcoinAmount*COIN)-pwalletMain->GetAnonymizedBalance() <= 1.1*COIN && 
                    walletModel->getEncryptionStatus() == WalletModel::Unlocked &&
                    darkSendPool.GetMyTransactionCount() == 0){

                    LogPrintf("Darksend is complete, locking wallet.\n");
                    walletModel->Lock();
                }
            }
        }

        /* *******************************************************/

        ui->darksendEnabled->setText("Enabled");
    }

    int state = darkSendPool.GetState();
    int entries = darkSendPool.GetEntriesCount();
    int accepted = darkSendPool.GetLastEntryAccepted();

    std::ostringstream convert;

    if(state == POOL_STATUS_ACCEPTING_ENTRIES) {
        if(entries == 0) {
            convert << "Darksend is idle";
            showingDarkSendMessage = 0;
        } else if (accepted == 1) {
            convert << "Darksend request complete: Your transaction was accepted into the pool!";
            if(showingDarkSendMessage % 10 > 8) {
                darkSendPool.lastEntryAccepted = 0;
                showingDarkSendMessage = 0;
            }
        } else {
            if(showingDarkSendMessage % 70 <= 40) convert << "Submitted to masternode, entries " << entries << "/" << darkSendPool.GetMaxPoolTransactions();
            else if(showingDarkSendMessage % 70 <= 50) convert << "Submitted to masternode, Waiting for more entries (" << entries << "/" << darkSendPool.GetMaxPoolTransactions() << " ) .";
            else if(showingDarkSendMessage % 70 <= 60) convert << "Submitted to masternode, Waiting for more entries (" << entries << "/" << darkSendPool.GetMaxPoolTransactions() << " ) ..";
            else if(showingDarkSendMessage % 70 <= 70) convert << "Submitted to masternode, Waiting for more entries (" << entries << "/" << darkSendPool.GetMaxPoolTransactions() << " ) ...";
        }
    } else if(state == POOL_STATUS_SIGNING) {
        if(showingDarkSendMessage % 70 <= 10) convert << "Found enough users, signing";
        else if(showingDarkSendMessage % 70 <= 20) convert << "Found enough users, signing ( waiting. )";
        else if(showingDarkSendMessage % 70 <= 30) convert << "Found enough users, signing ( waiting.. )";
        else if(showingDarkSendMessage % 70 <= 40) convert << "Found enough users, signing ( waiting... )";
    } else if(state == POOL_STATUS_TRANSMISSION) {
        convert << "Transmitting final transaction";
    } else if (state == POOL_STATUS_IDLE) {
        convert << "Darksend is idle";
    } else if (state == POOL_STATUS_FINALIZE_TRANSACTION) {
        convert << "Finalizing transaction";
    } else if(state == POOL_STATUS_ERROR) {
        convert << "Darksend request incomplete: " << darkSendPool.lastMessage << ". Will retry...";
    } else if(state == POOL_STATUS_SUCCESS) {
        convert << "Darksend request complete: " << darkSendPool.lastMessage;
    } else if(state == POOL_STATUS_QUEUE) {
        if(showingDarkSendMessage % 70 <= 50) convert << "Submitted to masternode, waiting in queue .";
        else if(showingDarkSendMessage % 70 <= 60) convert << "Submitted to masternode, waiting in queue ..";
        else if(showingDarkSendMessage % 70 <= 70) convert << "Submitted to masternode, waiting in queue ...";
    } else {
        convert << "unknown state : id=" << state;
    }

    if(state == POOL_STATUS_ERROR || state == POOL_STATUS_SUCCESS) darkSendPool.Check();    

    QString s(convert.str().c_str());

    if(s != ui->darksendStatus->text())
        LogPrintf("%s\n", convert.str().c_str());
    
    ui->darksendStatus->setText(s);

    showingDarkSendMessage++;
    darksendActionCheck++;

    // Get DarkSend Denomination Status
}

void OverviewPage::runDoAutomaticDenomination(){
    darkSendPool.DoAutomaticDenominating();
}

void OverviewPage::toggleDarksend(){
    int64 balance = pwalletMain->GetBalance();
    if(balance < 2.5*COIN){
        QMessageBox::warning(this, tr("Darksend"),
            tr("Darksend requires at least 2.5 DRK to use."),
            QMessageBox::Ok, QMessageBox::Ok);
        return;
    }


    darkSendPool.cachedNumBlocks = 0;

    fEnableDarksend = !fEnableDarksend;

    if(!fEnableDarksend){
        ui->toggleDarksend->setText("Start Darksend Mixing");
    } else {
        ui->toggleDarksend->setText("Stop Darksend Mixing");

        /* show darksend configuration if client has defaults set */

        if(nAnonymizeDarkcoinAmount == 0){
            DarksendConfig dlg(this);
            dlg.setModel(walletModel);
            dlg.exec();
        }

        darkSendPool.DoAutomaticDenominating();
    }
}
