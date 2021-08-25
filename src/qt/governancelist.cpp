#include <qt/governancelist.h>
#include <qt/forms/ui_governancelist.h>

#include <chainparams.h>
#include <evo/deterministicmns.h>
#include <qt/clientmodel.h>
#include <clientversion.h>
#include <coins.h>
#include <qt/guiutil.h>
#include <netbase.h>
#include <qt/walletmodel.h>
#include <governance/governance-object.h>

#include <univalue.h>

#include <QMessageBox>
#include <QTableWidgetItem>
#include <QtGui/QClipboard>


template <typename T>
class CGovernanceListWidgetItem : public QTableWidgetItem
{
    T itemData;

public:
    explicit CGovernanceListWidgetItem(const QString& text, const T& data, int type = Type) :
        QTableWidgetItem(text, type),
        itemData(data) {}

    bool operator<(const QTableWidgetItem& other) const
    {
        return itemData < ((CGovernanceListWidgetItem*)&other)->itemData;
    }
};

GovernanceList::GovernanceList(QWidget* parent) :
    QWidget(parent),
    ui(new Ui::GovernanceList),
    clientModel(0),
    fGovernanceFilterUpdated(true),
    nTimeGovernanceFilterUpdated(0),
    nTimeGovernanceUpdated(0),
    governanceListChanged(true)
{
    ui->setupUi(this);

    GUIUtil::setFont({ui->label_count_2, ui->countLabel}, GUIUtil::FontWeight::Bold, 14);
    GUIUtil::setFont({ui->label_filter_2}, GUIUtil::FontWeight::Normal, 15);

    int columnHashWidth = 80;
    int columnTitleWidth = 220;
    int columnCreationWidth = 110;
    int columnStartWidth = 110;
    int columnEndWidth = 110;
    int columnAmountWidth = 80;
    int columnActiveWidth = 50;
    int columnStatusWidth = 220;

    ui->tableWidgetGovernances->setColumnWidth(COLUMN_HASH, columnHashWidth);
    ui->tableWidgetGovernances->setColumnWidth(COLUMN_TITLE, columnTitleWidth);
    ui->tableWidgetGovernances->setColumnWidth(COLUMN_CREATION, columnCreationWidth);
    ui->tableWidgetGovernances->setColumnWidth(COLUMN_START, columnStartWidth);
    ui->tableWidgetGovernances->setColumnWidth(COLUMN_END, columnEndWidth);
    ui->tableWidgetGovernances->setColumnWidth(COLUMN_AMOUNT, columnAmountWidth);
    ui->tableWidgetGovernances->setColumnWidth(COLUMN_ACTIVE, columnActiveWidth);
    ui->tableWidgetGovernances->setColumnWidth(COLUMN_STATUS, columnStatusWidth);

    ui->tableWidgetGovernances->setContextMenuPolicy(Qt::CustomContextMenu);
    ui->tableWidgetGovernances->sortItems(COLUMN_CREATION, Qt::DescendingOrder);

    ui->filterLineEdit->setPlaceholderText(tr("Filter by Title"));

    timer = new QTimer(this);
    connect(timer, SIGNAL(timeout()), this, SLOT(updateGovernanceListScheduled()));
    timer->start(1000);

    GUIUtil::updateFonts();
}

GovernanceList::~GovernanceList()
{
    delete ui;
}

void GovernanceList::setClientModel(ClientModel* model)
{
    this->clientModel = model;
}

void GovernanceList::handleGovernanceListChanged()
{
    LOCK(cs_proposalList);
    governanceListChanged = true;
}

void GovernanceList::updateGovernanceListScheduled()
{
    if (timer->remainingTime() < 10 *  GOVERNANCELIST_UPDATE_SECONDS) {
        timer->start(1000 * GOVERNANCELIST_UPDATE_SECONDS);
    }

    TRY_LOCK(cs_proposalList, fLockAcquired);
    if (!fLockAcquired) return;

    if (!clientModel || clientModel->node().shutdownRequested()) {
        return;
    }

    // To prevent high cpu usage update only once in GOVERNANCELIST_FILTER_COOLDOWN_SECONDS seconds
    // after filter was last changed unless we want to force the update.
    int64_t nSecondsToWait = GOVERNANCELIST_UPDATE_SECONDS;
    if (fGovernanceFilterUpdated) {
        nSecondsToWait = nTimeGovernanceFilterUpdated - GetTime() + GOVERNANCELIST_FILTER_COOLDOWN_SECONDS;
        ui->countLabel->setText(tr("Please wait...") + " " + QString::number(nSecondsToWait));

    } else if (governanceListChanged) {
        int64_t nUpdateSecods = clientModel->masternodeSync().isBlockchainSynced() ? GOVERNANCELIST_UPDATE_SECONDS : GOVERNANCELIST_UPDATE_SECONDS * 10;
        nSecondsToWait = nTimeGovernanceUpdated - GetTime() + nUpdateSecods;
    }

    if (nSecondsToWait <= 0) {
        updateGovernanceList();
        fGovernanceFilterUpdated = false;
    }

}

