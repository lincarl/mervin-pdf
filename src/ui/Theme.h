#pragma once

#include <QColor>
#include <QString>

class QAbstractSpinBox;
class QPalette;

namespace mervin {

// Application-wide QSS, including top-level dialogs, with colours from ThemeTokens. Each scheme
// installs its palette (Nord in dark, Cool slate in light) for both QSS and palette-based
// painters, carrying the resolved accent: a custom one, or for "system" the OS accent (the
// design accent when the desktop reports none).
namespace Theme {

// Build the full stylesheet for a given palette and accent ("#RRGGBB").
// `assetDir` is the directory holding the generated indicator PNGs (spin-box
// arrows, combo chevrons, menu check marks, tab close glyphs - see applyApp);
// when empty those image-based rules are omitted and the native indicators are
// used. Exposed mainly for testing; normal callers use applyApp().
QString buildStyleSheet(const QPalette &pal, const QString &accentHex, const QString &assetDir = QString());

// Rebuild from the current Settings accent + the effective colour scheme and
// apply palette + stylesheet to qApp. Call at startup and whenever the colour
// scheme or accent changes.
void applyApp();

// Ink for runtime-tinted toolbar/menu icons: the inkBody token, a step below the
// primary text (the design's "toolbar body" tone) in both themes.
QColor iconInk(const QPalette &pal);

// The resolved accent colour: a custom "#RRGGBB" accent setting as-is, or for
// "system"/empty the OS accent in `pal` (see systemAccent) - exactly the
// resolution the stylesheet uses. The hamburger menu paints its right-side check
// marks with it so they match the design's blue menu checks in both themes.
QColor accentColor(const QString &accentSetting, const QPalette &pal);

// The accent the last applyApp() resolved, for painters the stylesheet cannot
// reach, such as widgets whose class palette the platform style overrides.
// Invalid until applyApp() has run.
QColor appliedAccent();

// The accent "system" gives the dark or light scheme, whatever the accent setting
// is: the OS accent applyApp() last read from the platform in that scheme, made
// legible there, or the scheme's design accent when the desktop reported none.
// A scheme the application has not been in yet uses the other scheme's reading.
// Settings previews it while "Use the system accent colour" is ticked but not yet
// applied. Before applyApp() has run, it reads the application palette.
QColor systemAccent(bool dark);

// True while applyApp() is installing its palette and stylesheet, so a listener
// can tell the palette changes it causes from those made by someone else.
bool applying();

// Every spin box goes through one of the two helpers below. Both keep the
// embedded editor inside the stylesheet's content rect, which Qt 6.12.0 no
// longer does on its own: it starts the editor at x 0, over the border and left
// padding, and gives a NoButtons box a 1px editor, so the value disappears.

// Turn a spin box into a typed-only field: no stepper arrows, and no reserved
// space for them. Use this instead of calling setButtonSymbols(NoButtons)
// directly - the stylesheet reserves 22px on the right of every spin box for the
// stepper column, and a NoButtons box that keeps that reservation can push its
// own value out of the visible content rect.
void useTypedSpinBox(QAbstractSpinBox *box);

// For a spin box that keeps its stepper arrows: keeps the value at the same
// 8px inset as a QLineEdit instead of against the left border.
void useSteppedSpinBox(QAbstractSpinBox *box);

} // namespace Theme
} // namespace mervin
