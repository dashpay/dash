// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_QT_PLATFORM_CONTACTSPAGE_H
#define BITCOIN_QT_PLATFORM_CONTACTSPAGE_H

#include <QString>
#include <QWidget>

#include <cstddef>
#include <cstdint>

class ContactsModel;
class PlatformService;
namespace PlatformUi {
class MessageLine;
} // namespace PlatformUi

QT_BEGIN_NAMESPACE
class QCheckBox;
class QLabel;
class QPoint;
class QProgressBar;
class QPushButton;
class QShowEvent;
class QTableView;
class QTimer;
QT_END_NAMESPACE

/** Contacts section of the dashboard: established contacts and pending
 *  requests, with the action the selected row allows (accept, finish,
 *  ignore) and Add contact in the section's header. The list reads itself
 *  again while it is shown: on a new ChainLock (at most every minute), on
 *  a slow timer, and after the user's own changes, backing off while reads
 *  fail; only a failure offers Try again. Paying a contact starts on the
 *  Send tab. */
class ContactsPage : public QWidget
{
    Q_OBJECT

public:
    explicit ContactsPage(PlatformService& service, QWidget* parent = nullptr);

    //! No rows at all (the dashboard hides the section before registration).
    bool isEmpty() const;
    //! The "No contacts yet" state with its own Add contact button shows.
    bool showsEmptyState() const;
    //! Read the list now (the user changed it).
    void refresh();
    //! refresh() for the dashboard being shown, unless reads are failing and
    //! the backoff says to wait.
    void refreshIfShown();
    //! What takes the focus when the dashboard opens on the list: the empty
    //! state's Add contact…, otherwise the table.
    QWidget* focusTarget() const;

Q_SIGNALS:
    //! isEmpty() or showsEmptyState() may have changed.
    void emptyChanged();

protected:
    void changeEvent(QEvent* event) override;
    void showEvent(QShowEvent* event) override;

private Q_SLOTS:
    //! refresh() when an automatic read is due and the list is shown.
    void refreshIfDue();
    void addContact();
    void acceptSelected();
    void setSelectedHidden(bool hidden);
    void activateSelected();
    void updateActions();
    void showContextMenu(const QPoint& pos);
    void onContactRequestPending(const QString& id);
    void onContactRequestFinished(const QString& id, bool ok, const QString& error, const QString& details);

private:
    //! Size the table to its rows and its columns to their content.
    void fitTable();
    //! The selected row survives a model reset (every refresh).
    void rememberSelection();
    void restoreSelection();
    //! "Last updated at …" while the list may be out of date.
    void updateStaleness();

    PlatformService& m_service;
    ContactsModel* m_model{nullptr};
    QLabel* m_attention{nullptr};
    QCheckBox* m_show_ignored{nullptr};
    QTableView* m_view{nullptr};
    QWidget* m_empty{nullptr};
    QWidget* m_loading{nullptr};
    QLabel* m_updated{nullptr};
    QPushButton* m_primary_button{nullptr};
    QPushButton* m_ignore_button{nullptr};
    QPushButton* m_add_button{nullptr};
    QPushButton* m_empty_add_button{nullptr};
    QTimer* m_refresh_timer{nullptr};
    QProgressBar* m_busy{nullptr};
    PlatformUi::MessageLine* m_status{nullptr};
    //! The request being accepted, then the reply Platform is confirming.
    QString m_accepting_identity;
    QString m_confirming_identity;
    QString m_selected_identity;
    //! The status line shows a refresh failure, cleared by the next success.
    bool m_refresh_failed{false};
    //! isEmpty() when emptyChanged() was last considered.
    bool m_was_empty{true};
    //! The first waiting request was selected since the list was shown.
    bool m_auto_selected{false};
    //! No automatic read before this time (GetTime()).
    int64_t m_next_refresh{0};
    //! When the list was last read successfully (GetTime()); 0 before.
    int64_t m_last_success{0};
    //! Reads that failed in a row.
    size_t m_failures{0};
};

#endif // BITCOIN_QT_PLATFORM_CONTACTSPAGE_H
