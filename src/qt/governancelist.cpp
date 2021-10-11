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

#include <univalue.h>

#include <QAbstractItemView>
#include <QDesktopServices>
#include <QMessageBox>
#include <QTableWidgetItem>
#include <QUrl>
#include <QtGui/QClipboard>


GovernanceList::GovernanceList(QWidget* parent) :
    QWidget(parent),
    ui(new Ui::GovernanceList),
    clientModel(0),
    proposalModel(new ProposalModel(this)),
    proposalModelProxy(new QSortFilterProxyModel(this)),
    proposalContextMenu(new QMenu(this)),
    timer(new QTimer(this))
{
    ui->setupUi(this);

    GUIUtil::setFont({ui->label_count_2, ui->countLabel}, GUIUtil::FontWeight::Bold, 14);
    GUIUtil::setFont({ui->label_filter_2}, GUIUtil::FontWeight::Normal, 15);

    proposalModelProxy->setSourceModel(proposalModel);
    ui->govTableView->setModel(proposalModelProxy);
    ui->govTableView->setSelectionBehavior(QAbstractItemView::SelectRows);
    ui->govTableView->horizontalHeader()->setStretchLastSection(true);
    //ui->govTableView->sortItems(2, Qt::DescendingOrder); // sort on created... 

    for (int i = 0; i < proposalModel->columnCount(); ++i) {
       ui->govTableView->setColumnWidth(i, proposalModel->columnWidth(i)); 
    }

    // Set up filtering.
    proposalModelProxy->setFilterKeyColumn(1);  // filter by title column...
    ui->filterLineEdit->setPlaceholderText(tr("Filter by Title"));
    connect(ui->filterLineEdit, &QLineEdit::textChanged, proposalModelProxy, &QSortFilterProxyModel::setFilterFixedString);

    // Changes to number of rows should update proposal count display.
    connect(proposalModelProxy, &QSortFilterProxyModel::rowsInserted, this, &GovernanceList::updateProposalCount);
    connect(proposalModelProxy, &QSortFilterProxyModel::rowsRemoved, this, &GovernanceList::updateProposalCount);
    connect(proposalModelProxy, &QSortFilterProxyModel::layoutChanged, this, &GovernanceList::updateProposalCount);

    // Enable CustomContextMenu on the table to make the view emit customContextMenuRequested signal.
    ui->govTableView->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(ui->govTableView, &QTableView::customContextMenuRequested, this, &GovernanceList::showProposalContextMenu);

    connect(timer, &QTimer::timeout, this, &GovernanceList::updateProposalList);

    GUIUtil::updateFonts();
}

GovernanceList::~GovernanceList()
{
    delete ui;
}

void GovernanceList::setClientModel(ClientModel* model)
{
    this->clientModel = model;
    updateProposalList();
}

void GovernanceList::updateProposalList()
{
    if (this->clientModel) {
        std::vector<const CGovernanceObject*> govObjList = clientModel->getAllGovernanceObjects();
        std::vector<const Proposal*> newProposals;
        for (const auto pGovObj: govObjList) {

            auto govObjType = pGovObj->GetObjectType();
            if (govObjType != GOVERNANCE_OBJECT_PROPOSAL) {
                continue; // Skip triggers.
            }
            
            newProposals.push_back(new Proposal(pGovObj, proposalModel));

        }
        proposalModel->reconcile(newProposals);
    }

    // Schedule next update.
    timer->start(GOVERNANCELIST_UPDATE_SECONDS * 1000);

}

void GovernanceList::updateProposalCount()
{
    ui->countLabel->setText(QString::number(proposalModelProxy->rowCount()));
}

void GovernanceList::showProposalContextMenu(const QPoint& pos)
{
    const auto index = ui->govTableView->indexAt(pos);

    if (!index.isValid()) {
        return;
    }
    
    const auto proposal = proposalModel->getProposalAt(index);

    // right click menu with option to open proposal url
    QAction* openProposalUrl = new QAction(tr("Open url"), this);
    proposalContextMenu->addAction(openProposalUrl);
    connect(openProposalUrl, &QAction::triggered, proposal, &Proposal::openUrl);
    proposalContextMenu->exec(QCursor::pos());
}

///
/// Proposal wrapper
///

Proposal::Proposal(const CGovernanceObject *p, QObject *parent) :
    QObject(parent), 
    pGovObj(p)
{
    UniValue prop_data;
    if (prop_data.read(pGovObj->GetDataAsPlainString())) {
        UniValue titleValue = find_value(prop_data, "name");
        if (titleValue.isStr()) {
            m_title = QString::fromStdString(titleValue.get_str());
        }

        UniValue paymentStartValue = find_value(prop_data, "start_epoch");
        if (paymentStartValue.isNum()) {
            m_startDate = QDateTime::fromSecsSinceEpoch(paymentStartValue.get_int());
        }

        UniValue paymentEndValue = find_value(prop_data, "end_epoch");
        if (paymentEndValue.isNum()) {
            m_endDate = QDateTime::fromSecsSinceEpoch(paymentEndValue.get_int());
        }

        UniValue amountValue = find_value(prop_data, "payment_amount");
        if (amountValue.isNum()) {
            m_paymentAmount = amountValue.get_int();
        }

        UniValue urlValue = find_value(prop_data, "url");
        if (urlValue.isStr()) {
            m_url = QString::fromStdString(urlValue.get_str());
        }
    }
};

