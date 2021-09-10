#include <qt/forms/ui_governancelist.h>
#include <qt/governancelist.h>

#include <clientversion.h>
#include <coins.h>
#include <qt/guiutil.h>
#include <netbase.h>
#include <qt/clientmodel.h>
#include <qt/walletmodel.h>

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
    walletModel(0),
    fFilterUpdatedDIP3(true),
    nTimeFilterUpdatedDIP3(0),
    nTimeUpdatedDIP3(0),
    mnListChanged(true)
{
    ui->setupUi(this);
    GUIUtil::setFont({ui->label_filter_2}, GUIUtil::FontWeight::Normal, 15);

    //hiding for now, will be use in future
    ui->label_filter_2->hide();
    ui->filterLineEditDIP3->hide();

    int columnStatusWidth = 80;
    int columnAmountWidth = 80;
    int columnCyclesWidth = 130;
    int columnCurrentCycleWidth = 100;
    int columnAbsYesCountWidth = 130;
    int columnNoCountWidth = 80;
    int columnAbsteinCountWidth = 130;
    int columnPaymentStartWidth = 160;
    int columnPaymentEndWidth = 160;
    int columnCreationTime = 130;


    ui->tableWidgetProposalsDIP3->setColumnWidth(COLUMN_STATUS, columnStatusWidth);
    ui->tableWidgetProposalsDIP3->setColumnWidth(COLUMN_AMOUNT, columnAmountWidth);
    ui->tableWidgetProposalsDIP3->setColumnWidth(COLUMN_CYCLES, columnCyclesWidth);
    ui->tableWidgetProposalsDIP3->setColumnWidth(COLUMN_CURRENT_CYCLE, columnCurrentCycleWidth);
    ui->tableWidgetProposalsDIP3->setColumnWidth(COLUMN_ABS_YES_COUNT, columnAbsYesCountWidth);
    ui->tableWidgetProposalsDIP3->setColumnWidth(COLUMN_YES_COUNT, columnAbsYesCountWidth);
    ui->tableWidgetProposalsDIP3->setColumnWidth(COLUMN_NO_COUNT, columnNoCountWidth);
    ui->tableWidgetProposalsDIP3->setColumnWidth(COLUMN_ABSTAIN_COUNT, columnAbsteinCountWidth);
    ui->tableWidgetProposalsDIP3->setColumnWidth(COLUMN_PAYMENT_START, columnPaymentStartWidth);
    ui->tableWidgetProposalsDIP3->setColumnWidth(COLUMN_PAYMENT_END, columnPaymentEndWidth);
    ui->tableWidgetProposalsDIP3->setColumnWidth(COLUMN_CREATION_TIME, columnCreationTime);


    ui->tableWidgetProposalsDIP3->setContextMenuPolicy(Qt::CustomContextMenu);

    ui->filterLineEditDIP3->setPlaceholderText(tr("Filter by any property (e.g. address or protx hash)"));

    QAction* openProLinkAction = new QAction(tr("Open Proposal Link"), this);
    contextMenuDIP3 = new QMenu(this);
    contextMenuDIP3->addAction(openProLinkAction);
    connect(ui->tableWidgetProposalsDIP3, &QTableWidget::customContextMenuRequested, this, &GovernanceList::showContextMenuDIP3);
    connect(ui->tableWidgetProposalsDIP3, &QTableWidget::doubleClicked, this, &GovernanceList::extraInfoDIP3_clicked);
    connect(ui->tableWidgetProposalsDIP3, &QTableWidget::clicked, this, &GovernanceList::tableWidgetRow_clicked);
    connect(openProLinkAction, &QAction::triggered, this, &GovernanceList::openProLink_clicked);

    timer = new QTimer(this);
    connect(timer, &QTimer::timeout, this, &GovernanceList::updateDIP3ListScheduled);
    timer->start(10000);

    GUIUtil::updateFonts();
}

GovernanceList::~GovernanceList()
{
    delete ui;
}

