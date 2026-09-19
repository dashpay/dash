// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_QT_SHAREDMNCREATEDIALOG_H
#define BITCOIN_QT_SHAREDMNCREATEDIALOG_H

#include <consensus/amount.h>

#include <qt/mnsharesession.h>

#include <QDialog>
#include <QString>
#include <QStringList>
#include <QVector>

#include <memory>
#include <optional>
#include <vector>

class BitcoinAmountField;
class OperatorKeyWidget;
class ProTxSender;
class QValidatedLineEdit;
class SharedMnStatusBoard;
class SharedMnWalkthroughTests;
class SharedMnWizardTests;
struct ProTxResult;
class UniValue;
class WalletModel;

namespace interfaces {
class Node;
} // namespace interfaces

QT_BEGIN_NAMESPACE
class QButtonGroup;
class QComboBox;
class QCheckBox;
class QDoubleSpinBox;
class QLabel;
class QLineEdit;
class QProgressBar;
class QPushButton;
class QSpinBox;
class QStackedWidget;
class QTableWidget;
class QVBoxLayout;
QT_END_NAMESPACE

//! Multi-page dialog creating a masternode several people fund together.
//!
//! One participant coordinates. The group works through exactly three rounds,
//! and in each of them the coordinator hands out one identical message and
//! everybody answers it independently, so no participant ever waits for
//! another participant:
//!
//!   1. Invitation        -> Details            (addresses and funding coins)
//!   2. Locked Terms      -> Approval           (share owner consent signatures)
//!   3. Signing Request   -> Signed Contribution (funding input signatures)
//!
//! The rounds are irreducible: the coordinator cannot prepare a transaction
//! before it knows everyone's addresses and coins, consent must be signed over
//! the frozen transaction, and funding inputs can only be signed after
//! combining the consent signatures rewrites the payload.
//!
//! Everything the group exchanges is one self-describing MnShareSession
//! envelope carrying a short fingerprint, so the user never says what a
//! message is - the dialog recognises it. Session state lives entirely in that
//! envelope; any participant holding the latest copy can continue.
class SharedMnCreateDialog : public QDialog
{
    Q_OBJECT

public:
    explicit SharedMnCreateDialog(interfaces::Node& node, WalletModel* wallet_model, QWidget* parent = nullptr);
    ~SharedMnCreateDialog() override;

    //! Adopt a message the user pasted outside this dialog (the masternode
    //! list's single paste entry point). Safe to call before exec().
    void openSharedMessage(const QString& text);

    //! Close protection: offers to save an unsaved session first
    void reject() override;

private Q_SLOTS:
    void onNext();
    void onBack();
    void onPaste();
    void onOpenFile();
    void onSave();

private:
    // Stable seams for the headless page-by-page tests: goToPage(),
    // currentPage(), handleImportedText() and copySession() are what they
    // drive the dialog through.
    friend class SharedMnWalkthroughTests;
    friend class SharedMnWizardTests;

    //! Insertion order into the stack must match this enum: the enum value
    //! doubles as the stack index.
    enum Page : int {
        PageLanding = 0,
        PageParticipants,
        PageSettings,
        PageExitTerms,
        PageContribution,
        PageSecret,
        PageInvite,
        PageWaitTerms,
        PageApprovals,
        PageWaitSigning,
        PageSignatures,
        PageWaitBroadcast,
        PageComplete,
    };

    enum class Role {
        Undecided,   //!< nothing started or imported yet
        Coordinator, //!< hands out the three messages and broadcasts
        Participant, //!< answers the three messages
    };

    //! Coins chosen to fund one share, before anything is recorded in the
    //! session: the change output's address is only created when the user
    //! actually reserves them.
    struct CoinSelection {
        std::vector<MnShareSession::Input> inputs;
        CAmount total{0};
        CAmount change{0};
    };

    static constexpr int SHARE_COL_NUMBER{0};
    static constexpr int SHARE_COL_NAME{1};
    static constexpr int SHARE_COL_AMOUNT{2};
    static constexpr int SHARE_COL_ME{3};
    static constexpr int SHARE_COLUMNS{4};

    // Page construction
    QWidget* createLandingPage();
    QWidget* createParticipantsPage();
    QWidget* createSettingsPage();
    QWidget* createExitTermsPage();
    QWidget* createContributionPage();
    QWidget* createSecretPage();
    QWidget* createInvitePage();
    QWidget* createApprovalsPage();
    QWidget* createSignaturesPage();
    QWidget* createWaitingPage(Page page);
    QWidget* createCompletePage();
    //! Status board inside `layout`, registered for refreshBoards(). The page
    //! it belongs to decides what its one-line summary counts.
    SharedMnStatusBoard* addBoard(Page page, QWidget* parent, QVBoxLayout* layout);

