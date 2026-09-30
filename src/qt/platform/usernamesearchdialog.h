// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_QT_PLATFORM_USERNAMESEARCHDIALOG_H
#define BITCOIN_QT_PLATFORM_USERNAMESEARCHDIALOG_H

#include <QDialog>
#include <QPair>
#include <QString>
#include <QVector>

class PlatformService;
namespace PlatformUi {
class MessageLine;
} // namespace PlatformUi

QT_BEGIN_NAMESPACE
class QLabel;
class QLineEdit;
class QProgressBar;
class QPushButton;
class QTableWidget;
class QTimer;
QT_END_NAMESPACE

/** Add a contact: look a DashPay username up and send that person a contact
 *  request. Nothing is looked up before three characters are typed. */
class UsernameSearchDialog : public QDialog
{
    Q_OBJECT

public:
    explicit UsernameSearchDialog(PlatformService& service, QWidget* parent = nullptr);

protected:
    void changeEvent(QEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;

private Q_SLOTS:
    void onTextChanged();
    void onResults(const QString& prefix, const QVector<QPair<QString, QString>>& results);
    void onSearchFailed(const QString& prefix, const QString& error, const QString& details);
    void addSelected();
    void updateActions();
    void onContactRequestPending(const QString& id);
    void onContactRequestFinished(const QString& id, bool ok, const QString& error, const QString& details);

private:
    //! No request can be sent to this identity (one was sent, or it has no
    //! key to receive one): its row says why and can't be chosen.
    void markUnavailable(const QString& id, const QString& note);
    //! The Note column shows only when a row has a note.
    void updateNoteColumn();
    //! The dialog takes input again after a send.
    void finishSending();
    //! Why no request can be sent to this identity ("This is you", "Request
    //! sent", "Can't receive contact requests", …); empty when one can.
    QString noteFor(const QString& identity_hex) const;
    void setBusy(bool busy);

    PlatformService& m_service;
    QLineEdit* m_input{nullptr};
    QTableWidget* m_table{nullptr};
    QProgressBar* m_busy{nullptr};
    QLabel* m_footer{nullptr};
    PlatformUi::MessageLine* m_status{nullptr};
    QPushButton* m_add{nullptr};
    QTimer* m_debounce{nullptr};
    //! The request being sent, then the one Platform is confirming.
    QString m_pending_identity;
    QString m_confirming_identity;
    bool m_searching{false};
};

#endif // BITCOIN_QT_PLATFORM_USERNAMESEARCHDIALOG_H