void GovernanceList::setClientModel(ClientModel* model)
{
    this->clientModel = model;
    if (model) {
        // try to update list when masternode count changes
        connect(clientModel, &ClientModel::governanceListChanged, this, &GovernanceList::handleGovernanceListChanged);
    }
}

void GovernanceList::setWalletModel(WalletModel* model)
{
    this->walletModel = model;
}

void GovernanceList::showContextMenuDIP3(const QPoint& point)
{
    QTableWidgetItem* item = ui->tableWidgetProposalsDIP3->itemAt(point);
    if (item) contextMenuDIP3->exec(QCursor::pos());
}

void GovernanceList::handleGovernanceListChanged()
{
    LOCK(cs_dip3list);
    mnListChanged = true;
}


//update schedule is same as masternodes' update schedule. May be change if needed any governanceList specified change
void GovernanceList::updateDIP3ListScheduled()
{
    TRY_LOCK(cs_dip3list, fLockAcquired);
    if (!fLockAcquired) return;

    if (!clientModel || clientModel->node().shutdownRequested()) {
        return;
    }

    // To prevent high cpu usage update only once in GOVERNANCELIST_FILTER_COOLDOWN_SECONDS seconds
    // after filter was last changed unless we want to force the update.
    if (fFilterUpdatedDIP3) {
        int64_t nSecondsToWait = nTimeFilterUpdatedDIP3 - GetTime() + GOVERNANCELIST_FILTER_COOLDOWN_SECONDS;

        if (nSecondsToWait <= 0) {
            updateDIP3List();
            fFilterUpdatedDIP3 = false;
        }
    } else if (mnListChanged) {
        int64_t nMnListUpdateSecods = clientModel->masternodeSync().isBlockchainSynced() ? GOVERNANCELIST_UPDATE_SECONDS : GOVERNANCELIST_UPDATE_SECONDS * 10;
        int64_t nSecondsToWait = nTimeUpdatedDIP3 - GetTime() + nMnListUpdateSecods;

        if (nSecondsToWait <= 0) {
            updateDIP3List();
            mnListChanged = false;
        }
    }
}