    // Navigation, mirroring RegisterMasternodeWizard
    void rebuildOrder();
    Page currentPage() const;
    QString pageTitle(Page page) const;
    void updateProgress();
    void goToPage(Page page);
    void enterPage(Page page);
    bool validatePage(Page page, QString& err);
    void updateButtons();
    void onPageEdited();
    void setBusy(bool busy, const QString& busy_text = QString());
    void showError(const QString& message);
    //! Non-modal confirmation line under the button row ("Copied - ...")
    void showStatus(const QString& message);
    //! True while the generated operator secret exists nowhere but this dialog
    bool secretGateRequired() const;
    //! True when the user typed the generated operator secret's last 4 characters
    bool secretConfirmed() const;
    //! True while a secret this dialog generated would be lost by closing:
    //! the session file carries only the operator public key
    bool operatorSecretUnsaved() const;

    // Session -> widgets
    void refreshAll();
    void refreshWindowTitle();
    void refreshShareTable();
    void refreshSumMeter();
    void refreshExitTerms();
    void refreshContributionPage();
    void refreshInvitePage();
    void refreshApprovalsPage();
    void refreshSignaturesPage();
    void refreshWaitingPages();
    void refreshCompletePage();
    void refreshBoards();
    void refreshTermSheets();

    // Widgets -> session
    void syncSharesFromTable();
    void syncTermsToSession();
    void pushSessionToWidgets();

    // The three rounds
    void startSession();
    void beginLockConfirmation();
    void lockTerms();
    bool signOwnConsent(QString& error);
    void approveAndCopy();
    bool combineApprovals(QString& error);
    bool signOwnFundingInputs(bool& complete, QString& error);
    //! Funding inputs of the prepared transaction this wallet could spend but
    //! never contributed, as "txid:vout". Signing runs over the whole
    //! transaction, so a coin of ours recorded under somebody else's
    //! contribution would be signed along with our own.
    QStringList foreignWalletInputs() const;
    //! False when signing `before_hex` into `after_hex` touched anything
    //! outside `mine`: a scriptSig that appeared on another input, a rewritten
    //! prevout, or a different input count. `outpoint` names the offending
    //! coin, and is empty when the transaction's shape itself changed.
    static bool signedOnlyOwnInputs(const QString& before_hex, const QString& after_hex,
                                    const MnShareSession::Contribution* mine, QString& outpoint);
    //! Refusal naming a funding input this wallet owns but never contributed
    QString foreignInputError(const QString& outpoint) const;
    //! Record the first funding input this wallet owns but never contributed,
    //! so the pages leading to signing can refuse before the wallet unlocks
    void checkForeignInputs();
    void runCombineAndSign();
    void signAndCopy();
    void unlockTerms();
    void broadcastRegistration();
    bool confirmBroadcast() const;

    // Messages
    //! Display name of what `session` currently is, from the reader's point of
    //! view ("Invitation", "Details", "Locked Terms", ...)
    QString messageName(const MnShareSession& session, bool from_coordinator) const;
    //! Name of the participant a reply came from, or an empty string
    QString senderName(const MnShareSession& reply) const;
    //! Who the current message goes to
    QString recipientName() const;
    void copySession(const QString& what);
    void saveSession();
    void handleImportedText(const QString& text);
    void absorbSession(const MnShareSession& imported);
    void replaceSession(const MnShareSession& imported);

    // Funding
    //! Fresh receive address from the wallet, or empty with `error` set
    QString freshAddress(QString& error) const;
    //! Confirmed, unlocked, spendable wallet coins covering `target`, largest
    //! first, with the change kept above the dust threshold when possible
    bool selectCoins(CAmount target, CoinSelection& selection, QString& error) const;
    //! Amount this wallet must contribute: its share plus, for the
    //! coordinator, the network fee
    CAmount contributionTarget() const;
    void reserveCoins();
    void releaseCoins();
    //! Give up the coins this wallet reserved for the session, from a page
    //! where the contribution itself can no longer be withdrawn. Confirms
    //! first: if the coins are then spent, nobody can complete the
    //! registration.
    void releaseReservedCoins();
    //! True when at least one coin this wallet contributed is still locked in
    //! it, i.e. there is something for "Release my coins" to do
    bool hasLockedContributedCoins() const;
    //! Lock the coins of a session in progress, or unlock them once it is
    //! broadcast and they are spent
    void refreshContributedCoinLocks();
    void setContributionLocked(const MnShareSession::Contribution& contribution, bool lock);
    void unlockAllContributedCoins();
    void lockKnownContributedCoins();
    //! Re-check that every funding coin this wallet contributed is still
    //! unspent, and set m_dead_reason when one is gone
    void checkSessionLiveness();
    //! Contribution recorded for the share this wallet holds, or nullptr
    const MnShareSession::Contribution* myContribution() const;
    //! Value of a recorded funding input: this wallet's own view for the coins
    //! it tracks, else the confirmed UTXO set; nullopt when it cannot be
    //! resolved here (another participant's unconfirmed coin)
    std::optional<CAmount> resolveInputValue(const MnShareSession::Input& input) const;
    //! One sentence about whether the recorded inputs cover collateral, change
    //! and a positive fee; `fatal` marks funding that could never broadcast
    QString fundingCheck(bool& fatal) const;

