// Copyright (c) 2014-2025 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_QT_GUIUTIL_FONT_H
#define BITCOIN_QT_GUIUTIL_FONT_H

#include <QFont>
#include <QString>

#include <cstdint>
#include <utility>
#include <vector>

QT_BEGIN_NAMESPACE
class QTextEdit;
class QWidget;
QT_END_NAMESPACE

namespace GUIUtil {

enum class FontWeight : uint8_t {
    Normal,
    Bold,
};

struct FontAttrib {
    QString m_font;
    FontWeight m_weight_type;
    double m_point_size{-1};
    bool m_is_italic{false};

    FontAttrib(QString font, FontWeight weight_type, double point_size = -1, bool is_italic = false);
    // cppcheck-suppress noExplicitConstructor
    FontAttrib(FontWeight weight_type, double point_size = -1, bool is_italic = false);
    ~FontAttrib();
};

/** Default values for the corresponding `-font-*` options (used in arg help
 *  text and as persistence fallbacks). */
int defaultFontScale();
int defaultFontSize();
QString defaultFontFamily();

/** Register a font name as known. If selectable, it shows up in the appearance
 *  picker. `skip_checks` bypasses the QFontDatabase availability test — only
 *  legal right after QFontDatabase::addApplicationFont. */
[[nodiscard]] bool registerFont(const QString& font, bool selectable, bool skip_checks = false);
/** Switch the active font family. The family must have been registered. */
[[nodiscard]] bool setActiveFont(const QString& font);
/** Currently active font family. */
QString activeFont();
/** Known fonts and their "selectable in UI" flag, in registration order. */
const std::vector<std::pair<QString, /*selectable=*/bool>>& knownFonts();

void setFontScale(int font_scale);
int fontScale();

/* Weight operations expressed in caller-friendly arg ints (0..8). This is the
 * format used by `-font-weight-*` CLI args and QSettings persistence. */

/** True if `arg` (0..8) maps to a weight supported by the active font. */
bool isValidWeightArg(int arg);
/** Current weight for `slot`, as arg int. */
int currentWeightArg(FontWeight slot);
/** Default-best-match weight for `slot`, as arg int. Valid before loadFonts() too. */
int defaultWeightArg(FontWeight slot);
/** Apply a weight from its arg int. No-op if `arg` is out of 0..8. */
void setWeightFromArg(FontWeight slot, int arg);
/** Active font's supported weight args, in low-to-high order. */
std::vector<int> supportedWeightArgs();

/** Load dash specific application fonts */
bool loadFonts();

/** Check if the fonts have been loaded successfully */
bool fontsLoaded();

/** Register a QTextEdit for font styling. Applies immediately and updates when fonts change. */
void registerWidget(QTextEdit* widget, const QString& html);

/** Set an application wide default font, depends on the selected theme */
void setApplicationFont();

/** Workaround to set correct font styles in all themes since there is a bug in macOS which leads to
    issues loading variations of montserrat in css it also keeps track of the set fonts to update on
    theme changes. */
void setFont(const std::vector<QWidget*>& vecWidgets, const FontAttrib& font_attrib);

/** Update the font of all widgets where a custom font has been set with
    GUIUtil::setFont */
void updateFonts();

/** Get list of all selectable fonts */
std::vector<QString> getFonts(bool selectable_only);

/** Get the default bold QFont */
QFont getFontBold();

/** Get the default normal QFont */
QFont getFontNormal();

/** Get a scaled font with the specified base size, weight, and optional multiplier. */
QFont getScaledFont(double baseSize, bool bold, double multiplier = 1);

/** (Bitcoin) Return a monospace font */
QFont fixedPitchFont(bool use_embedded_font = false);
} // namespace GUIUtil

#endif // BITCOIN_QT_GUIUTIL_FONT_H
