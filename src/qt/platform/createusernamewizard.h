// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_QT_PLATFORM_CREATEUSERNAMEWIZARD_H
#define BITCOIN_QT_PLATFORM_CREATEUSERNAMEWIZARD_H

#include <consensus/amount.h>

#include <QDialog>
#include <QList>
#include <QPair>
#include <QWidget>

class PlatformService;
class WalletModel;
namespace PlatformUi {
class MessageLine;
} // namespace PlatformUi

QT_BEGIN_NAMESPACE
class QFormLayout;
class QFrame;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QProgressBar;
class QPushButton;
class QShowEvent;
class QStackedWidget;
class QTimer;
QT_END_NAMESPACE

//! Base for the registration flow's pages: QWizardPage-like hooks without
//! QWizard, which release builds do not have (depends Qt is configured with
//! -no-feature-wizard).
class UsernameWizardPage : public QWidget
{
    Q_OBJECT

public:
    using QWidget::QWidget;

    //! Called each time the page becomes current through forward navigation.
    virtual void initializePage() {}
    //! Gate for leaving the page forward; return false to stay.
    virtual bool validatePage() { return true; }
    //! Whether the forward (Next/Register) button may be enabled.
    virtual bool isComplete() const { return true; }
    //! A network wait of the page runs (the wizard shows its busy bar).
    virtual bool isBusy() const { return false; }

Q_SIGNALS:
    void completeChanged();
};

class UsernameEntryPage;
class UsernameCostPage;
class UsernameProgressPage;

/**
 * Guided flow for registering a username: name entry with live
 * availability + premium-name warnings, a cost confirmation, and a live
 * progress view bound to the IdentityFlow state machine. The wizard can be
 * closed at any point after the funding step; the flow continues headless
 * in PlatformService and the dashboard shows progress.
 */
class CreateUsernameWizard : public QDialog
{
    Q_OBJECT

public:
    CreateUsernameWizard(PlatformService& service, WalletModel& wallet_model, QWidget* parent = nullptr);

    //! Open directly on the live progress page (used when a registration is
    //! already underway and the user asks to see how it is going).
    void startAtProgress();

Q_SIGNALS:
    //! "Add profile…" on the finished registration.
    void addProfileRequested();

protected:
    void changeEvent(QEvent* event) override;
    bool event(QEvent* event) override;
    void showEvent(QShowEvent* event) override;

private Q_SLOTS:
    void onBack();
    void onNext();
    void updateButtons();

private:
    UsernameWizardPage* currentPage() const;
    void setCurrentPage(int id, bool initialize = true);

    PlatformService& m_service;
    QLabel* m_step_hint{nullptr};
    QStackedWidget* m_stack{nullptr};
    QProgressBar* m_busy{nullptr};
    QPushButton* m_back{nullptr};
    QPushButton* m_cancel{nullptr};
    QPushButton* m_close{nullptr};
    QPushButton* m_secondary{nullptr};
    QPushButton* m_next{nullptr};
    UsernameEntryPage* m_entry{nullptr};
    UsernameCostPage* m_cost{nullptr};
    UsernameProgressPage* m_progress{nullptr};
};

//! Page 1: username entry + debounced availability check.
class UsernameEntryPage : public UsernameWizardPage
{
    Q_OBJECT

public:
    UsernameEntryPage(PlatformService& service, QWidget* parent = nullptr);
    void initializePage() override;
    bool isComplete() const override;
    bool isBusy() const override { return m_checking; }
    //! A name that is already ours starts the flow here, which confirms it.
    bool validatePage() override;

    QString username() const;
    //! The DashPay display name to publish with the registration; empty
    //! when none was given or the identity exists already.
    QString displayName() const;
    bool contested() const { return m_contested; }
    //! The name is registered to this wallet's identity already.
    bool ours() const { return m_ours; }

private Q_SLOTS:
    void onTextChanged();
    void onAvailability(const QString& normalized_label, bool available, bool contested);
    void onAvailabilityFailed(const QString& normalized_label, const QString& error, const QString& details);
    void onNameIsOurs(const QString& normalized_label);

private:
    //! Whether an answer about `normalized_label` is about what is typed now.
    bool isCurrent(const QString& normalized_label) const;
    void setChecking(bool checking);

    PlatformService& m_service;
    QLineEdit* m_input{nullptr};
    //! The username rule, hidden while the error stating it is shown.
    QLabel* m_rule{nullptr};
    PlatformUi::MessageLine* m_status{nullptr};
    QLabel* m_normalized{nullptr};
    QWidget* m_profile{nullptr};
    QLineEdit* m_display_name{nullptr};
    QTimer* m_debounce{nullptr};
    bool m_available{false};
    bool m_ours{false};
    bool m_contested{false};
    bool m_checking{false};
    QString m_checked_normalized;
};

//! Page 2: cost confirmation; leaving it starts the registration.
class UsernameCostPage : public UsernameWizardPage
{
    Q_OBJECT

public:
    UsernameCostPage(PlatformService& service, WalletModel& wallet_model, const UsernameEntryPage& entry,
                     QWidget* parent = nullptr);
    void initializePage() override;
    bool validatePage() override;
    bool isComplete() const override { return m_ready; }
    bool isBusy() const override { return m_awaiting_balance; }

private Q_SLOTS:
    void onIdentityBalance(quint64 credits);

private:
    QString formatBalance(uint64_t credits) const;
    QString formatAmount(CAmount amount) const;

    PlatformService& m_service;
    WalletModel& m_wallet_model;
    const UsernameEntryPage& m_entry;
    QFormLayout* m_form{nullptr};
    //! Every row the summary can show, in order: (label, value).
    QList<QPair<QLabel*, QWidget*>> m_rows;
    QLabel* m_username{nullptr};
    QLabel* m_normalized{nullptr};
    QLabel* m_display_name{nullptr};
    QLabel* m_wallet{nullptr};
    QLabel* m_cost{nullptr};
    QLabel* m_explanation{nullptr};
    QFrame* m_premium{nullptr};
    PlatformUi::MessageLine* m_premium_text{nullptr};
    PlatformUi::MessageLine* m_error{nullptr};
    //! The asset lock a new identity is funded with; 0 when the identity
    //! exists and its balance on Dash Platform pays.
    CAmount m_funding_amount{0};
    bool m_ready{false};
    //! A premium name for an existing identity waits for its proved balance.
    bool m_awaiting_balance{false};
};

//! Page 3: the registration steps as a checklist, bound to IdentityFlow.
class UsernameProgressPage : public UsernameWizardPage
{
    Q_OBJECT

public:
    explicit UsernameProgressPage(PlatformService& service, QWidget* parent = nullptr);
    void initializePage() override;
    bool isBusy() const override;

protected:
    void changeEvent(QEvent* event) override;

private Q_SLOTS:
    void refresh();

private:
    void appendLog(const QString& line);

    PlatformService& m_service;
    QLabel* m_title{nullptr};
    QLabel* m_subtitle{nullptr};
    QList<QLabel*> m_step_icons;
    QList<QLabel*> m_step_labels;
    PlatformUi::MessageLine* m_error{nullptr};
    QPushButton* m_log_toggle{nullptr};
    QPlainTextEdit* m_log{nullptr};
    //! The furthest step logged, so the log records each step once, and the
    //! last line, so a repeated one is not logged again.
    int m_logged_step{-1};
    QString m_last_logged;
    //! The registration failed: the step it resumes at is logged again.
    bool m_retrying{false};
};

#endif // BITCOIN_QT_PLATFORM_CREATEUSERNAMEWIZARD_H
