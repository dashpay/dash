// Copyright (c) 2011-2014 The Bitcoin Core developers
// Copyright (c) 2014-2023 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/bitcoinaddressvalidator.h>
#include <qt/guiutil.h>

#include <key_io.h>

/* Base58 characters are:
     "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz"

  This is:
  - All numbers except for '0'
  - All upper-case letters except for 'I' and 'O'
  - All lower-case letters except for 'l'
*/

BitcoinAddressEntryValidator::BitcoinAddressEntryValidator(QObject *parent, bool fAllowURI) :
    QValidator(parent), fAllowURI(fAllowURI)
{
}

DashPayRecipientEntryValidator::DashPayRecipientEntryValidator(QObject* parent, bool allow_uri) :
    QValidator(parent),
    m_address_validator(nullptr, allow_uri)
{
}

QValidator::State DashPayRecipientEntryValidator::validate(QString& input, int& pos) const
{
    const State address_state = m_address_validator.validate(input, pos);
    if (address_state != Invalid) return address_state;

    // DPNS labels accept ASCII letters, digits, and interior hyphens. Keep
    // incomplete labels editable, but reject impossible candidates early.
    if (input.size() > 63 || input.startsWith('-')) return Invalid;
    for (const QChar ch : input) {
        const ushort c = ch.unicode();
        if (!((c >= '0' && c <= '9') ||
              (c >= 'a' && c <= 'z') ||
              (c >= 'A' && c <= 'Z') || c == '-')) {
            return Invalid;
        }
    }

    if (input.size() < 3 || input.endsWith('-')) return Intermediate;
    return Acceptable;
}

QValidator::State BitcoinAddressEntryValidator::validate(QString &input, int &pos) const
{
    Q_UNUSED(pos);

    // Empty address is "intermediate" input
    if (input.isEmpty())
        return QValidator::Intermediate;

    if (fAllowURI && GUIUtil::validateBitcoinURI(input)) {
        return QValidator::Acceptable;
    }

    // Correction
    for (int idx = 0; idx < input.size();)
    {
        bool removeChar = false;
        QChar ch = input.at(idx);
        // Corrections made are very conservative on purpose, to avoid
        // users unexpectedly getting away with typos that would normally
        // be detected, and thus sending to the wrong address.
        switch(ch.unicode())
        {
        // Qt categorizes these as "Other_Format" not "Separator_Space"
        case 0x200B: // ZERO WIDTH SPACE
        case 0xFEFF: // ZERO WIDTH NO-BREAK SPACE
            removeChar = true;
            break;
        default:
            break;
        }

        // Remove whitespace
        if (ch.isSpace())
            removeChar = true;

        // To next character
        if (removeChar)
            input.remove(idx, 1);
        else
            ++idx;
    }

    // Validation
    QValidator::State state = QValidator::Acceptable;
    for (int idx = 0; idx < input.size(); ++idx)
    {
        int ch = input.at(idx).unicode();

        if (((ch >= '0' && ch<='9') ||
            (ch >= 'a' && ch<='z') ||
            (ch >= 'A' && ch<='Z')) &&
            ch != 'l' && ch != 'I' && ch != '0' && ch != 'O')
        {
            // Alphanumeric and not a 'forbidden' character
        }
        else
        {
            state = QValidator::Invalid;
        }
    }

    return state;
}

BitcoinAddressCheckValidator::BitcoinAddressCheckValidator(QObject *parent) :
    QValidator(parent)
{
}

QValidator::State BitcoinAddressCheckValidator::validate(QString &input, int &pos) const
{
    Q_UNUSED(pos);
    // Validate the passed Dash address
    if (IsValidDestinationString(input.toStdString())) {
        return QValidator::Acceptable;
    }

    return QValidator::Invalid;
}