void GovernanceList::updateGovernanceList()
{
    if (!clientModel || clientModel->node().shutdownRequested()) {
        return;
    }

    std::vector<const CGovernanceObject*> govObjList = clientModel->getAllGovernanceObjects();
    LOCK(cs_proposalList);

    ui->countLabel->setText(tr("Updating..."));
    ui->tableWidgetGovernances->setSortingEnabled(false);
    ui->tableWidgetGovernances->clearContents();
    ui->tableWidgetGovernances->setRowCount(0);

    // A propsal is considered passing if (YES votes - NO votes) > (Total Number of Masternodes / 10),
    // count total valid (ENABLED) masternodes to determine passing threshold.
    int nMnCount = clientModel->getMasternodeList().GetValidMNsCount();
    int nAbsVoteReq = std::max(Params().GetConsensus().nGovernanceMinQuorum, nMnCount / 10);

    // TODO: implement filtering...

    nTimeGovernanceUpdated = GetTime();
    for (const auto pGovObj: govObjList) {

        auto govObjType = pGovObj->GetObjectType();
        if (govObjType != GOVERNANCE_OBJECT_PROPOSAL) {
            continue; // Skip triggers.
        }

        QString qHashString = QString::fromStdString(pGovObj->GetHash().ToString());
        QTableWidgetItem* hashItem = new QTableWidgetItem(qHashString);

        QString qTitleString = QString::fromStdString("UNKNOWN");
        QString qPaymentStartString = QString::fromStdString("UNKNOWN");
        QString qPaymentEndString = QString::fromStdString("UNKNOWN");
        QString qAmountString = QString::fromStdString("UNKNOWN");
        QString qUrlString = QString::fromStdString("UNKNOWN");

        UniValue prop_data;
        if (prop_data.read(pGovObj->GetDataAsPlainString())) {
            UniValue titleValue = find_value(prop_data, "name");
            if (titleValue.isStr()) qTitleString = QString::fromStdString(titleValue.get_str());

            UniValue paymentStartValue = find_value(prop_data, "start_epoch");
            if (paymentStartValue.isNum()) qPaymentStartString = QDateTime::fromSecsSinceEpoch(paymentStartValue.get_int()).toString(GOVERNANCELIST_DATEFMT);

            UniValue paymentEndValue = find_value(prop_data, "end_epoch");
            if (paymentEndValue.isNum()) qPaymentEndString = QDateTime::fromSecsSinceEpoch(paymentEndValue.get_int()).toString(GOVERNANCELIST_DATEFMT);

            UniValue amountValue = find_value(prop_data, "payment_amount");
            if (amountValue.isNum()) qAmountString = QString::fromStdString(std::to_string(amountValue.get_int()));

            UniValue urlValue = find_value(prop_data, "url");
            if (urlValue.isStr()) qUrlString = QString::fromStdString(urlValue.get_str());
        }

        QTableWidgetItem* titleItem = new QTableWidgetItem(qTitleString);
        QTableWidgetItem* paymentStartItem = new QTableWidgetItem(qPaymentStartString);
        QTableWidgetItem* paymentEndItem = new QTableWidgetItem(qPaymentEndString);
        QTableWidgetItem* amountItem = new QTableWidgetItem(qAmountString);

        QString qCreationString = QDateTime::fromSecsSinceEpoch(pGovObj->GetCreationTime()).toString(GOVERNANCELIST_DATEFMT);
        QTableWidgetItem* creationItem = new QTableWidgetItem(qCreationString);

        QString qActiveString = QString::fromStdString("-");
        std::string strError = "";
        if (pGovObj->IsValidLocally(strError, false)) {
            qActiveString = "Y";
        } else {
            qActiveString = "N";
        }
        QTableWidgetItem* activeItem = new QTableWidgetItem(qActiveString);

        // Voting status...
        // TODO: determine if voting is in progress vs. funded or not funded for past proposals.
        // see CSuperblock::GetNearestSuperblocksHeights(nBlockHeight, nLastSuperblock, nNextSuperblock); 
        int absYesCount = pGovObj->GetAbsoluteYesCount(VOTE_SIGNAL_FUNDING);
        QString qStatusString;
        if (absYesCount > nAbsVoteReq) {
            // Could use pGovObj->IsSetCachedFunding here, but need nAbsVoteReq to display numbers anyway.
            qStatusString = QString("Passing +%1 (%2 of %3 needed)").arg(absYesCount - nAbsVoteReq).arg(absYesCount).arg(nAbsVoteReq);
        } else {
            qStatusString = QString("Needs additional %1 votes").arg(nAbsVoteReq - absYesCount);
        }
        QTableWidgetItem* statusItem = new QTableWidgetItem(qStatusString);

        ui->tableWidgetGovernances->insertRow(0);
        ui->tableWidgetGovernances->setItem(0, COLUMN_HASH, hashItem);
        ui->tableWidgetGovernances->setItem(0, COLUMN_TITLE, titleItem);
        ui->tableWidgetGovernances->setItem(0, COLUMN_CREATION, creationItem);
        ui->tableWidgetGovernances->setItem(0, COLUMN_START, paymentStartItem);
        ui->tableWidgetGovernances->setItem(0, COLUMN_END, paymentEndItem);
        ui->tableWidgetGovernances->setItem(0, COLUMN_AMOUNT, amountItem);
        ui->tableWidgetGovernances->setItem(0, COLUMN_ACTIVE, activeItem);
        ui->tableWidgetGovernances->setItem(0, COLUMN_STATUS, statusItem);


    }
    ui->countLabel->setText(QString::number(ui->tableWidgetGovernances->rowCount()));
    ui->tableWidgetGovernances->setSortingEnabled(true);

}

void GovernanceList::on_filterLineEdit_textChanged(const QString& strFilterIn)
{
    strCurrentFilter = strFilterIn;
    nTimeGovernanceFilterUpdated = GetTime();
    fGovernanceFilterUpdated = true;
    ui->countLabel->setText(tr("Please wait...") + " " + QString::number(GOVERNANCELIST_FILTER_COOLDOWN_SECONDS));
}