QString Proposal::title() const { return m_title; }

QString Proposal::hash() const { return QString::fromStdString(pGovObj->GetHash().ToString()); }

QDateTime Proposal::startDate() const { return m_startDate; }

QDateTime Proposal::endDate() const { return m_endDate; }

float Proposal::paymentAmount() const { return m_paymentAmount; }

QString Proposal::url() const { return m_url; }

bool Proposal::isActive() const 
{
    std::string strError = "";
    return pGovObj->IsValidLocally(strError, false);
}

QString Proposal::status() const
{
    // Voting status...


    // A propsal is considered passing if (YES votes - NO votes) > (Total Number of Masternodes / 10),
    // count total valid (ENABLED) masternodes to determine passing threshold.
    // int nMnCount = clientModel->getMasternodeList().GetValidMNsCount();
    // int nAbsVoteReq = std::max(Params().GetConsensus().nGovernanceMinQuorum, nMnCount / 10);

    // TODO: determine if voting is in progress vs. funded or not funded for past proposals.
    // see CSuperblock::GetNearestSuperblocksHeights(nBlockHeight, nLastSuperblock, nNextSuperblock); 
    /*
    int absYesCount = pGovObj->GetAbsoluteYesCount(VOTE_SIGNAL_FUNDING);
    QString qStatusString;
    if (absYesCount > nAbsVoteReq) {
        // Could use pGovObj->IsSetCachedFunding here, but need nAbsVoteReq to display numbers anyway.
        qStatusString = QString("Passing +%1 (%2 of %3 needed)").arg(absYesCount - nAbsVoteReq).arg(absYesCount).arg(nAbsVoteReq);
    } else {
        qStatusString = QString("Needs additional %1 votes").arg(nAbsVoteReq - absYesCount);
    }
    */
    return QString::fromStdString("TODO");
}

void Proposal::openUrl() const
{
    QDesktopServices::openUrl(QUrl(m_url)); 
}

///
/// Proposal Model
///

ProposalModel::ProposalModel(QObject *parent) : QAbstractTableModel(parent) {}

int ProposalModel::rowCount(const QModelIndex &index) const
{
    return m_data.count();
}

int ProposalModel::columnCount(const QModelIndex &index) const
{
    return 8;
}

QVariant ProposalModel::data(const QModelIndex &index, int role) const
{
    if (role != Qt::DisplayRole && role != Qt::EditRole) return {};
    const auto proposal = m_data[index.row()];
    switch (index.column()) {
        case 0: return proposal->hash();
        case 1: return proposal->title();
        case 2: return proposal->startDate();
        case 3: return proposal->endDate();
        case 4: return proposal->paymentAmount();
        case 5: return proposal->isActive() ? "Y" : "N";
        case 6: return proposal->status();
        default: return {};
    };
}
    


QVariant ProposalModel::headerData(int section, Qt::Orientation orientation, int role) const
{
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole) return {};
    switch (section) {
        case 0: return "Hash";
        case 1: return "Title";
        case 2: return "Start";
        case 3: return "End";
        case 4: return "Amount";
        case 5: return "Active";
        case 6: return "Status";
        default: return {};
    }
}

int ProposalModel::columnWidth(int section) const {
    switch (section) {
        case 0: return 80;
        case 1: return 220;
        case 2: return 110;
        case 3: return 110;
        case 4: return 110;
        case 5: return 80;
        case 6: return 220;
        default: return 80;
    }
}

void ProposalModel::append(const Proposal* proposal)
{
    beginInsertRows({}, m_data.count(), m_data.count());
    m_data.append(proposal);
    endInsertRows();
}

void ProposalModel::remove(int row)
{
    beginRemoveRows({}, row, row);
    m_data.removeAt(row);
    endRemoveRows();
}

void ProposalModel::reconcile(const std::vector<const Proposal*> &proposals)
{
    std::vector<bool> keep_index(m_data.count(), false);
    for (const auto proposal : proposals) {
        bool found = false;
        for (unsigned int i = 0; i < m_data.count(); ++i) {
            if (m_data.at(i)->hash() == proposal->hash()) {
                found = true;
                keep_index.at(i) = true;
                break;
            }
        }
        if (!found) {
            append(proposal);
        }
    }
    for (unsigned int i = keep_index.size(); i > 0; --i) {
        if (!keep_index.at(i-1)) {
            remove(i-1);
        }
    }
}

const Proposal* ProposalModel::getProposalAt(const QModelIndex &index) const
{
    return m_data[index.row()];
}
