#include <qt/governancelist.h>
#include <qt/forms/ui_governancelist.h>

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

    int columnNoWidth = 80;
    int columnTitleWidth = 200;
    int columnOwnerWidth = 100;
    int columnStatusWidth = 80;
    int columnActiveWidth = 80;
    int columnBudgetWidth = 80;

    ui->tableWidgetGovernances->setColumnWidth(COLUMN_NO, columnNoWidth);
    ui->tableWidgetGovernances->setColumnWidth(COLUMN_TITLE, columnTitleWidth);
    ui->tableWidgetGovernances->setColumnWidth(COLUMN_OWNER, columnOwnerWidth);
    ui->tableWidgetGovernances->setColumnWidth(COLUMN_STATUS, columnStatusWidth);
    ui->tableWidgetGovernances->setColumnWidth(COLUMN_ACTIVE, columnActiveWidth);
    ui->tableWidgetGovernances->setColumnWidth(COLUMN_BUDGET, columnBudgetWidth);

    ui->tableWidgetGovernances->setContextMenuPolicy(Qt::CustomContextMenu);

    ui->filterLineEdit->setPlaceholderText(tr("Filter by any property"));

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
    } else {
        ui->countLabel->setText("update in " + QString::number(nSecondsToWait));
    }

    timer->start(nSecondsToWait * 1000);
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

    // TODO: implement filtering...

    nTimeGovernanceUpdated = GetTime();
    for (const auto pGovObj: govObjList) {
        QString qHashString = QString::fromStdString(pGovObj->GetHash().ToString());
        QTableWidgetItem* hashItem = new QTableWidgetItem(qHashString);

        ui->tableWidgetGovernances->insertRow(0);
        ui->tableWidgetGovernances->setItem(0, COLUMN_NO, hashItem);

    }
    ui->countLabel->setText(QString::number(ui->tableWidgetGovernances->rowCount()+1));
    ui->tableWidgetGovernances->setSortingEnabled(true);

}

void GovernanceList::on_filterLineEdit_textChanged(const QString& strFilterIn)
{
    strCurrentFilter = strFilterIn;
    nTimeGovernanceFilterUpdated = GetTime();
    fGovernanceFilterUpdated = true;
    ui->countLabel->setText(tr("Please wait...") + " " + QString::number(GOVERNANCELIST_FILTER_COOLDOWN_SECONDS));
}


