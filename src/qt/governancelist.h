#ifndef BITCOIN_QT_GOVERNANCELIST_H
#define BITCOIN_QT_GOVERNANCELIST_H

#include <primitives/transaction.h>
#include <sync.h>

#include <governance/governance-classes.h>

#include <QMenu>
#include <QTimer>
#include <QWidget>
#include <QDesktopServices>
#include <QUrl>
#include <QNetworkAccessManager>
#include <QNetworkReply>

#define MASTERNODELIST_UPDATE_SECONDS 3
#define MASTERNODELIST_FILTER_COOLDOWN_SECONDS 3

namespace Ui
{
class GovernanceList;
}

class ClientModel;
class WalletModel;
class CGovernanceObject;
class CSuperblock;

QT_BEGIN_NAMESPACE
class QModelIndex;
QT_END_NAMESPACE

/** Masternode Manager page widget */
class GovernanceList : public QWidget
{
    Q_OBJECT

public:
    explicit GovernanceList(QWidget* parent = 0);
    ~GovernanceList();

    enum {
        COLUMN_STATUS,
        COLUMN_AMOUNT,
        COLUMN_CYCLES,
        COLUMN_CURRENT_CYCLE,
        COLUMN_ABS_YES_COUNT,
        COLUMN_YES_COUNT,
        COLUMN_NO_COUNT,
        COLUMN_ABSTAIN_COUNT,
        COLUMN_PAYMENT_START,
        COLUMN_PAYMENT_END,
        COLUMN_CREATION_TIME,
    };

    void setClientModel(ClientModel* clientModel);
    void setWalletModel(WalletModel* walletModel);

private:
    QMenu* contextMenuDIP3;
    int64_t nTimeFilterUpdatedDIP3;
    int64_t nTimeUpdatedDIP3;
    bool fFilterUpdatedDIP3;

    QTimer* timer;
    Ui::GovernanceList* ui;
    ClientModel* clientModel;
    WalletModel* walletModel;

    // Protects tableWidgetProposalsDIP3
    CCriticalSection cs_dip3list;

    QString strCurrentFilterDIP3;

    bool mnListChanged;

    CGovernanceObject *GetSelectedDIP3GOV();

    void updateDIP3List();

Q_SIGNALS:
    void doubleClicked(const QModelIndex&);

private Q_SLOTS:
    void showContextMenuDIP3(const QPoint&);
    void on_filterLineEditDIP3_textChanged(const QString& strFilterIn);

    void extraInfoDIP3_clicked();
    void openProLink_clicked();
    void tableWidgetRow_clicked();

    void handleGovernanceListChanged();
    void updateDIP3ListScheduled();
};
#endif // BITCOIN_QT_MASTERNODELIST_H
