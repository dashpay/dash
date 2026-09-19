// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_QT_MASTERNODEWIDGETS_H
#define BITCOIN_QT_MASTERNODEWIDGETS_H

#include <consensus/amount.h>
#include <primitives/transaction.h>
#include <support/allocators/secure.h>
#include <span.h>

#include <QComboBox>
#include <QString>
#include <QStringList>
#include <QWidget>

#include <optional>
#include <vector>

class QValidatedLineEdit;
class WalletModel;

QT_BEGIN_NAMESPACE
class QFrame;
class QLabel;
class QRadioButton;
class QVBoxLayout;
QT_END_NAMESPACE

//! Free validation and layout helpers shared by the masternode management dialogs
namespace MasternodeWidgetUtil {
//! Tokenize a comma/whitespace-separated endpoint list. Syntax, purpose,
//! routability, port, duplicate and cardinality validation belongs to the
//! typed provider service so the GUI and transaction builder use one ruleset.
QStringList tokenizeEndpointList(const QString& input);
//! True when `address` decodes to a P2PKH destination on the current network
bool isP2PKHAddress(const QString& address);
//! True when `address` decodes to a P2PKH or P2SH destination on the current network
bool isP2PKHorP2SHAddress(const QString& address);

//! Spacing tokens (px) keeping one vertical rhythm across the dialogs' pages
constexpr int GROUP_SPACING{16}; //!< between top-level groups of a page
constexpr int TITLE_SPACING{8};  //!< between a group's title and its body
constexpr int ROW_SPACING{10};   //!< between rows inside a group
constexpr int BODY_INDENT{26};   //!< indent of a body under its radio header
constexpr int CARD_PADDING{12};  //!< a card's internal padding

//! Bold label. A negative `point_size` keeps the theme's own size.
QLabel* makeTitle(const QString& text, QWidget* parent, double point_size = -1);
//! Dim, word-wrapped explanation
QLabel* makeHint(const QString& text, QWidget* parent);
//! Word-wrapped, selectable plain-text value; `monospace` for addresses, hashes and keys
QLabel* makeValue(const QString& text, QWidget* parent, bool monospace = false);
//! Group `text` into blocks of `chunk_size` characters separated by spaces. A BLS
//! key is one 96-character word that a wrapping label refuses to break; grouped,
//! it wraps between blocks and stays readable. Inline because the masternode
//! model, which is built without wallet support too, formats keys with it.
inline QString chunked(const QString& text, int chunk_size = 12)
{
    if (chunk_size <= 0) return text;
    QString ret;
    ret.reserve(text.size() + text.size() / chunk_size);
    for (int pos = 0; pos < text.size(); pos += chunk_size) {
        if (pos > 0) ret += QLatin1Char(' ');
        ret += text.mid(pos, chunk_size);
    }
    return ret;
}
//! Monospace value with a Copy button beside it. `display` is shown as given,
//! `copy_text` is what the button puts on the clipboard, so a key can be shown
//! in chunks and still be copied unbroken.
QWidget* makeCopyableValue(const QString& display, const QString& copy_text, QWidget* parent);
//! Frame grouping one choice or one section, named "mnCard" for theming
QFrame* makeCard(QWidget* parent);

//! A card presenting one choice: `header` (the radio button) sits on top, the
//! optional `hint` below it, and `body` holds the controls that only matter
//! while the choice is selected, so it can be hidden to collapse the card.
struct OptionCard {
    QFrame* card;
    QWidget* body;
    QVBoxLayout* body_layout;
};
//! Build an option card. `header` is re-parented into the card, so radio
//! buttons of one page need a QButtonGroup to stay mutually exclusive.
OptionCard makeOptionCard(QWidget* parent, QWidget* header, const QString& hint = QString());
} // namespace MasternodeWidgetUtil

//! Combo box listing the wallet's addresses with their spendable balance,
//! largest first. Used to pick the fee source (or funding source) of a protx
//! operation, defaulting to the address most likely able to pay.
class FeeSourcePicker : public QComboBox
{
    Q_OBJECT

public:
    explicit FeeSourcePicker(QWidget* parent = nullptr);

    void setWalletModel(WalletModel* wallet_model);
    //! Add an explicit first option which leaves fee-source selection to Core.
    //! An empty label removes it. Registration flows do not enable this option.
    void setAutomaticOption(const QString& label);
    //! Hide addresses whose spendable balance is below `minimum` (default 0 = show all)
    void setMinimumBalance(CAmount minimum);
    //! Omit a collateral outpoint that the registration operation will lock
    //! before funding its fee transaction.
    void setExcludedOutpoint(std::optional<COutPoint> outpoint);
    //! Re-scan the wallet's coins and rebuild the list
    void refresh();

    QString selectedAddress() const;

private:
    WalletModel* m_wallet_model{nullptr};
    CAmount m_minimum_balance{0};
    QString m_automatic_label;
    std::optional<COutPoint> m_excluded_outpoint;
};

//! Operator key selection: generate a fresh basic-scheme BLS key pair or paste
//! an existing operator public key. A generated secret is kept only in memory
//! here; the owning dialog is responsible for showing it to the user once.
class OperatorKeyWidget : public QWidget
{
    Q_OBJECT

public:
    //! Where the operator key comes from and what happens to its secret
    enum class Mode {
        GenerateOnly, //!< generated here; the secret exists only in this dialog
        Existing,     //!< an operator public key supplied by the user
    };

    explicit OperatorKeyWidget(QWidget* parent = nullptr);
    ~OperatorKeyWidget() override;

    //! Currently selected mode
    Mode mode() const;
    //! Selected operator public key in basic-scheme hex (empty when invalid)
    QString publicKeyHex() const;
    //! Generated secret key hex; empty when an existing public key is used. The
    //! returned QString is a transient display copy; callers must not retain it.
    QString secretHex() const;
    //! True when this widget holds the generated operator secret rather than
    //! only the public key of a secret somebody else keeps
    bool hasGeneratedSecret() const;
    //! True when publicKeyHex() returns a usable key
    bool isValid() const;

Q_SIGNALS:
    void changed();

private Q_SLOTS:
    void updateState();

private:
    static QString secretDisplay(Span<const unsigned char> secret);
    static void clearSecret(SecureVector& secret);
    QRadioButton* m_generate_radio{nullptr};
    QRadioButton* m_existing_radio{nullptr};
    QWidget* m_generate_body{nullptr};
    QWidget* m_existing_body{nullptr};
    QValidatedLineEdit* m_existing_edit{nullptr};
    SecureVector m_generated_secret;
    QString m_generated_public;
};

#endif // BITCOIN_QT_MASTERNODEWIDGETS_H
