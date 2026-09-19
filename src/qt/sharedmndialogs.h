// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_QT_SHAREDMNDIALOGS_H
#define BITCOIN_QT_SHAREDMNDIALOGS_H

#include <qt/bitcoinunits.h>
#include <qt/masternodedialogs.h>
#include <qt/protxsender.h>

#include <consensus/amount.h>
#include <interfaces/node.h>

#include <QDialog>
#include <QString>
#include <QWidget>

#include <map>
#include <vector>

class BitcoinAmountField;
class SharedMnStatusBoard;
struct ProTxResult;

QT_BEGIN_NAMESPACE
class QCheckBox;
class QPlainTextEdit;
class QTableWidget;
class QTabWidget;
QT_END_NAMESPACE

//! Send a ProUpShareTx ("protx shared_update_share"): change the reward address
//! collateral share of a shared masternode. Only shares whose owner key this
//! wallet holds are offered; every other share field is immutable.
class UpdateShareDialog : public MasternodeActionDialog
{
    Q_OBJECT

    friend class MasternodeMaintenanceTests;
    friend class SharedMnWalkthroughTests;

public:
    UpdateShareDialog(interfaces::Node& node, WalletModel* wallet_model, const MasternodeEntry& entry, QWidget* parent = nullptr);

    //! Why `address` cannot be used as a share's reward address, or an empty
    //! string when it can. Mirrors the consensus payee-reuse rule: the reward
    //! destination may be neither the voting key's address nor any share
    //! owner's address.
    static QString RewardAddressProblem(const QString& address, const QString& voting_address,
                                        const std::vector<interfaces::MnShare>& shares);

protected:
    void validate() override;
    void submit() override;

private:
    //! Fill the reward field with the selected share's immutable refund
    //! address, the only way to point rewards back at it
    void useRefundAddress();
    //! Fill the reward field with a fresh address of this wallet
    void useNewAddress();

    const bool m_v24_active;
    const bool m_is_shared;
    const QString m_voting_address;
    const std::vector<interfaces::MnShare> m_shares;
    ProTxSender* m_sender{nullptr};

    QComboBox* m_share_combo{nullptr};
    QValidatedLineEdit* m_reward_edit{nullptr};
    QPushButton* m_use_new_button{nullptr};
    QPushButton* m_use_refund_button{nullptr};
    FeeSourcePicker* m_fee_source{nullptr};
};

//! Collects share owner signatures for a multi-party shared masternode
//! transaction (unanimous ProDisTx or ProUpSharedRegTx): shows the prepared
//! transaction, exports/imports a small session envelope
//! via clipboard and file, and verifies every imported signature natively
//! against the share owner keys (an unverifiable signature is never counted).
class SharedSigCollector : public QWidget
{
    Q_OBJECT

    friend class MasternodeMaintenanceTests;
    friend class SharedMnWalkthroughTests;

public:
    enum class Kind {
        Dissolve,  //!< unanimous ProDisTx from "protx shared_dissolve_prepare"
        Registrar, //!< ProUpSharedRegTx from "protx shared_update_registrar_prepare"
    };

    SharedSigCollector(Kind kind, const QString& pro_tx_hash, const std::vector<interfaces::MnShare>& shares,
                       WalletModel* wallet_model, QWidget* parent = nullptr);

    bool hasTransaction() const { return !m_tx_hex.isEmpty(); }
    const QString& txHex() const { return m_tx_hex; }
    //! Lowercase hex of the digest every share owner signs, recomputed natively
    //! from the adopted transaction (empty when no transaction is adopted)
    QString signHashHex() const;
    //! Adopt the prepared transaction (rejects a different transaction than
    //! the one already adopted; signatures collected so far are kept)
    bool setTransaction(const QString& tx_hex, QString& error);

    //! Verify and absorb an array of {shareIndex, signature} entries (the
    //! "protx shared_sign" result shape). Returns the number of newly absorbed
    //! signatures; `error` collects per-entry rejections.
    int addSignatures(const UniValue& entries, QString& error);