    // Session queries
    int myShareIndex() const { return m_my_share; }
    //! Pick the share this wallet holds from the session, preferring a share
    //! whose owner address this wallet can spend from
    void inferMyShare();
    //! True when `session` is this wallet's own coordinated session, reopened
    //! from a backup: this wallet prepared it and can spend the owner key of
    //! the share it names as the coordinator's. A registration takes days, so
    //! the coordinator will close and reopen the dialog; without this every
    //! reopened session would come back as a Participant and could never be
    //! locked, combined or broadcast.
    bool coordinatedHere(const MnShareSession& session) const;
    bool canSign() const;
    //! True when this wallet coordinates a locked session that is still missing
    //! its own consent signature. The approval runs automatically at lock, but
    //! a cancelled unlock or a failed "protx shared_sign" leaves it undone, and
    //! allApproved() can then never become true.
    bool needsOwnApproval() const;
    bool hasDetails(int share_index) const;
    bool hasFunding(int share_index) const;
    //! Signed funding inputs of a share, as "signed of total"
    std::pair<int, int> fundingSignatureCount(int share_index) const;
    bool allDetailsCollected() const;
    bool allApproved() const;
    bool allFundingSigned() const;

    //! Execute one RPC command on the bridge, waiting in a nested event loop on
    //! the GUI thread (which keeps the non-movable unlock context alive when
    //! needs_unlock). Returns false when the call could not be started.
    bool runRpc(const QString& method, const UniValue& params, bool needs_unlock, ProTxResult& result);
    //! Replace the session's transaction hex in place (newly signed funding
    //! inputs) without changing the stage, via an envelope round-trip
    bool replaceSessionProTx(const QString& tx_hex, QString& error);

    interfaces::Node& m_node;
    WalletModel* const m_wallet_model;
    const bool m_v24_active;
    MnShareSession m_session;
    ProTxSender* const m_sender;

    QVector<Page> m_order;
    int m_pos{0};
    bool m_busy{false};
    bool m_dirty{false};
    bool m_updating{false}; //!< guards widget refresh against change signals
    bool m_lock_confirming{false};
    bool m_finished{false};
    Role m_role{Role::Undecided};
    int m_my_share{-1};
    std::optional<Page> m_validation_page;
    //! Advisory returned by "protx shared_register_prepare" for the locked
    //! terms (a zero early penalty lets anyone exit at will). Not part of the
    //! envelope, so it only exists on the wallet that prepared it.
    QString m_prepare_warning;
    //! The session was imported rather than started here, so its operator key
    //! is one the group already agreed on
    bool m_session_imported{false};
    //! The session arrived with an agreed operator key: show it read-only and
    //! never overwrite it from this dialog's key widget
    bool m_operator_key_from_import{false};
    //! Revision of the draft when the last participant reply was absorbed, or
    //! -1 when no reply has come back yet. A later revision means the
    //! coordinator edited the draft after somebody had already answered it,
    //! which is the only case where the replies really are stale.
    int m_replies_absorbed_revision{-1};
    //! A recorded funding coin was spent, so the registration can never
    //! complete
    QString m_dead_reason;
    //! "txid:vout" of a funding input this wallet owns but never contributed,
    //! i.e. a coin this session would have us sign away
    QString m_foreign_input;
    //! Warning from the envelope currently being imported ("This message was
    //! The last message this wallet handed on, for the waiting pages
    QString m_sent_what;
    QString m_sent_code;
    QString m_sent_time;

