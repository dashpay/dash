// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_QT_PLATFORM_CREATEUSERNAMEWIZARD_H
#define BITCOIN_QT_PLATFORM_CREATEUSERNAMEWIZARD_H

#include <consensus/amount.h>

#include <QDialog>
#include <QWidget>

class PlatformService;
class WalletModel;

QT_BEGIN_NAMESPACE
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
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
    //! Whether the forward (Next/Finish) button may be enabled.
    virtual bool isComplete() const { return true; }

Q_SIGNALS:
    void completeChanged();
};

class UsernameEntryPage;
class UsernameCostPage;
class UsernameProgressPage;

/**
 * Guided flow for registering a username: name entry with live
 * availability + contested-name warnings, a cost confirmation, wallet
 * unlock, and a live progress view bound to the IdentityFlow state machine.
 * The wizard can be closed at any point after the funding step — the flow
 * continues headless in PlatformService and the dashboard shows progress.
 */
class CreateUsernameWizard : public QDialog
{
    Q_OBJECT

public:
    CreateUsernameWizard(PlatformService& service, WalletModel& wallet_model, QWidget* parent = nullptr);

    //! Open directly on the live progress page (used when a registration is
    //! already underway and the user asks to see how it is going).
    void startAtProgress();

private Q_SLOTS:
    void onBack();
    void onNext();
    void updateButtons();

private:
    UsernameWizardPage* currentPage() const;
    void setCurrentPage(int id, bool initialize = true);

    PlatformService& m_service;
    WalletModel& m_wallet_model;
    QStackedWidget* m_stack{nullptr};
    QPushButton* m_back{nullptr};
    QPushButton* m_next{nullptr};
    QPushButton* m_cancel{nullptr};
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
    bool isComplete() const override;

    QString username() const;
    bool contested() const { return m_contested; }

private Q_SLOTS:
    void onTextChanged();
    void onAvailability(const QString& normalized_label, bool available, bool contested);
    void onAvailabilityFailed(const QString& normalized_label, const QString& error);

private:
    PlatformService& m_service;
    QLineEdit* m_input{nullptr};
    QLabel* m_status{nullptr};
    QTimer* m_debounce{nullptr};
    bool m_available{false};
    bool m_contested{false};
    QString m_checked_normalized;
};

//! Page 2: cost confirmation + wallet unlock trigger.
class UsernameCostPage : public UsernameWizardPage
{
    Q_OBJECT

public:
    UsernameCostPage(PlatformService& service, WalletModel& wallet_model, const UsernameEntryPage& entry,
                     QWidget* parent = nullptr);
    void initializePage() override;
    bool validatePage() override;

    CAmount fundingAmount() const { return m_funding_amount; }

private:
    PlatformService& m_service;
    WalletModel& m_wallet_model;
    const UsernameEntryPage& m_entry;
    QLabel* m_summary{nullptr};
    QLabel* m_warning{nullptr};
    CAmount m_funding_amount{0};
};

//! Page 3: live progress bound to IdentityFlow.
class UsernameProgressPage : public UsernameWizardPage
{
    Q_OBJECT

public:
    UsernameProgressPage(PlatformService& service, QWidget* parent = nullptr);
    void initializePage() override;
    bool isComplete() const override;

private Q_SLOTS:
    void refresh();

private:
    PlatformService& m_service;
    QPlainTextEdit* m_log{nullptr};
    QLabel* m_headline{nullptr};
    QString m_last_headline;
    bool m_done{false};
};

#endif // BITCOIN_QT_PLATFORM_CREATEUSERNAMEWIZARD_H