    int signedCount() const { return static_cast<int>(m_sigs.size()); }
    //! Human-checkable code of the envelope as it stands, so two participants
    //! can confirm over any channel that they hold the same message
    QString code() const;
    //! Import an envelope exactly as the Paste button does, and report the
    //! outcome as a code line under the board
    void importAndReport(const QString& text);
    //! Put the current envelope on the clipboard and show its code line
    void copyEnvelope();
    bool complete() const { return m_sigs.size() == m_shares.size(); }
    //! Signature entries in the shape "protx shared_combine" expects
    UniValue signaturesArray() const;

Q_SIGNALS:
    //! The transaction or the signature set changed
    void changed();

private Q_SLOTS:
    void saveEnvelope();
    void pasteEnvelope();
    void loadEnvelope();

private:
    UniValue envelopeJson() const;
    bool importEnvelope(const QString& text, QString& error);
    //! One line under the board naming the last message copied or received
    void showCodeLine(const QString& text);
    bool importEnvelopeChecked(const UniValue& json, QString& error);
    void refreshUi();
    void showStatus(const QString& message, bool error);

    const Kind m_kind;
    const QString m_protx_hash;
    const std::vector<interfaces::MnShare> m_shares;
    WalletModel* const m_wallet_model;
    QString m_tx_hex;
    std::map<int, QString> m_sigs; //!< shareIndex -> base64 signature

    SharedMnStatusBoard* m_board{nullptr};
    QLabel* m_status_label{nullptr};
    QPushButton* m_copy_button{nullptr};
    QPushButton* m_save_button{nullptr};
};

//! Scaffolding for the larger shared masternode dialogs that run several RPC
//! commands over their lifetime: wallet/watch-only gating and the unlock +
//! busy-state + ProTxSender round trip with the result returned to the caller.
class SharedMnDialog : public QDialog
{
    Q_OBJECT

public:
    //! Ignores Esc and window-close while a command runs in runCommand()'s
    //! nested event loop, so a flow cannot be dismissed mid-flight
    void reject() override;

protected:
    SharedMnDialog(interfaces::Node& node, WalletModel* wallet_model, const MasternodeEntry& entry, QWidget* parent);

    //! True when a wallet able to sign transactions is available
    bool canSign() const;
    //! Disable `button` with an explanatory tooltip when no signing wallet is
    //! available or the v24 hard fork is not active yet; returns true when the
    //! button stays usable
    bool gateButton(QPushButton* button) const;
    //! Unlock the wallet, execute `method` with named `params` on the RPC
    //! bridge and wait in a local event loop (the dialog is disabled
    //! meanwhile). Returns false with a user-displayable `error` when the
    //! command could not run or failed.
    bool runCommand(const QString& method, const UniValue& params, ProTxResult& result, QString& error);

    //! True when no input of `tx_hex` is positively spent. An input that is
    //! merely waiting for its confirmation (an unconfirmed own-wallet fee
    //! input) counts as unspent; a spent input makes the transaction
    //! permanently unbroadcastable: the session is dead and a new one must be
    //! started.
    bool txInputsUnspent(const QString& tx_hex, QString& error);

    //! Adopt a "..._prepare" result into `collector` and cross-check the
    //! locally computed signing digest against the node's; reports into
    //! `status` and returns false when the transaction must not be signed
    bool adoptPreparedTx(SharedSigCollector* collector, const ProTxResult& result, QLabel* status);
    //! "protx shared_sign" the collector's transaction with every share owner
    //! key this wallet holds and absorb the resulting signatures. False when
    //! nothing was absorbed, with the reason reported into `status`.
    bool signWithWallet(SharedSigCollector* collector, QLabel* status);
    //! "protx shared_combine" the fully signed transaction and submit it
    bool combineAndSubmit(SharedSigCollector* collector, QLabel* status, QLineEdit* result_edit,
                          const QString& success_text, const QString& failure_hint);
    //! A spent input makes the prepared transaction permanently
    //! unbroadcastable: the flow is dead and must be started over. Reports
    //! into `status` and returns false.
    bool sessionAlive(SharedSigCollector* collector, QLabel* status);
    //! True when this wallet holds at least one of the share owner keys
    bool holdsAnyShareKey() const;