    QStackedWidget* m_pages{nullptr};
    QLabel* m_progress_label{nullptr};
    QLabel* m_error_label{nullptr};
    QLabel* m_status_label{nullptr};
    QProgressBar* m_busy_bar{nullptr};
    QPushButton* m_back_button{nullptr};
    QPushButton* m_paste_button{nullptr};
    QPushButton* m_save_button{nullptr};
    QPushButton* m_cancel_button{nullptr};
    QPushButton* m_next_button{nullptr};
    std::vector<std::pair<Page, SharedMnStatusBoard*>> m_boards;

    // Landing
    QLabel* m_landing_gate{nullptr};
    QPushButton* m_start_button{nullptr};

    // Step 1 - Participants
    QTableWidget* m_share_table{nullptr};
    QButtonGroup* m_me_group{nullptr};
    QPushButton* m_add_share_button{nullptr};
    QPushButton* m_remove_share_button{nullptr};
    QLabel* m_sum_label{nullptr};

    // Step 2 - Masternode settings
    QLineEdit* m_service_edit{nullptr};
    OperatorKeyWidget* m_operator_widget{nullptr};
    QLabel* m_operator_imported_label{nullptr};
    QLineEdit* m_node_run_by_edit{nullptr};
    QValidatedLineEdit* m_voting_edit{nullptr};
    QDoubleSpinBox* m_operator_reward_spin{nullptr};
    QLabel* m_operator_reward_warning{nullptr};

    // Step 3 - Exit terms
    QButtonGroup* m_preset_group{nullptr};
    QSpinBox* m_early_period_spin{nullptr};
    QLabel* m_early_period_hint{nullptr};
    BitcoinAmountField* m_early_penalty_field{nullptr};
    QLabel* m_early_penalty_hint{nullptr};
    QLabel* m_early_preview{nullptr};
    QLabel* m_early_warning{nullptr};

    // Step 4 - Your contribution
    QLabel* m_contribution_title{nullptr};
    QLabel* m_contribution_you{nullptr};
    QComboBox* m_who_am_i_combo{nullptr};
    QWidget* m_who_am_i_row{nullptr};
    QValidatedLineEdit* m_owner_edit{nullptr};
    QValidatedLineEdit* m_refund_edit{nullptr};
    QValidatedLineEdit* m_reward_edit{nullptr};
    QWidget* m_fee_box{nullptr};
    BitcoinAmountField* m_fee_field{nullptr};
    QLabel* m_coins_label{nullptr};
    QPushButton* m_reserve_button{nullptr};
    QPushButton* m_release_button{nullptr};

    // Save operator key
    QLineEdit* m_secret_edit{nullptr};
    QLineEdit* m_conf_line_edit{nullptr};
    QLineEdit* m_confirm_edit{nullptr};

    // Step 5 - Invite participants
    QPushButton* m_copy_invitation_button{nullptr};
    QPushButton* m_save_invitation_button{nullptr};
    QWidget* m_lock_confirm_card{nullptr};
    QLabel* m_lock_terms_label{nullptr};
    QLabel* m_lock_funding_label{nullptr};
    QLabel* m_invite_edit_warning{nullptr};

    // Approvals / Approve terms
    QLabel* m_approvals_title{nullptr};
    QLabel* m_approvals_purpose{nullptr};
    QLabel* m_approvals_terms{nullptr};
    QPushButton* m_copy_terms_button{nullptr};
    QPushButton* m_unlock_button{nullptr};
    QLabel* m_prepare_warning_label{nullptr};

    // Signatures / Sign contribution
    QLabel* m_signatures_title{nullptr};
    QLabel* m_signatures_purpose{nullptr};
    QLabel* m_signatures_summary{nullptr};
    QPushButton* m_copy_signing_button{nullptr};

    // Waiting pages
    struct WaitingPage {
        QLabel* title{nullptr};
        QLabel* sent{nullptr};
        QLabel* next{nullptr};
        QPushButton* release{nullptr};
    };
    std::vector<std::pair<Page, WaitingPage>> m_waiting_pages;

    // Complete
    QLabel* m_complete_hash{nullptr};
    QCheckBox* m_keep_owner{nullptr};
    QCheckBox* m_keep_operator{nullptr};
    QCheckBox* m_keep_standby{nullptr};
    QCheckBox* m_keep_record{nullptr};
    QLabel* m_keep_owner_hint{nullptr};
    QLabel* m_keep_operator_hint{nullptr};
    QPushButton* m_copy_record_button{nullptr};
    QPushButton* m_complete_release_button{nullptr};
    QLabel* m_next_steps{nullptr};
};

#endif // BITCOIN_QT_SHAREDMNCREATEDIALOG_H