void GovernanceList::updateDIP3List()
{
    if (!clientModel || clientModel->node().shutdownRequested()) {
        return;
    }

    std::vector<const CGovernanceObject*> governanceList = clientModel->getGovernanceList();

    LOCK(cs_dip3list);

    QString strToFilter;
    ui->tableWidgetProposalsDIP3->setSortingEnabled(false);

    //deletes prev. items in the table
    for (int i = 0; i < ui->tableWidgetProposalsDIP3->rowCount(); i++) {
        QTableWidgetItem* status_item = ui->tableWidgetProposalsDIP3->item(i, COLUMN_STATUS);
        if (status_item != nullptr) delete status_item;

        QTableWidgetItem* amount_item = ui->tableWidgetProposalsDIP3->item(i, COLUMN_AMOUNT);
        if (status_item != nullptr) delete amount_item;

        QTableWidgetItem* cycles_item = ui->tableWidgetProposalsDIP3->item(i, COLUMN_CYCLES);
        if (status_item != nullptr) delete cycles_item;

        QTableWidgetItem* curr_cycles_item = ui->tableWidgetProposalsDIP3->item(i, COLUMN_CURRENT_CYCLE);
        if (status_item != nullptr) delete curr_cycles_item;

        QTableWidgetItem* abs_yes_item = ui->tableWidgetProposalsDIP3->item(i, COLUMN_ABS_YES_COUNT);
        if (status_item != nullptr) delete abs_yes_item;

        QTableWidgetItem* yes_item = ui->tableWidgetProposalsDIP3->item(i, COLUMN_YES_COUNT);
        if (status_item != nullptr) delete yes_item;

        QTableWidgetItem* no_item = ui->tableWidgetProposalsDIP3->item(i, COLUMN_NO_COUNT);
        if (status_item != nullptr) delete no_item;

        QTableWidgetItem* abstain_yes_item = ui->tableWidgetProposalsDIP3->item(i, COLUMN_ABSTAIN_COUNT);
        if (status_item != nullptr) delete abstain_yes_item;

        QTableWidgetItem* payment_start_item = ui->tableWidgetProposalsDIP3->item(i, COLUMN_PAYMENT_START);
        if (status_item != nullptr) delete payment_start_item;

        QTableWidgetItem* payment_end_item = ui->tableWidgetProposalsDIP3->item(i, COLUMN_PAYMENT_END);
        if (status_item != nullptr) delete payment_end_item;

        QTableWidgetItem* creation_item = ui->tableWidgetProposalsDIP3->item(i, COLUMN_CREATION_TIME);
        if (status_item != nullptr) delete creation_item;
    }

    ui->tableWidgetProposalsDIP3->clearContents();
    ui->tableWidgetProposalsDIP3->setRowCount(0);

    nTimeUpdatedDIP3 = GetTime();

    for (int i = 0; i < governanceList.size(); i++) {
        CGovernanceObject* obj = (CGovernanceObject*)governanceList.at(i);

        QString isActiveStr = "";
        std::string err = "";

        if(obj->IsValidLocally(err, false))
            isActiveStr = "Yes";
        else
            isActiveStr = "No";

        QDateTime creationTime = QDateTime::fromSecsSinceEpoch(obj->GetCreationTime());
        QDateTime paymentStartTime = QDateTime::fromSecsSinceEpoch(obj->GetPaymentStartTime());
        QDateTime paymentEndTime = QDateTime::fromSecsSinceEpoch(obj->GetPaymentEndTime());
        int yesCount = obj->GetYesCount(VOTE_SIGNAL_FUNDING);
        int noCount = obj->GetNoCount(VOTE_SIGNAL_FUNDING);
        int abstainCount = obj->GetAbstainCount(VOTE_SIGNAL_FUNDING);
        int absYesCount = obj->GetAbsoluteYesCount(VOTE_SIGNAL_FUNDING);
        int paymentAmount = obj->GetPaymentAmount();
        int cycles = obj->GetCycles();
        int currentCycle = obj->GetCurrentCycle();

        QString paymentAmountStr = "-";
        QString cyclesStr = "-";
        QString currentCycleStr = "-";
        QString paymentStartStr = "-";
        QString paymentEndStr = "-";
        QString creationTimeStr = "-";

        if (obj->GetObjectType() == GOVERNANCE_OBJECT_PROPOSAL) {
            paymentAmountStr = QString::number(paymentAmount);
            cyclesStr = QString::number(cycles);
            currentCycleStr = QString::number(currentCycle);
            paymentStartStr = paymentStartTime.toString("dd.MM.yyyy HH.mm");
            paymentEndStr = paymentEndTime.toString("dd.MM.yyyy HH.mm");
            creationTimeStr = creationTime.toString("dd.MM.yyyy HH.mm");
        }
        QTableWidgetItem* statusItem = new QTableWidgetItem(isActiveStr);
        QTableWidgetItem* amountItem = new QTableWidgetItem(paymentAmountStr);
        QTableWidgetItem* cyclesItem = new QTableWidgetItem(cyclesStr);
        QTableWidgetItem* currentCycleItem = new QTableWidgetItem(currentCycleStr);
        QTableWidgetItem* absuluteYesCountItem = new QTableWidgetItem(QString::number(absYesCount));
        QTableWidgetItem* yesCountItem = new QTableWidgetItem(QString::number(yesCount));
        QTableWidgetItem* noCountItem = new QTableWidgetItem(QString::number(noCount));
        QTableWidgetItem* abstainCountItem = new QTableWidgetItem(QString::number(abstainCount));
        QTableWidgetItem* paymentStartItem = new QTableWidgetItem(paymentStartStr);
        QTableWidgetItem* paymentEndItem = new QTableWidgetItem(paymentEndStr);
        QTableWidgetItem* creationTimeItem = new QTableWidgetItem(creationTimeStr);

        //hash is embedding to COLUMN_STATUS/UserRole to use on row selection
        statusItem->setData(Qt::UserRole, QString::fromStdString(governanceList[i]->GetHash().ToString()));

        ui->tableWidgetProposalsDIP3->insertRow(0);

        ui->tableWidgetProposalsDIP3->setItem(0, COLUMN_STATUS, statusItem);
        ui->tableWidgetProposalsDIP3->setItem(0, COLUMN_AMOUNT, amountItem);
        ui->tableWidgetProposalsDIP3->setItem(0, COLUMN_CYCLES, cyclesItem);
        ui->tableWidgetProposalsDIP3->setItem(0, COLUMN_CURRENT_CYCLE, currentCycleItem);
        ui->tableWidgetProposalsDIP3->setItem(0, COLUMN_ABS_YES_COUNT, absuluteYesCountItem);
        ui->tableWidgetProposalsDIP3->setItem(0, COLUMN_YES_COUNT, yesCountItem);
        ui->tableWidgetProposalsDIP3->setItem(0, COLUMN_NO_COUNT, noCountItem);
        ui->tableWidgetProposalsDIP3->setItem(0, COLUMN_ABSTAIN_COUNT, abstainCountItem);
        ui->tableWidgetProposalsDIP3->setItem(0, COLUMN_PAYMENT_START, paymentStartItem);
        ui->tableWidgetProposalsDIP3->setItem(0, COLUMN_PAYMENT_END, paymentEndItem);
        ui->tableWidgetProposalsDIP3->setItem(0, COLUMN_CREATION_TIME, creationTimeItem);
    }

    ui->tableWidgetProposalsDIP3->setSortingEnabled(true);
}