    //! Combo box over the shares whose owner key this wallet can sign with:
    //! "Share k of n — amount — owner address"
    QComboBox* makeMyShareCombo(QWidget* parent);
    //! Transaction-fee input shared by every dissolution flow: 0.00001 to 0.01
    //! DASH, defaulting to 0.001 DASH, in the user's display unit
    BitcoinAmountField* makeFeeField(QWidget* parent) const;
    //! "Share k of n" for the 0-based `share_index` the RPCs use
    QString shareLabel(int share_index) const;
    //! Unit every amount on this dialog is rendered in
    BitcoinUnits::Unit displayUnit() const;

    interfaces::Node& m_node;
    WalletModel* const m_wallet_model;
    const QString m_protx_hash;
    const std::vector<interfaces::MnShare> m_shares;
    const CAmount m_early_penalty;
    const uint32_t m_early_period_blocks;
    const int m_registered_height;
    const bool m_v24_active;
    ProTxSender* m_sender{nullptr};

private:
    bool m_busy{false};
};

//! Dissolution of a shared masternode ("protx shared_dissolve" / "protx
//! shared_dissolve_prepare" + "protx shared_sign" + "protx shared_combine"):
//! - Dissolve now: unilateral, with a live penalty preview mirroring the real
//!   transaction's outputs;
//! - Unanimous: penalty-free at any height, needs every share owner's
//!   signature, collected via a SharedSigCollector envelope;
//! - Standby: generate and store both offline dissolution variants
//!   (payPenalty true/false) next to the refund key backup.
class DissolveDialog : public SharedMnDialog
{
    Q_OBJECT

    friend class MasternodeMaintenanceTests;
    friend class SharedMnWalkthroughTests;

public:
    DissolveDialog(interfaces::Node& node, WalletModel* wallet_model, const MasternodeEntry& entry, int current_height,
                   QWidget* parent = nullptr);


    //! Open on the standby tab, for callers that come from a "create a standby
    //! dissolution" entry point instead of the dissolution itself
    void selectStandbyTab();

    //! Import a "dash-shared-mn-sigs" dissolution envelope exactly as the
    //! Paste button does and show the "Dissolve Together" tab. Used when this
    //! dialog is opened by pasting a request somebody else prepared.
    void preloadEnvelope(const QString& text);

private Q_SLOTS:
    void updateNowPreview();
    void submitNow();
    void prepareTogether();
    void approveTogether();
    void sendTogether();

private:
    QWidget* buildNowTab();
    QWidget* buildTogetherTab();
    QWidget* buildStandbyTab();
    //! Build both standby variants and write them to one file the user picks
    void createStandby();
    //! Contents of the standby file: a header saying which transaction to
    //! broadcast when, then both of them
    QString standbyFileText() const;
    //! "standby-dissolution-<first 8 of proTxHash>-share<k>.txt"
    QString standbyFileName() const;
    bool writeStandbyFile(const QString& path, QString& error);
    //! Show the payouts of the adopted request, or hide the table when there
    //! is none
    void updateTogetherPayouts();
    //! Remember that a standby exists for this masternode, so the list can
    //! stop warning about it
    void recordStandbySaved();
    void refreshTogetherUi();

    //! Height captured at dialog open, refreshed from the tip in submitNow()
    int m_current_height;

    QTabWidget* m_tabs{nullptr};
    int m_standby_tab_index{-1};

    // Dissolve now
    QComboBox* m_now_actor{nullptr};
    BitcoinAmountField* m_now_fee{nullptr};
    QLabel* m_now_early_label{nullptr};
    QLabel* m_now_nudge_label{nullptr};
    QLabel* m_now_error_label{nullptr};
    QLabel* m_now_reason{nullptr};
    QTableWidget* m_now_table{nullptr};
    QCheckBox* m_now_accept{nullptr};
    QPushButton* m_now_submit{nullptr};
    QLineEdit* m_now_result{nullptr};
    QLabel* m_now_status{nullptr};

