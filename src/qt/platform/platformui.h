// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_QT_PLATFORM_PLATFORMUI_H
#define BITCOIN_QT_PLATFORM_PLATFORMUI_H

#include <platform/types.h>
#include <qt/guiutil.h>

#include <QString>
#include <QWidget>

#include <cstdint>
#include <optional>

QT_BEGIN_NAMESPACE
class QLabel;
class QPlainTextEdit;
class QProgressBar;
class QPushButton;
QT_END_NAMESPACE

//! Building blocks the DashPay pages and dialogs share, so every surface
//! follows the masternode dialogs' button hierarchy, progress feedback and
//! message style, and says the same thing about the same failure.
namespace PlatformUi {
//! Point size of section headings and the dashboard's username, as the
//! Overview page's "Balances" and "Recent transactions".
inline constexpr double SECTION_HEADING_SIZE{16};

//! Paint `button` in the light secondary style (or, with secondary=false,
//! filled again): a view has one filled button.
void makeSecondary(QPushButton* button, bool secondary = true);
//! The masternode wizard's indeterminate progress bar, hidden until a wait starts.
QProgressBar* makeBusyBar(QWidget* parent);
//! Dim, word-wrapped explanation that follows theme changes (see restyle()).
QLabel* makeHint(const QString& text, QWidget* parent);
//! Colour a label's text with a theme style or colour. Stylesheets set from
//! code keep the theme they were computed for, so restyle() re-applies them.
void setTextStyle(QLabel* label, GUIUtil::ThemedStyle style);
void setTextColor(QLabel* label, GUIUtil::ThemedColor color);
//! Re-apply every setTextStyle()/setTextColor() below `root` after a theme change.
void restyle(QWidget* root);

//! Where a failed Platform call came from: some consensus codes mean
//! something else, and ask for another next step, depending on the action.
enum class Context {
    READ,
    SEARCH,
    NAME_CHECK,
    IDENTITY_CREATE,
    NAME_REGISTER,
    PROFILE,
    CONTACT_REQUEST,
    CONTACT_ACCEPT,
    PAYMENT_LOOKUP,
};
//! A failure as the user reads it: one or two plain sentences, and the raw
//! result for "Show details" (empty for a failure without a Status).
struct UserError {
    QString text;
    QString details;
};
//! When a REJECTED status is the SDK refusing a response whose block time
//! trails the local clock beyond its tolerance, the block time it carried
//! (unix milliseconds); nullopt otherwise. Every node answering with the
//! same old block time means Platform itself has stopped producing blocks.
std::optional<int64_t> StaleBlockTimeMs(const platform::Status& status);
//! Plain-language text for a failed client call. `operation` names the step
//! in the details; `subject` is the username or contact some texts mention.
//! Empty for the kinds that are not failures (OK, PROVEN_ABSENT, ALREADY_EXISTS).
UserError Describe(const platform::Status& status, Context context, const QString& operation, const QString& subject = {});
//! The "Show details" text of a failed step: its operation, the raw result of
//! the client call when one failed, and when it happened (unix seconds).
QString Details(const platform::Status* status, const QString& operation, int64_t time);

/** One message line: an icon and a sentence coloured by severity, an
 *  optional secondary action (Retry, Try again), and a "Show details"
 *  disclosure with the raw result and a Copy details button. Hidden while
 *  empty. */
class MessageLine : public QWidget
{
    Q_OBJECT

public:
    enum class Severity {
        Info,
        Success,
        Attention,
        Error,
    };

    explicit MessageLine(QWidget* parent = nullptr);

    void setMessage(Severity severity, const QString& text, const QString& details = {});
    void setMessage(Severity severity, const UserError& error) { setMessage(severity, error.text, error.details); }
    //! A message that stops being true soon ("Profile saved."): cleared after
    //! a few seconds unless another message replaced it.
    void setTransientMessage(Severity severity, const QString& text);
    //! Show a secondary button after the text; an empty label hides it.
    void setAction(const QString& label);
    void clear();
    QString text() const;

Q_SIGNALS:
    void actionClicked();

protected:
    void changeEvent(QEvent* event) override;

private:
    void applySeverity();

    Severity m_severity{Severity::Info};
    QLabel* m_icon{nullptr};
    QLabel* m_text{nullptr};
    QPushButton* m_action{nullptr};
    QPushButton* m_toggle{nullptr};
    QWidget* m_details{nullptr};
    QPlainTextEdit* m_details_text{nullptr};
};
} // namespace PlatformUi

#endif // BITCOIN_QT_PLATFORM_PLATFORMUI_H