void GovernanceList::on_filterLineEditDIP3_textChanged(const QString& strFilterIn)
{
    strCurrentFilterDIP3 = strFilterIn;
    nTimeFilterUpdatedDIP3 = GetTime();
    fFilterUpdatedDIP3 = true;
}

CGovernanceObject* GovernanceList::GetSelectedDIP3GOV()
{
    if (!clientModel) {
        return nullptr;
    }

    std::string strProTxHash;
    {
        LOCK(cs_dip3list);

        QItemSelectionModel* selectionModel = ui->tableWidgetProposalsDIP3->selectionModel();
        QModelIndexList selected = selectionModel->selectedRows();

        if (selected.count() == 0) return nullptr;

        QModelIndex index = selected.at(0);
        int nSelectedRow = index.row();
        strProTxHash = ui->tableWidgetProposalsDIP3->item(nSelectedRow, COLUMN_STATUS)->data(Qt::UserRole).toString().toStdString();
    }

    uint256 proTxHash;
    proTxHash.SetHex(strProTxHash);

    std::vector<const CGovernanceObject*> list = clientModel->getGovernanceList();
    for (int i = 0; i < list.size(); i++) {
        const CGovernanceObject* obj = list[i];

        if (proTxHash == obj->GetHash()) {
            return (CGovernanceObject*)obj;
        }
    }
    return nullptr;
}

void GovernanceList::extraInfoDIP3_clicked()
{
    CGovernanceObject* gov = GetSelectedDIP3GOV();
    if (gov == nullptr) {
        return;
    }

    UniValue json(UniValue::VOBJ);
    json = gov->ToJson();

    // Title of popup window
    QString strWindowtitle = tr("Additional information for Proposal %1").arg(QString::fromStdString(gov->GetHash().ToString()));
    QString strText = QString::fromStdString(json.write(2));

    QMessageBox::information(this, strWindowtitle, strText);
}

void GovernanceList::openProLink_clicked()
{
    CGovernanceObject* gov = GetSelectedDIP3GOV();
    if (gov == nullptr) {
        return;
    }

    std::string url = gov->GetURL();
    QDesktopServices::openUrl(QUrl(QString::fromStdString(url)));
}


//For future use. Maybe to show details in a some area on UI
void GovernanceList::tableWidgetRow_clicked()
{
    /*
    CGovernanceObject *gov = GetSelectedDIP3GOV();
    if (gov == nullptr) {
        return;
    }
    */

}