    // Dissolve together
    int m_together_tab_index{-1};
    //! True once this wallet ran the prepare: only then does it own the flow
    QComboBox* m_un_actor{nullptr};
    BitcoinAmountField* m_un_fee{nullptr};
    QPushButton* m_un_prepare{nullptr};
    QPushButton* m_un_approve{nullptr};
    QPushButton* m_un_send{nullptr};
    QLabel* m_un_reason{nullptr};
    //! What the adopted request actually pays out, so nobody approves a
    //! transaction on the strength of its title alone
    QLabel* m_un_payouts_title{nullptr};
    QTableWidget* m_un_table{nullptr};
    QLabel* m_un_fee_label{nullptr};
    SharedSigCollector* m_un_collector{nullptr};
    QLineEdit* m_un_result{nullptr};
    QLabel* m_un_status{nullptr};

    // Standby
    QComboBox* m_sb_actor{nullptr};
    BitcoinAmountField* m_sb_fee{nullptr};
    //! Share the generated standby transactions dissolve on behalf of
    int m_sb_share_index{-1};
    //! Full principal; inside the early period it only becomes valid at the
    //! penalty-free boundary, and stays valid forever after that
    QString m_sb_hex_full;
    //! Pays the early-exit penalty and is therefore valid immediately
    QString m_sb_hex_immediate;
    QPushButton* m_sb_create{nullptr};
    QPlainTextEdit* m_sb_full_view{nullptr};
    QPlainTextEdit* m_sb_immediate_view{nullptr};
    QLabel* m_sb_stored_label{nullptr};
    QLabel* m_sb_status{nullptr};
};

//! Rotate a shared masternode's operator and/or voting key ("protx
//! shared_update_registrar_prepare" + "protx shared_sign" + "protx
//! shared_combine"). The prepare and the final combine must run on the same
//! wallet, whose coins pay the fee; other participants open this dialog only
//! to import the envelope, sign and export.
class RotateSharedKeysDialog : public SharedMnDialog
{
    Q_OBJECT

    friend class MasternodeMaintenanceTests;
    friend class SharedMnWalkthroughTests;

public:
    RotateSharedKeysDialog(interfaces::Node& node, WalletModel* wallet_model, const MasternodeEntry& entry,
                           QWidget* parent = nullptr);

    //! Import a "dash-shared-mn-sigs" registrar envelope exactly as the Paste
    //! button does and move to the approvals card. Used when this dialog is
    //! opened by pasting a request somebody else prepared.
    void preloadEnvelope(const QString& text);

    //! True when `hex` is a usable basic-scheme BLS public key. 96 hex
    //! characters are not enough: the bytes must deserialize to a curve point,
    //! which is what the node checks.
    static bool IsValidOperatorKey(const QString& hex);

private Q_SLOTS:
    void validateForm();
    void prepare();
    void approveAndCopy();
    void sendRotation();

private:
    void refreshButtons();
    //! Read-only summary of what the adopted transaction changes, for a wallet
    //! that is approving somebody else's request
    void showRequestedChange();
    //! The operator key as typed, with the grouping spaces removed. The field
    //! shows a 96-character key in blocks so it can be read back and checked,
    //! and a key pasted from anywhere else that groups it is accepted too.
    QString operatorKeyInput() const;
    //! True when the adopted transaction spends only coins of this wallet, i.e.
    //! this is the wallet that prepared it. Combining re-signs those fee
    //! inputs, so only this wallet can send the result. It is a property of the
    //! transaction, not of this dialog instance: the request outlives the
    //! window, and the coordinator must be able to reopen it and finish.
    bool preparedByThisWallet() const;

    const QString m_current_operator_pubkey;
    const QString m_current_voting_address;

    QLineEdit* m_operator_edit{nullptr};
    QPushButton* m_operator_copy{nullptr};
    QValidatedLineEdit* m_voting_edit{nullptr};
    FeeSourcePicker* m_fee_source{nullptr};
    QLabel* m_pose_warning{nullptr};
    QLabel* m_requested_change{nullptr};
    QLabel* m_finish_banner{nullptr};
    QPushButton* m_prepare_button{nullptr};
    QPushButton* m_approve_button{nullptr};
    QPushButton* m_send_button{nullptr};
    QLabel* m_reason_label{nullptr};
    SharedSigCollector* m_collector{nullptr};
    QLineEdit* m_result_edit{nullptr};
    QLabel* m_status_label{nullptr};
};

#endif // BITCOIN_QT_SHAREDMNDIALOGS_H
