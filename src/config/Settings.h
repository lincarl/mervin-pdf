#pragma once

#include "render/AnnotTypes.h"

#include <QByteArray>
#include <QString>

namespace mervin {

// Application settings, persisted as TOML in %APPDATA%/MervinPDF/config.toml.
// Defaults match the functional spec. Fields not yet consumed by the current
// milestone are still loaded/saved so the file round-trips cleanly.
struct Settings
{
    // View defaults
    QString defaultZoom = QStringLiteral("fit-width"); // "fit-width" | "fit-page" | "<percent>"
    // The two page-layout axes, kept apart so either can be chosen without
    // giving up the other. `page_mode` used to carry a third value, "two-page",
    // which is migrated on load to continuous + spread (see Settings::load).
    QString pageMode = QStringLiteral("continuous");   // "continuous" | "single"
    bool twoPageSpread = false;                        // facing pages in one row
    // UI theme: the application chrome's light/dark scheme, chosen in Settings ->
    // Appearance -> UI theme. New installs default to "dark". "system" follows the
    // OS setting, including live auto-switches.
    QString colorScheme = QStringLiteral("dark");      // "system" | "light" | "dark"
    // Document theme: how PDF pages are tinted, independent of the UI theme. A
    // user may want dark chrome but classic white pages, or vice versa.
    //   "light"     - classic, never inverted (the default)
    //   "dark"      - always inverted (plain colour negative)
    //   "comfort"   - the Inverted negative offset onto a soft dark ramp
    //                 (white paper -> dark grey, black ink -> light grey,
    //                 colours -> muted complements), applied uniformly to
    //                 every pixel (see ComfortTransform)
    //   "follow-ui" - inverted only when the UI theme is dark
    // Migrated from the old boolean `invert_colors` (true -> "dark", false ->
    // "light") when an older config is loaded; fresh installs default to light.
    QString documentTheme = QStringLiteral("light"); // "light" | "dark" | "comfort" | "follow-ui"

    // UI accent colour. Drives selected segments, tabs, focus rings, etc. via the
    // central Theme. "system" (the default) follows the OS accent colour; set to a
    // "#RRGGBB" value for a fixed custom accent.
    QString accentColor = QStringLiteral("system");

    // Open behaviour (consumed in M5)
    QString openBehavior = QStringLiteral("new-tab");  // "new-tab" | "new-window"

    // Recent files (consumed in M6)
    int recentVisibleCount = 100;
    int recentRetention = 500;

    // Measuring tool defaults (seed each new tab's measure panel).
    QString measurementUnit = QStringLiteral("mm");        // mm | cm | m | in | ft
    QString measurementType = QStringLiteral("distance");  // distance | path | area | angle
    int measurementPrecision = 2;                          // decimal places
    double measurementLineWidth = 2.0;                     // stroke width (points)
    bool measurementSnap = true;                           // snap to CAD vertices/edges

    // Form filling: tint fillable AcroForm fields while in form-fill mode, and
    // enter form-fill mode automatically when opening a PDF that has fields.
    bool highlightFormFields = true;
    bool autoFormFill = true;

    // Selection OCR. This is a Tesseract language code (for example, "eng").
    // If its model is not installed, the OCR picker falls back to the first
    // installed language instead of asking Tesseract to load a missing model.
    QString ocrDefaultLanguage = QStringLiteral("eng");

    // Annotations (highlight / comment). The author stamps new marks' /T field;
    // empty means "use the OS user name" (resolved at use). annotationColor is the
    // default colour for new marks and sticky notes (one shared colour), chosen in
    // the Settings dialog; existing marks are recoloured per-note from the swatches
    // in their comment card. annotationStyle is the last markup style.
    QString annotationAuthor;
    // Derived from the markup presets rather than re-typed: this default and
    // annot::palette()[0] have to be the same colour, and they were three separate
    // literals before.
    QString annotationColor = annot::defaultColor().name(QColor::HexRgb).toUpper();
    QString annotationStyle = QStringLiteral("highlight"); // highlight | underline | strikeout

    // Crash recovery / session restore (M11), on by default
    bool restoreSession = true;

    // Updates (opt-in, off by default)
    bool checkUpdatesOnStartup = false;

    // First-run: whether we've already offered to make Mervin the default PDF
    // viewer. The prompt is shown exactly once, on the first launch; after that
    // the user is never asked again, regardless of how they answered.
    bool promptedSetDefaultApp = false;

    // Window geometry/state (base64 of QMainWindow::saveGeometry/saveState)
    QByteArray windowGeometry;
    QByteArray windowState;

    static Settings load();
    void save() const;
};

} // namespace mervin
