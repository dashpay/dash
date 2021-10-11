#ifndef BITCOIN_QT_GOVERNANCELIST_H
#define BITCOIN_QT_GOVERNANCELIST_H

#include <primitives/transaction.h>
#include <sync.h>
#include <util/system.h>
#include <governance/object.h>

#include <QAbstractTableModel>
#include <QMenu>
#include <QSortFilterProxyModel>
#include <QTimer>
#include <QWidget>
#include <QDateTime>

#define GOVERNANCELIST_UPDATE_SECONDS 10
#define GOVERNANCELIST_DATEFMT "yyyy-MM-dd hh:mm"

namespace Ui
{
class GovernanceList;
}

class CDeterministicMNList;
class ClientModel;

class Proposal : public QObject 
{
private:
    Q_OBJECT

	const CGovernanceObject* pGovObj;
	QString m_title;
	QDateTime m_startDate;
	QDateTime m_endDate;
	float m_paymentAmount;
	QString m_url;
public:
	Proposal(const CGovernanceObject *p, QObject *parent = nullptr);
	QString title() const;
	QString hash() const;
	QDateTime startDate() const;
	QDateTime endDate() const;
	float paymentAmount() const;
	QString url() const;
	bool isActive() const;
    QString status() const;

    void openUrl() const;
};

class ProposalModel : public QAbstractTableModel 
{
private:
	QList<const Proposal*> m_data;
public:
	ProposalModel(QObject* parent = nullptr);
	int rowCount(const QModelIndex &parent = QModelIndex()) const override;
	int columnCount(const QModelIndex &parent = QModelIndex()) const override;
	QVariant data(const QModelIndex &index, int role) const override;
	QVariant headerData(int section, Qt::Orientation orientation, int role) const override;
    int columnWidth(int section) const;
	void append(const Proposal* proposal);
    void remove(int row);
    void reconcile(const std::vector<const Proposal*> &proposals);

    const Proposal* getProposalAt(const QModelIndex &index) const;
};

/** Governance Manager page widget */
class GovernanceList : public QWidget
{
    Q_OBJECT

public:
    explicit GovernanceList(QWidget* parent = nullptr);
    ~GovernanceList();
    void setClientModel(ClientModel* clientModel);

private:
    ClientModel* clientModel;

    Ui::GovernanceList* ui;
	ProposalModel* proposalModel;
    QSortFilterProxyModel* proposalModelProxy;

    QMenu* proposalContextMenu;
    QTimer* timer;

    void updateProposalList();
    void updateProposalCount();

Q_SIGNALS:
    void doubleClicked(const QModelIndex&);

private Q_SLOTS:
    void showProposalContextMenu(const QPoint& pos);
};


#endif // BITCOIN_QT_GOVERNANCELIST_H
