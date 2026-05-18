// Copyright (c) 2014-2025 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_QT_GUIUTIL_FONT_H
#define BITCOIN_QT_GUIUTIL_FONT_H

#include <QString>

#include <cstdint>
#include <utility>
#include <vector>

QT_BEGIN_NAMESPACE
class QFont;
class QTextEdit;
class QWidget;
QT_END_NAMESPACE

namespace GUIUtil {

enum class FontWeight : uint8_t {
    Normal,
    Bold,
};

/** Default values for the corresponding `-font-*` options (used in arg help
 *  text and as persistence fallbacks). */
int defaultFontScale();
int defaultFontSize();
QString defaultFontFamily();

/** Switch the active font family. Registers `font_name` if not yet known and
 *  applies the new font to qApp. Empty `font_name` means "use defaultFontFamily()".
 *  No-op if loadFonts() hasn't completed. Returns true on success. */
bool setActiveFont(const QString& font_name = {});
/** Currently active font family. */
QString activeFont();
/** Known fonts and their "selectable in UI" flag, in registration order. */
const std::vector<std::pair<QString, /*selectable=*/bool>>& knownFonts();

void setFontScale(int font_scale);
int fontScale();

/* Weight operations expressed in caller-friendly arg ints (0..8). This is the
 * format used by `-font-weight-*` CLI args and QSettings persistence. */

/** Current weight for `slot`, as arg int. */
int currentWeightArg(FontWeight slot);
/** Default-best-match weight for `slot`, as arg int. Valid before loadFonts() too. */
int defaultWeightArg(FontWeight slot);
/** Apply a weight from its arg int. Returns true on success; false if `arg` is
 *  out of 0..8 or maps to a weight not supported by the active font (no change
 *  to state in that case). */
bool setWeightFromArg(FontWeight slot, int arg);
/** Active font's supported weight args, in low-to-high order. */
std::vector<int> supportedWeightArgs();

/** Load dash specific application fonts */
bool loadFonts();

/** Check if the fonts have been loaded successfully */
bool fontsLoaded();

/** Set HTML content on a QTextEdit with font-aware styling. Captures the
 *  widget's base point size on first call so re-application on font/theme
 *  changes preserves it. Subsequent calls update the HTML. */
void setStyledHtml(QTextEdit* widget, const QString& html);

/** Set an application wide default font, depends on the selected theme */
void setApplicationFont();

/** Register `widgets` to receive the given font attributes on the next updateFonts() pass.
 *  Uses the currently active font family. */
void setFont(const std::vector<QWidget*>& widgets, FontWeight weight, double point_size = -1, bool is_italic = false);
/** Same as above, but with an explicit font family. */
void setFont(const std::vector<QWidget*>& widgets, const QString& font, FontWeight weight, double point_size = -1, bool is_italic = false);

/** Update the font of all widgets where a custom font has been set with
    GUIUtil::setFont */
void updateFonts();

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
