#pragma once

#include <QColor>
#include <QString>

class QAbstractSpinBox;
class QPalette;

namespace mervin {

// Central application styling. One QSS sheet, applied at the QApplication level
// (so it reaches the main windows AND the separate top-level dialogs).
//
// Every colour it uses comes from ui/ThemeTokens.h - this file owns the rules,
// that one owns the values, so a colour change is a single edit there.
//
// Light mode matches the md-easy visual language: clean, light, flat, rounded,
// with a single user-configurable accent colour driving selected segments, tabs
// and focus. Dark mode implements the "Compact Slate" spec from the Mervin PDF
// design project: a cool blue-gray slate ramp with flat borderless toolbar
// buttons, hairline white-alpha separators and a single blue accent.
//
// The sheet is theme-aware: light colours derive from the active QPalette; in
// dark mode applyApp() additionally installs an explicit slate QPalette
// (theme::darkPalette) so that palette-driven paint code (panels, item
// delegates, icon tinting) matches the QSS chrome. Switching back to light
// restores the platform palette.
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

// Ink for runtime-tinted toolbar/menu icons. The slate dark theme draws icons a
// step below the primary text (the design's "toolbar body" tone); light mode
// follows the palette text as before.
QColor iconInk(const QPalette &pal);

// The resolved accent colour: a custom "#RRGGBB" accent setting as-is, or the
// palette's accent for "system"/empty - exactly the resolution the stylesheet
// uses. The hamburger menu paints its right-side check marks with it so they
// match the design's blue menu checks in both themes.
QColor accentColor(const QString &accentSetting, const QPalette &pal);

// Turn a spin box into a typed-only field: no stepper arrows, and no reserved
// space for them. Use this instead of calling setButtonSymbols(NoButtons)
// directly - the stylesheet reserves 22px on the right of every spin box for the
// stepper column, and a NoButtons box that keeps that reservation can push its
// own value out of the visible content rect.
void useTypedSpinBox(QAbstractSpinBox *box);

} // namespace Theme
} // namespace mervin
