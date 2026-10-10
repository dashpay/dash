// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_QT_PLATFORM_PLATFORMUI_H
#define BITCOIN_QT_PLATFORM_PLATFORMUI_H

#include <platform/types.h>
#include <platform/walletrecords.h>
#include <qt/bitcoinunits.h>
#include <qt/guiutil.h>

#include <QColor>
#include <QPixmap>
#include <QString>
#include <QStringList>
#include <QWidget>

#include <array>
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

//! The fills avatars are drawn with, derived from the active theme's brand
//! colours (blue, green, orange): no purple, whatever the name hashes to.
std::array<QColor, 6> avatarPalette();
//! Round avatar: a palette fill picked by the normalized username, with the
//! first letter of the display name (or of the username). Without a
//! username it is a neutral disc without a letter.
QPixmap avatarPixmap(const QString& username, const QString& display_name, int size, qreal dpr);
//! Numbered step badge of the welcome page: a filled brand-blue disc.
QPixmap stepBadgePixmap(int number, int size, qreal dpr);

//! The steps of a registration, in order: four for a new identity, two when
//! an existing identity only registers a username.
QStringList registrationSteps(const platform::IdentityRecord& record);
//! Index into registrationSteps() of the step the record is at (or failed
//! or waits at); the step count once the name is registered or submitted to
//! a vote; -1 when no registration runs.
int registrationStep(const platform::IdentityRecord& record);
//! "Step 2 of 4: Creating your identity", or empty.
QString registrationStepLine(const platform::IdentityRecord& record);
//! A balance on Dash Platform (in credits) as a Dash amount in `unit`,
//! rounded down to a duff so it never overstates.
QString formatPlatformBalance(BitcoinUnit unit, uint64_t credits);

//! What a failed registration left behind: the identity and its balance, the
//! funding payment, or nothing spent.
QString failureReassurance(const platform::IdentityRecord& record);

//! An identity id the way every other Dash Platform tool shows it (Base58).
QString identityIdBase58(const platform::Identifier& id);
//! identityIdBase58() of a hex identity id; empty when `hex` is not one.
QString identityIdBase58(const QString& hex);

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
    //! Leave out the severity icon, for a line under a title that has one.
    void setIconShown(bool shown);
    void clear();
    QString text() const;

Q_SIGNALS:
    void actionClicked();

protected:
    void changeEvent(QEvent* event) override;

private:
    void applySeverity();

    Severity m_severity{Severity::Info};
    bool m_icon_shown{true};
    QLabel* m_icon{nullptr};
    QLabel* m_text{nullptr};
    QPushButton* m_action{nullptr};
    QPushButton* m_toggle{nullptr};
    QWidget* m_details{nullptr};
    QPlainTextEdit* m_details_text{nullptr};
};
} // namespace PlatformUi

#endif // BITCOIN_QT_PLATFORM_PLATFORMUI_H
