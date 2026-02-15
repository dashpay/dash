// Copyright (c) 2021-2025 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_QT_PROPOSALLIST_H
#define BITCOIN_QT_PROPOSALLIST_H

#include <interfaces/node.h>
#include <primitives/transaction.h>
#include <pubkey.h>
#include <saltedhasher.h>

#include <qt/proposalmodel.h>

#include <QMenu>
#include <QSortFilterProxyModel>
#include <QTimer>
#include <QThread>
#include <QWidget>

#include <atomic>
#include <map>
#include <optional>
#include <vector>

inline constexpr int PROPOSALLIST_UPDATE_SECONDS = 10;

class ClientModel;
class ProposalModel;
class WalletModel;
class ProposalCreate;
class CDeterministicMNList;
enum vote_outcome_enum_t : int;
namespace Governance {
class Object;
} // namespace Governance
namespace Ui {
class ProposalList;
} // namespace Ui

enum class ProposalSource : uint8_t {
    Active,
    Local
};

struct ProposalData {
    int m_abs_vote_req{0};
    interfaces::GOV::GovernanceInfo m_gov_info;
    Proposals m_proposals;
    Uint256HashMap<CKeyID> m_votable_masternodes;
    Uint256HashSet m_fundable_hashes;
};

class ProposalFeed : public QObject {
    Q_OBJECT

public:
    ProposalFeed();
    ~ProposalFeed();

    static ProposalData fetch(ClientModel* client_model);
};

/** Proposal list widget */
class ProposalList : public QWidget
{
    Q_OBJECT

    Ui::ProposalList* ui;

public:
    explicit ProposalList(QWidget* parent = nullptr);
    ~ProposalList() override;
    void setClientModel(ClientModel* clientModel);
    void setWalletModel(WalletModel* walletModel);

private:
    ClientModel* clientModel{nullptr};
    interfaces::GOV::GovernanceInfo m_gov_info;
    ProposalModel* proposalModel{nullptr};
    ProposalSource m_proposal_source{ProposalSource::Active};
    QMenu* proposalContextMenu{nullptr};
    QObject* m_worker{nullptr};
    QSortFilterProxyModel* proposalModelProxy{nullptr};
    QThread* m_thread{nullptr};
    QTimer* m_timer{nullptr};
    std::atomic<bool> m_col_refresh{false};
    std::atomic<bool> m_in_progress{false};
    Uint256HashMap<CKeyID> votableMasternodes;
    WalletModel* walletModel{nullptr};

    bool canVote() const { return !votableMasternodes.empty(); }
    ProposalData calcProposalList() const;
    int queryCollateralDepth(const uint256& collateralHash) const;
    std::vector<Governance::Object> getWalletProposals(std::optional<bool> pending) const;
    void handleProposalListChanged(bool force);
    void refreshColumnWidths();
    void setProposalList(ProposalData&& data);
    void updateEmptyPagePalette();
    void updateEmptyState();
    void updateProposalButtons();
    void voteForProposal(vote_outcome_enum_t outcome);

protected:
    void changeEvent(QEvent* event) override;
    void showEvent(QShowEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

private Q_SLOTS:
    void updateDisplayUnit();
    void updateProposalList();
    void updateProposalCount();
    void updateMasternodeCount() const;
    void setProposalSource(int index);
    void showProposalContextMenu(const QPoint& pos);
    void showAdditionalInfo(const QModelIndex& index);
    void showCreateProposalDialog();
    void showResumeProposalDialog();
    void openProposalUrl();
    void copyProposalJson();

    // Voting slots
    void voteYes();
    void voteNo();
    void voteAbstain();
};

#endif // BITCOIN_QT_PROPOSALLIST_H
