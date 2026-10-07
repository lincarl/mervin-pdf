#include "ui/Theme.h"

#include "config/ConfigPaths.h"
#include "config/Settings.h"
#include "ui/Icons.h"
#include "ui/ThemeTokens.h"

#include <QAbstractSpinBox>
#include <QApplication>
#include <QColor>
#include <QDir>
#include <QEvent>
#include <QGuiApplication>
#include <QLineEdit>
#include <QPainter>
#include <QPalette>
#include <QPixmap>
#include <QScopedValueRollback>
#include <QStandardPaths>
#include <QStyle>
#include <QStyleFactory>
#include <QStyleHints>
#include <QStyleOption>

#include <memory>
#include <optional>

namespace mervin {

// Every colour below comes from theme::chrome() - see ui/ThemeTokens.h. This
// file owns the *rules*; the token module owns the values.
using theme::css;

namespace {

// Fusion's palette, which Qt uses for every role a platform theme leaves unset.
const QPalette &fusionPalette()
{
    static const QPalette pal = [] {
        const std::unique_ptr<QStyle> fusion(QStyleFactory::create(QStringLiteral("Fusion")));
        return fusion ? fusion->standardPalette() : QPalette(Qt::transparent);
    }();
    return pal;
}

// The OS accent in `pal`, or an invalid colour when the desktop reports none.
// Qt's Windows theme reports it as Accent, in the shade Windows 11 uses for the
// current scheme, and Plasma's own platform theme sets both roles. Qt's GTK theme
// and its built-in KDE theme (which the AppImage uses on Plasma) report it only as
// the selection colour, Highlight, and leave Accent at Fusion's #308cc6. A role
// that still holds #308cc6 came from Qt, not the desktop, as both do on Linux
// without a desktop platform theme. So does a plain white or black one, which Qt
// makes up when a KDE profile has no colours.
QColor platformAccent(const QPalette &pal)
{
    for (const QPalette::ColorRole role : {QPalette::Accent, QPalette::Highlight}) {
        const QColor c = pal.color(role);
        if (c.isValid() && c != fusionPalette().color(role) && c != QColor(Qt::white)
            && c != QColor(Qt::black))
            return c;
    }
    return QColor();
}

// The accent "system" gives a scheme: the OS accent `os`, made legible there,
// or the scheme's design accent when the desktop reports none.
QColor systemAccentFor(const QColor &os, bool dark)
{
    return os.isValid() ? theme::legibleAccent(os, dark) : theme::defaultAccent(dark);
}

// Resolve the accent setting to a colour. "system" (or empty) follows the OS
// accent in `pal` (see systemAccentFor); anything else is parsed as an explicit
// "#RRGGBB". applyApp() installs the resolved accent into the palette, so after
// it "system" reads back the same colour.
QColor resolveAccent(const QString &hex, const QPalette &pal)
{
    if (hex.isEmpty() || hex.compare(QLatin1String("system"), Qt::CaseInsensitive) == 0)
        return systemAccentFor(platformAccent(pal), theme::isDark(pal));
    const QColor c(hex);
    return c.isValid() ? c : theme::defaultAccent(theme::isDark(pal));
}

// ---- Generated indicator glyphs ---------------------------------------------
// QSS can only point at glyph images via image: url(...) on disk, so the icons
// it needs are rendered at runtime into the cache dir, tinted to the theme. They
// are the app's Lucide icons (mervin::icons), drawn with a heavier stroke than
// the set's 1.5 units: at 10-15 px that line would thin to about a pixel.

// Lucide "check" (menu indicators, checkbox check).
QPixmap paintCheck(const QColor &color, int sz)
{
    return icons::glyphPixmap(icons::Glyph::Check, color, sz, 2.6);
}

// Lucide "x" (tab close buttons). 2.0 units keeps the cross about as much heavier
// than the 16 px file icon beside it as it was before the set's stroke went to 1.5.
QPixmap paintCross(const QColor &color, int sz)
{
    return icons::glyphPixmap(icons::Glyph::Close, color, sz, 2.0);
}

// A filled dot (radio-button check).
QPixmap paintDot(const QColor &color, int sz)
{
    QPixmap pm(sz, sz);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(Qt::NoPen);
    p.setBrush(color);
    p.drawEllipse(pm.rect());
    return pm;
}

// Rasterize QSS glyphs into the cache and return its forward-slash path, or empty on failure.
// Both themes need arrows/close; dark additionally supplies menu/check/radio indicators.
QString writeThemeAssets(const theme::Chrome &t)
{
    // A --profile instance keeps its glyph cache inside the profile: the file
    // names are fixed and the pixels depend on the palette, so a profile
    // instance running ALONGSIDE the installed app with a different theme
    // would otherwise overwrite the shared cache under it (and vice versa).
    QString dir = ConfigPaths::overrideDir().isEmpty()
                      ? QStandardPaths::writableLocation(QStandardPaths::CacheLocation)
                      : QDir(ConfigPaths::overrideDir()).filePath(QStringLiteral("cache"));
    if (dir.isEmpty() || !QDir().mkpath(dir))
        return {};

    // Every glyph is written at 1x and 2x ("<name>@2x.png"): the stylesheet
    // engine resolves image:url() through Qt's @Nx lookup, so displays scaled
    // above 100% (125/150/200%) pick the 2x pixels instead of blurring 1x up.
    const auto save = [&dir](const QPixmap &pm, const char *file) {
        return pm.save(dir + QLatin1Char('/') + QLatin1String(file), "PNG");
    };

    constexpr int sz = 16;
    // Two spin tints: full-strength ink for the enabled state and the disabled
    // ink for the greyed-out end of the range. The combo chevron follows the
    // toolbar ink, so a closed combo reads as a control rather than a heading.
    const struct {
        const char *file;
        const char *file2x;
        bool down;
        QColor color;
    } chevrons[] = {
        {"spin_up.png", "spin_up@2x.png", false, t.inkBody},
        {"spin_down.png", "spin_down@2x.png", true, t.inkBody},
        {"spin_up_off.png", "spin_up_off@2x.png", false, t.inkDisabled},
        {"spin_down_off.png", "spin_down_off@2x.png", true, t.inkDisabled},
        {"combo_arrow.png", "combo_arrow@2x.png", true, t.inkBody},
        {"combo_arrow_off.png", "combo_arrow_off@2x.png", true, t.inkDisabled},
    };
    for (const auto &it : chevrons) {
        if (!save(icons::spinChevron(it.down, it.color, sz), it.file)
            || !save(icons::spinChevron(it.down, it.color, sz * 2), it.file2x))
            return {};
    }

    // The tab close cross is painted in both themes: the native light-mode glyph
    // is a red-filled box that matches nothing else in the app.
    const struct {
        QPixmap (*paint)(const QColor &, int);
        QColor color;
        int sz;
        const char *file;
        const char *file2x;
    } crosses[] = {
        {paintCross, t.inkFaint, 14, "tab_close.png", "tab_close@2x.png"},
        {paintCross, t.inkBody, 14, "tab_close_hover.png", "tab_close_hover@2x.png"},
    };
    for (const auto &g : crosses) {
        if (!save(g.paint(g.color, g.sz), g.file)
            || !save(g.paint(g.color, g.sz * 2), g.file2x))
            return {};
    }

    if (t.dark) {
        // Menu checks and check/radio indicators: the native ones don't match the
        // dark design, but the native LIGHT ones are fine, so these stay
        // dark-only. The tick and dot sit on the accent fill, so they take its
        // text colour: dark on Nord's light frost accent, white on a deep one.
        const struct {
            QPixmap (*paint)(const QColor &, int);
            QColor color;
            int sz;
            const char *file;
            const char *file2x;
        } glyphs[] = {
            {paintCheck, t.accent, 15, "menu_check.png", "menu_check@2x.png"},
            {paintCheck, t.onAccent, 10, "check_box.png", "check_box@2x.png"},
            {paintDot, t.onAccent, 6, "radio_dot.png", "radio_dot@2x.png"},
            // A disabled checked box loses the accent fill to the faint disabled
            // wash, where the accent's text colour can be near-invisible.
            {paintCheck, t.inkFaint, 10, "check_box_off.png", "check_box_off@2x.png"},
            {paintDot, t.inkFaint, 6, "radio_dot_off.png", "radio_dot_off@2x.png"},
        };
        for (const auto &g : glyphs) {
            if (!save(g.paint(g.color, g.sz), g.file)
                || !save(g.paint(g.color, g.sz * 2), g.file2x))
                return {};
        }
    }
    return QDir::fromNativeSeparators(dir);
}

} // namespace

QString Theme::buildStyleSheet(const QPalette &pal, const QString &accentHex, const QString &assetDir)
{
    const theme::Chrome t = theme::chrome(pal, resolveAccent(accentHex, pal));
    const bool dark = t.dark;

    // Corner radius for buttons / inputs / segments: the dark design rounds small
    // controls at 6px; light keeps the md-easy 8px.
    const QString rad = dark ? QStringLiteral("6px") : QStringLiteral("8px");
    // Buttons: light draws md-easy outlined buttons; dark draws the design's
    // flat borderless toolbar buttons and outlined-pill push buttons.
    const QString transparent = QStringLiteral("transparent");
    const QString pushBg = dark ? transparent : css(t.fill);
    const QString toolBorder = dark ? transparent : css(t.border);
    const QString toolBorderHover = dark ? transparent : css(t.borderStrong);

    QString s;
    const auto add = [&s](const QString &rule) { s += rule; s += QLatin1Char('\n'); };

    // ── Generic controls (reach the dialogs too, via the app-level sheet) ────
    add(QStringLiteral("QDialog, QMessageBox { background:%1; }").arg(css(t.window)));

    add(QStringLiteral("QPushButton { background:%1; color:%2; border:1px solid %3;"
                       " border-radius:%4; padding:6px 14px; }")
            .arg(pushBg, css(t.ink), css(t.borderPush), rad));
    add(QStringLiteral("QPushButton:hover { background:%1; border-color:%2; }")
            .arg(css(t.hover), css(t.borderStrong)));
    add(QStringLiteral("QPushButton:pressed { background:%1; }").arg(css(t.pressed)));
    // Primary / default button: solid accent fill (Fluent look).
    add(QStringLiteral("QPushButton:default { background:%1; color:%2; border-color:%1; }")
            .arg(css(t.accent), css(t.onAccent)));
    add(QStringLiteral("QPushButton:default:hover { background:%1; border-color:%1; }")
            .arg(css(t.accentHover)));
    add(QStringLiteral("QPushButton:default:pressed { background:%1; border-color:%1; }")
            .arg(css(t.accentHover)));
    // A disabled default must not keep the accent fill - :disabled alone doesn't
    // set background, so reset it explicitly to the neutral style.
    add(QStringLiteral("QPushButton:default:disabled { background:%1; color:%2; border-color:%3; }")
            .arg(pushBg, css(t.inkDisabled), css(t.borderDisabled)));
    add(QStringLiteral("QPushButton:disabled { color:%1; border-color:%2; }")
            .arg(css(t.inkDisabled), css(t.borderDisabled)));

    add(QStringLiteral("QLineEdit, QPlainTextEdit, QTextEdit, QSpinBox, QDoubleSpinBox, QComboBox {"
                       " background:%1; color:%2; border:1px solid %3; border-radius:%6;"
                       " padding:4px 8px; selection-background-color:%4; selection-color:%5; }")
            .arg(css(t.well), css(t.inkPrimary), css(t.border), css(t.accent), css(t.onAccent), rad));
    add(QStringLiteral("QLineEdit:focus, QPlainTextEdit:focus, QTextEdit:focus, QSpinBox:focus,"
                       " QDoubleSpinBox:focus, QComboBox:focus, QComboBox:on { border-color:%1; }")
            .arg(css(t.accent)));
    // Editable combos embed a QLineEdit; neutralise it so it doesn't draw a
    // second border/radius inside the combo frame.
    add(QStringLiteral("QComboBox QLineEdit { border:none; border-radius:0; padding:0;"
                       " background:transparent; selection-background-color:%1; selection-color:%2; }")
            .arg(css(t.accent), css(t.onAccent)));
    add(QStringLiteral("QComboBox QAbstractItemView { background:%1; border:1px solid %2;"
                       " selection-background-color:%3; selection-color:%4; }")
            .arg(css(t.popover), css(t.border), css(t.accent), css(t.onAccent)));
    // The UI language picker's popup (LanguageCombo): a popover card holding a
    // search field above a list with rounded rows.
    add(QStringLiteral("QFrame#languagePopup { background:%1; border:1px solid %2;"
                       " border-radius:10px; }")
            .arg(css(t.popover), css(t.borderPopover)));
    add(QStringLiteral("QListView#uiLanguageList { background:transparent; border:none; outline:0; }"));
    add(QStringLiteral("QListView#uiLanguageList::item { padding:6px 8px; border-radius:6px;"
                       " color:%1; }")
            .arg(css(t.ink)));
    add(QStringLiteral("QListView#uiLanguageList::item:hover { background:%1; }")
            .arg(css(t.rowHover)));
    add(QStringLiteral("QListView#uiLanguageList::item:selected { background:%1; color:%2; }")
            .arg(css(t.accent), css(t.onAccent)));
    // A spin box embeds a QLineEdit too, and the shared rule above matches it by
    // type - so without this reset it drew its own border, radius and 8px padding
    // *inside* the spin box frame, squeezing the value.
    add(QStringLiteral("QSpinBox QLineEdit, QDoubleSpinBox QLineEdit { border:none;"
                       " border-radius:0; padding:0; background:transparent;"
                       " selection-background-color:%1; selection-color:%2; }")
            .arg(css(t.accent), css(t.onAccent)));

    // Combo boxes: a closed combo has to advertise that it opens. Qt's native
    // arrow is suppressed the moment a stylesheet styles ::drop-down, so the app
    // showed bare wells with no affordance at all; draw the house chevron (the
    // same glyph as the spin steppers) and reserve room for it.
    if (!assetDir.isEmpty()) {
        add(QStringLiteral("QComboBox { padding-right:24px; }"));
        add(QStringLiteral("QComboBox::drop-down { subcontrol-origin:padding;"
                           " subcontrol-position:center right; width:20px; border:none;"
                           " background:transparent; }"));
        add(QStringLiteral("QComboBox::down-arrow { image:url(%1/combo_arrow.png);"
                           " width:12px; height:12px; }")
                .arg(assetDir));
        add(QStringLiteral("QComboBox::down-arrow:disabled { image:url(%1/combo_arrow_off.png); }")
                .arg(assetDir));
    } else {
        add(QStringLiteral("QComboBox::drop-down { border:none; width:18px; }"));
    }

    // Qt adds right padding to the dropdown width. The compact zoom box needs only a small gap;
    // the subcontrol already reserves chevron space.
    add(QStringLiteral("QComboBox#zoomCombo { padding-right:6px; }"));

    // Spin boxes: the native up/down arrows render illegibly small under our
    // stylesheet, so draw a proper stacked stepper with clear chevron glyphs
    // (painted to PNGs in writeThemeAssets). Skipped when no asset dir is given
    // (e.g. headless tests) - the arrows then fall back to the native ones.
    if (!assetDir.isEmpty()) {
        // Reserve room on the right so the value text never slides under the
        // button column (overrides the shared padding's right edge only).
        add(QStringLiteral("QSpinBox, QDoubleSpinBox { padding-right:22px; }"));
        add(QStringLiteral("QSpinBox::up-button, QDoubleSpinBox::up-button {"
                           " subcontrol-origin:border; subcontrol-position:top right; width:18px;"
                           " border-left:1px solid %1; border-bottom:1px solid %1;"
                           " border-top-right-radius:%2; }")
                .arg(css(t.border), rad));
        add(QStringLiteral("QSpinBox::down-button, QDoubleSpinBox::down-button {"
                           " subcontrol-origin:border; subcontrol-position:bottom right; width:18px;"
                           " border-left:1px solid %1; border-bottom-right-radius:%2; }")
                .arg(css(t.border), rad));
        add(QStringLiteral("QSpinBox::up-button:hover, QDoubleSpinBox::up-button:hover,"
                           " QSpinBox::down-button:hover, QDoubleSpinBox::down-button:hover {"
                           " background:%1; }")
                .arg(css(t.hover)));
        add(QStringLiteral("QSpinBox::up-button:pressed, QDoubleSpinBox::up-button:pressed,"
                           " QSpinBox::down-button:pressed, QDoubleSpinBox::down-button:pressed {"
                           " background:%1; }")
                .arg(css(t.pressed)));
        add(QStringLiteral("QSpinBox::up-arrow, QDoubleSpinBox::up-arrow {"
                           " image:url(%1/spin_up.png); width:12px; height:12px; }")
                .arg(assetDir));
        add(QStringLiteral("QSpinBox::down-arrow, QDoubleSpinBox::down-arrow {"
                           " image:url(%1/spin_down.png); width:12px; height:12px; }")
                .arg(assetDir));
        add(QStringLiteral("QSpinBox::up-arrow:disabled, QDoubleSpinBox::up-arrow:disabled {"
                           " image:url(%1/spin_up_off.png); }")
                .arg(assetDir));
        add(QStringLiteral("QSpinBox::down-arrow:disabled, QDoubleSpinBox::down-arrow:disabled {"
                           " image:url(%1/spin_down_off.png); }")
                .arg(assetDir));
    }
    // Typed-only spin boxes reserve no stepper width; otherwise narrow fields clip their
    // values. The attribute selector overrides plain-type rules. QSS alone cannot keep
    // these fields readable on Qt 6.12, which sizes a NoButtons editor to 1px whatever
    // the rules say; Theme::useTypedSpinBox adds the editor guard that fixes that.
    add(QStringLiteral("QAbstractSpinBox[noButtons=\"true\"] { padding-right:8px; }"));
    add(QStringLiteral("QAbstractSpinBox[noButtons=\"true\"]::up-button,"
                       " QAbstractSpinBox[noButtons=\"true\"]::down-button {"
                       " width:0; border:none; image:none; }"));

    add(QStringLiteral("QGroupBox { border:1px solid %1; border-radius:8px;"
                       " margin-top:10px; padding:12px 12px 10px; }").arg(css(t.border)));
    add(QStringLiteral("QGroupBox::title { subcontrol-origin:margin; subcontrol-position:top left;"
                       " left:10px; padding:0 4px; color:%1; }").arg(css(t.inkSoft)));
    // Settings draws a group the menu already names as an untitled frame: a
    // QGroupBox keeps room for its title even when it has none. Same edge and
    // insets as the group box, without the title margin.
    add(QStringLiteral("QFrame#settingsBox { border:1px solid %1; border-radius:8px;"
                       " padding:12px 12px 10px; }").arg(css(t.border)));

    // ── Settings dialog: page menu on the left, scrolling pages on the right ──
    // The menu sits on the darker status band. A selected row gets a fill clearly
    // above the hover wash plus a hairline, so selection and hover never look
    // alike (the menu's own rule, hover as the only tint, does not suit a list
    // that keeps a selection).
    add(QStringLiteral("QListWidget#settingsNav { background:%1; border:none;"
                       " border-right:1px solid %2; outline:0; padding:10px 8px; }")
            .arg(css(t.status), css(t.borderBar)));
    add(QStringLiteral("QListWidget#settingsNav::item { min-height:32px; padding:0 8px;"
                       " margin:1px 0; border:1px solid transparent; border-radius:6px; color:%1; }")
            .arg(css(t.inkBody)));
    add(QStringLiteral("QListWidget#settingsNav::item:hover { background:%1; }").arg(css(t.rowHover)));
    add(QStringLiteral("QListWidget#settingsNav::item:selected { background:%1; border-color:%2;"
                       " color:%3; }")
            .arg(css(t.pressed), css(t.border), css(t.inkPrimary)));
    add(QStringLiteral("QFrame#settingsNavDivider, QFrame#settingsRule { background:%1; border:none; }")
            .arg(css(t.separator)));
    // Pages scroll inside the dialog body; without this the scroll area paints
    // the palette Window grey over the dialog background in light mode.
    add(QStringLiteral("QScrollArea#settingsPage, QScrollArea#settingsPage > QWidget > QWidget"
                       " { background:transparent; border:none; }"));
    add(QStringLiteral("QWidget#settingsFooter { border-top:1px solid %1; }").arg(css(t.separator)));
    add(QStringLiteral("QLabel#settingsHint, QLabel#settingsPath, QLabel#aboutVersion { color:%1; }")
            .arg(css(t.inkSoft)));
    add(QStringLiteral("QLabel#aboutName { font-size:18px; font-weight:600; color:%1; }")
            .arg(css(t.inkPrimary)));
    add(QStringLiteral("QFrame#settingsList { background:%1; border:1px solid %2; border-radius:6px; }")
            .arg(css(t.well), css(t.border)));
    add(QStringLiteral("QToolButton#settingsListRemove { border:none; background:transparent;"
                       " padding:3px; border-radius:4px; }"));
    add(QStringLiteral("QToolButton#settingsListRemove:hover { background:%1; }").arg(css(t.hover)));
    add(QStringLiteral("QWidget#shortcutRow { border-bottom:1px solid %1; }").arg(css(t.separator)));
    add(QStringLiteral("QLabel#shortcutAction { font-weight:600; color:%1; }").arg(css(t.inkPrimary)));

    add(QStringLiteral("QCheckBox { spacing:6px; }"));
    if (dark) {
        // The design's checkboxes: a 14px rounded square outlined in the
        // disabled ink, filling solid accent with a check in the accent's text
        // colour when on. Radio buttons get the same treatment with a round shape
        // and a dot. Disabled ones keep the mark in a neutral ink on the disabled
        // wash. Light mode keeps the native indicators.
        add(QStringLiteral("QCheckBox::indicator, QRadioButton::indicator {"
                           " width:14px; height:14px; background:transparent;"
                           " border:1px solid %1; }").arg(css(t.inkDisabled)));
        add(QStringLiteral("QCheckBox::indicator { border-radius:4px; }"));
        add(QStringLiteral("QRadioButton::indicator { border-radius:7px; }"));
        add(QStringLiteral("QCheckBox::indicator:hover, QRadioButton::indicator:hover {"
                           " border-color:%1; }").arg(css(t.inkSoft)));
        add(QStringLiteral("QCheckBox::indicator:checked, QRadioButton::indicator:checked {"
                           " background:%1; border-color:%1; }").arg(css(t.accent)));
        if (!assetDir.isEmpty()) {
            add(QStringLiteral("QCheckBox::indicator:checked { image:url(%1/check_box.png); }")
                    .arg(assetDir));
            add(QStringLiteral("QRadioButton::indicator:checked { image:url(%1/radio_dot.png); }")
                    .arg(assetDir));
        }
        add(QStringLiteral("QCheckBox::indicator:disabled, QRadioButton::indicator:disabled {"
                           " border-color:%1; }").arg(css(t.borderDisabled)));
        add(QStringLiteral("QCheckBox::indicator:checked:disabled,"
                           " QRadioButton::indicator:checked:disabled { background:%1; }")
                .arg(css(t.borderDisabled)));
        if (!assetDir.isEmpty()) {
            add(QStringLiteral("QCheckBox::indicator:checked:disabled {"
                               " image:url(%1/check_box_off.png); }").arg(assetDir));
            add(QStringLiteral("QRadioButton::indicator:checked:disabled {"
                               " image:url(%1/radio_dot_off.png); }").arg(assetDir));
        }
    }

    add(QStringLiteral("QToolTip { background:%1; color:%2; border:1px solid %3; padding:3px 6px; }")
            .arg(css(t.popover), css(t.ink), css(t.border)));

    // Scrollbars: thin, minimal, light thumb on a transparent track.
    add(QStringLiteral("QScrollBar:vertical { background:transparent; width:12px; margin:0; }"
                       "QScrollBar::handle:vertical { background:%1; border-radius:4px;"
                       " min-height:28px; margin:2px; }"
                       "QScrollBar::handle:vertical:hover { background:%2; }"
                       "QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical {"
                       " height:0; background:none; border:none; }"
                       "QScrollBar::add-page:vertical, QScrollBar::sub-page:vertical { background:none; }")
            .arg(css(t.scrollThumb), css(t.scrollThumbHover)));
    add(QStringLiteral("QScrollBar:horizontal { background:transparent; height:12px; margin:0; }"
                       "QScrollBar::handle:horizontal { background:%1; border-radius:4px;"
                       " min-width:28px; margin:2px; }"
                       "QScrollBar::handle:horizontal:hover { background:%2; }"
                       "QScrollBar::add-line:horizontal, QScrollBar::sub-line:horizontal {"
                       " width:0; background:none; border:none; }"
                       "QScrollBar::add-page:horizontal, QScrollBar::sub-page:horizontal { background:none; }")
            .arg(css(t.scrollThumb), css(t.scrollThumbHover)));

    // ── Menus ────────────────────────────────────────────────────────────────
    // Dark implements the design's popover spec: raised surface, hairline border,
    // rounded 6px items with a soft white hover wash and blue check marks.
    // Light mode keeps the native platform menus.
    if (dark) {
        add(QStringLiteral("QMenu { background-color:%1; color:%2; border:1px solid %3;"
                           " border-radius:10px; padding:6px; }")
                .arg(css(t.popover), css(t.ink), css(t.border)));
        add(QStringLiteral("QMenu::item { background:transparent; padding:6px 10px;"
                           " border-radius:6px; margin:1px 0; }"));
        add(QStringLiteral("QMenu::item:selected { background:%1; }").arg(css(t.rowHover)));
        // No background on checked rows: hover is the only row tint in a menu, so
        // a filled row always means "the cursor is here". Active state is carried
        // by the accent check mark alone (the indicator below, or the right-edge
        // check MainWindow's RightCheckMenu paints for the icon-bearing toggles
        // whose left indicator the item pipeline suppresses).
        add(QStringLiteral("QMenu::item:disabled { color:%1; }").arg(css(t.inkDisabled)));
        add(QStringLiteral("QMenu::separator { height:1px; background:%1; margin:6px 4px; }")
                .arg(css(t.separator)));
        add(QStringLiteral("QMenu::indicator { width:15px; height:15px; }"));
        if (!assetDir.isEmpty()) {
            // One blue check for both checkable kinds (the design marks radio
            // choices with the same check), nothing when unchecked.
            add(QStringLiteral("QMenu::indicator:checked { image:url(%1/menu_check.png); }")
                    .arg(assetDir));
            add(QStringLiteral("QMenu::indicator:unchecked { image:none; }"));
        }
    }

    // Menu section headers (the design's group labels): a muted caption,
    // painted as QLabels inside a QWidgetAction (MainWindow::addSectionHeader) so
    // the app sheet reaches them in both the styled dark menus and the native light
    // ones. The :disabled variant keeps the colour even though the header action is
    // disabled (to make it non-selectable).
    add(QStringLiteral("QLabel#menuSectionHeader, QLabel#menuSectionHeader:disabled { color:%1; }")
            .arg(css(t.inkMenuHeader)));

    // ── Toolbar (flat icon buttons; borderless in the dark theme) ───────────
    add(QStringLiteral("QToolBar { spacing:4px; padding:3px; background:transparent;"
                       " border:none; border-bottom:1px solid %1; }")
            .arg(dark ? css(t.borderBar) : transparent));
    add(QStringLiteral("QToolButton { border:1px solid %1; border-radius:%2;"
                       " padding:4px 8px; background:transparent; color:%3; }")
            .arg(toolBorder, rad, css(t.inkBody)));
    add(QStringLiteral("QToolButton:hover { background:%1; border-color:%2; }")
            .arg(css(t.hover), toolBorderHover));
    add(QStringLiteral("QToolButton:pressed { background:%1; border-color:%2; }")
            .arg(css(t.pressed), toolBorderHover));
    // Checked tools (Measure / Comment): the design's accent wash in dark, the
    // pressed overlay in light (as before).
    add(QStringLiteral("QToolButton:checked { background:%1; border-color:%2; }")
            .arg(dark ? css(t.accentWash) : css(t.pressed), toolBorderHover));
    add(QStringLiteral("QToolButton:disabled { color:%1; border-color:%2; }")
            .arg(css(t.inkDisabled), dark ? transparent : css(t.borderDisabled)));
    add(QStringLiteral("QToolButton#menuButton::menu-indicator { image:none; width:0; }"));
    add(QStringLiteral("QToolButton#openButton::menu-button { border:none; width:18px; }"));
    add(QStringLiteral("QToolButton#openButton::menu-arrow { image:none; width:0; height:0; }"));
    add(QStringLiteral("QToolButton#saveButton::menu-button { border:none; width:18px; }"));
    add(QStringLiteral("QToolButton#saveButton::menu-arrow { image:none; width:0; height:0; }"));
    add(QStringLiteral("QToolButton#documentButton::menu-button { border:none; width:18px; }"));
    add(QStringLiteral("QToolButton#documentButton::menu-arrow { image:none; width:0; height:0; }"));
    // Clear button inside line edits stays frameless.
    add(QStringLiteral("QLineEdit QToolButton { border:none; border-radius:0; padding:0; background:transparent; }"));
    if (dark) {
        // The " / N" page counter reads as meta text in the design.
        add(QStringLiteral("QToolBar QLabel { color:%1; }").arg(css(t.inkFaint)));
    }

    // ── Status bar ──────────────────────────────────────────────────────────
    add(QStringLiteral("QStatusBar { background:%1; border-top:1px solid %2; }")
            .arg(css(t.status), css(dark ? t.borderBar : t.border)));
    add(QStringLiteral("QStatusBar::item { border:none; }"));
    if (dark) {
        add(QStringLiteral("QStatusBar QLabel { color:%1; font-size:11px; }")
                .arg(css(t.inkFaint)));
    }

    // ── Tab pills ───────────────────────────────────────────────────────────
    add(QStringLiteral("QTabBar::tab { height:32px; padding:0 10px 0 12px; margin:6px 2px;"
                       " border-radius:%1; border:1px solid transparent; background:transparent; color:%2; }")
            .arg(rad, css(t.inkBody)));
    // Selected tab: the design's lifted body with primary ink in dark;
    // md-easy white fill with accent ink in light.
    add(QStringLiteral("QTabBar::tab:selected { background:%1; border-color:%2; color:%3; font-weight:%4; }")
            .arg(css(t.fill), dark ? css(t.borderDisabled) : css(t.border),
                 dark ? css(t.inkPrimary) : css(t.accent),
                 dark ? QStringLiteral("400") : QStringLiteral("600")));
    add(QStringLiteral("QTabBar::tab:hover:!selected { background:%1; }").arg(css(t.hover)));
    // While the Recent view is active, no document tab should read as selected.
    add(QStringLiteral("QTabBar#docTabBar[recentActive=\"true\"]::tab:selected {"
                       " background:transparent; border-color:transparent; color:%1; font-weight:400; }")
            .arg(css(t.inkBody)));
    // ...but the (still logically current) tab must light up on hover just like
    // every other tab - the base hover rule is :hover:!selected, which skips it,
    // so it otherwise sat inert while the Recent view was up.
    add(QStringLiteral("QTabBar#docTabBar[recentActive=\"true\"]::tab:selected:hover {"
                       " background:%1; }").arg(css(t.hover)));
    add(QStringLiteral("QTabWidget::pane { border:none; }"));
    add(QStringLiteral("QTabBar::scroller { width:44px; }"));
    add(QStringLiteral("QTabBar QToolButton { border:1px solid %1; border-radius:6px;"
                       " background:%2; margin:6px 1px; padding:0; }")
            .arg(css(t.border), css(t.bar)));
    add(QStringLiteral("QTabBar QToolButton:hover { background:%1; border-color:%2; }")
            .arg(css(t.hover), css(t.borderStrong)));
    add(QStringLiteral("QTabBar QToolButton:disabled { background:transparent; border-color:%1; }")
            .arg(css(t.border)));
    if (!assetDir.isEmpty()) {
        // The painted X in both themes: it matches the app's line-icon language,
        // where the native glyph is a mismatch in dark and a red-filled box
        // in light.
        add(QStringLiteral("QTabBar::close-button { image:url(%1/tab_close.png);"
                           " border-radius:4px; padding:1px; }")
                .arg(assetDir));
        add(QStringLiteral("QTabBar::close-button:hover { image:url(%1/tab_close_hover.png);"
                           " background:%2; }")
                .arg(assetDir, css(t.pressed)));
    }

    // ── Tab row chrome ──────────────────────────────────────────────────────
    add(QStringLiteral("QWidget#tabRowWidget { background:%1; border-bottom:1px solid %2; }")
            .arg(css(t.bar), css(dark ? t.borderBar : t.border)));

    // ── Recent pill (mirrors the selected-tab treatment) ────────────────────
    // Both themes use the dark spec's rounded-rectangle shape (6px radius, snug
    // padding); light previously drew a fully-rounded stadium pill, which read as
    // a different control than the dark one.
    add(QStringLiteral("QPushButton#recentPillBtn { background:transparent; color:%1;"
                       " border:1px solid %2; border-radius:6px; padding:5px 12px; }")
            .arg(css(t.inkBody), css(t.borderPush)));
    add(QStringLiteral("QPushButton#recentPillBtn:hover { background:%1; border-color:%2; }")
            .arg(css(t.hover), css(dark ? t.borderStrong : t.border)));
    // Active pill (the Recent view is showing): mirror a selected document tab -
    // the lifted fill and primary ink - but carry the accent border
    // (the same blue the active document tab's glyph takes) so the lone pill reads
    // as highlighted. A selected tab gets its contrast from its transparent
    // neighbours; the pill stands alone, so a neutral border left it looking inert.
    add(QStringLiteral("QPushButton#recentPillBtn[recentActive=\"true\"] {"
                       " background:%1; color:%2; border-color:%3; font-weight:%4; }")
            .arg(css(t.fill), dark ? css(t.inkPrimary) : css(t.accent),
                 css(t.accent),
                 dark ? QStringLiteral("400") : QStringLiteral("600")));

    // Exclusive segmented choices use a solid accent fill: Measure/Comment kinds.
    const QString segSel = QStringLiteral("QWidget#segmentBar QToolButton%1");
    add(segSel.arg(QString()) + QStringLiteral(" { border:1px solid %1; background:%2; color:%3;"
                                               " padding:5px 14px; border-radius:0; }")
                                    .arg(css(t.border), css(t.fill), css(t.ink)));
    // Corner rounding keyed off a `segpos` dynamic property rather than the
    // :first-child/:last-child pseudo-classes (which Qt QSS does not reliably
    // match for plain child widgets - they left the segments square).
    add(segSel.arg(QStringLiteral("[segpos=\"first\"]"))
        + QStringLiteral(" { border-top-left-radius:%1; border-bottom-left-radius:%1; border-right:none; }").arg(rad));
    add(segSel.arg(QStringLiteral("[segpos=\"mid\"]")) + QStringLiteral(" { border-right:none; }"));
    add(segSel.arg(QStringLiteral("[segpos=\"last\"]"))
        + QStringLiteral(" { border-top-right-radius:%1; border-bottom-right-radius:%1; }").arg(rad));
    add(segSel.arg(QStringLiteral(":hover")) + QStringLiteral(" { background:%1; }").arg(css(t.hover)));
    add(segSel.arg(QStringLiteral(":checked"))
        + QStringLiteral(" { background:%1; color:%2; border-color:%1; }")
              .arg(css(t.accent), css(t.onAccent)));
    add(segSel.arg(QStringLiteral(":checked:hover")) + QStringLiteral(" { background:%1; }")
                                                          .arg(css(t.accentHover)));

    // ── Recent page search ──────────────────────────────────────────────────
    // The scope toggles inside the field's right end are plain text: faint at
    // rest, the accent (and bold, set in code) when chosen.
    add(QStringLiteral("QLineEdit#recentSearch QToolButton#recentScope { border:none;"
                       " background:transparent; padding:0; color:%1; }")
            .arg(css(t.inkFaint)));
    add(QStringLiteral("QLineEdit#recentSearch QToolButton#recentScope:hover { color:%1; }")
            .arg(css(t.ink)));
    add(QStringLiteral("QLineEdit#recentSearch QToolButton#recentScope:checked { color:%1; }")
            .arg(css(theme::legibleAccent(t.accent, dark))));
    // Reached with Tab (a click never focuses them): a wash, not a colour, so the
    // chosen toggle keeps its accent.
    add(QStringLiteral("QLineEdit#recentSearch QToolButton#recentScope:focus {"
                       " background:%1; border-radius:4px; }")
            .arg(css(t.hover)));
    // A quiet clear cross, like the find card's close button, in place of the
    // platform's coloured clear icon.
    add(QStringLiteral("QLineEdit#recentSearch QToolButton#recentSearchClear { border:none;"
                       " border-radius:4px; padding:0; background:transparent; color:%1; }")
            .arg(css(t.inkSoft)));
    add(QStringLiteral("QLineEdit#recentSearch QToolButton#recentSearchClear:hover {"
                       " background:%1; }")
            .arg(css(t.hover)));
    add(QStringLiteral("QFrame#recentSearchRule { background:%1; border:none; }")
            .arg(css(t.borderStrong)));
    add(QStringLiteral("QLabel#recentSearchStatus { color:%1; }").arg(css(t.inkSoft)));
    // While a content scan runs the status is in full ink, beside a Stop button.
    add(QStringLiteral("QLabel#recentSearchStatus[running=\"true\"] { color:%1; }")
            .arg(css(t.ink)));
    add(QStringLiteral("QToolButton#recentSearchStop { border:1px solid %1; border-radius:%2;"
                       " padding:2px 10px; background:transparent; color:%3; }")
            .arg(css(t.borderPush), rad, css(t.ink)));
    add(QStringLiteral("QToolButton#recentSearchStop:hover { background:%1; border-color:%2; }")
            .arg(css(t.hover), css(t.borderStrong)));
    add(QStringLiteral("QToolButton#recentSearchStop:pressed { background:%1; }")
            .arg(css(t.pressed)));
    // Reached with Tab: an accent edge, since the hover wash already means hover.
    add(QStringLiteral("QToolButton#recentSearchStop:focus { border-color:%1; }")
            .arg(css(theme::legibleAccent(t.accent, dark))));

    // ── Floating tool panels (Measure, Comment) ─────────────────────────────
    // These live on the viewer's viewport, so they get an explicit ink for every
    // label: the viewport's QPalette::Dark background role makes Qt derive
    // QPalette::Light for child foregrounds, and a panel label that falls back to
    // the palette is invisible in both themes (see MeasurePanel's constructor).
    // The panels stack with the find card, so they share one surface and rim.
    // The dark rim is borderStrong: the popover sits on the canvas at about 1.1:1,
    // and the app draws no shadows to lift a floating panel.
    add(QStringLiteral("QWidget#measurePanel, QWidget#findCard { background:%1;"
                       " border:1px solid %2; border-radius:10px; }")
            .arg(css(t.popover), css(dark ? t.borderStrong : t.borderPopover)));
    // The toolbar's buttons are deliberately borderless, but on a floating panel
    // over a document that leaves nine bare words with no hit target. Scope a
    // visible edge to the panel only.
    add(QStringLiteral("QWidget#measurePanel QToolButton { border-color:%1; }")
            .arg(css(t.borderPopoverControl)));
    add(QStringLiteral("QWidget#measurePanel QToolButton:hover { border-color:%1; }")
            .arg(css(t.borderStrong)));
    add(QStringLiteral("QWidget#measurePanel QToolButton:disabled { border-color:%1; }")
            .arg(css(t.borderDisabled)));
    add(QStringLiteral("QLabel#measureTitle { color:%1; }").arg(css(t.inkPrimary)));
    add(QStringLiteral("QLabel#measureFieldLabel { color:%1; }").arg(css(t.inkSoft)));
    add(QStringLiteral("QLabel#measureRowText { color:%1; }").arg(css(t.ink)));
    add(QStringLiteral("QLabel#measureReadout { font-size:15px; font-weight:600; color:%1; }")
            .arg(css(t.inkPrimary)));
    // The active scale is what every measurement depends on, so it reads at body
    // strength rather than as secondary copy.
    add(QStringLiteral("QLabel#measureScale { color:%1; }").arg(css(t.inkBody)));
    add(QStringLiteral("QLabel#measureListHeader { color:%1; font-weight:600; }")
            .arg(css(t.inkSoft)));
    add(QStringLiteral("QListWidget#measureList { background:%1; border:1px solid %2;"
                       " border-radius:6px; padding:2px; }")
            .arg(css(t.well), css(t.border)));
    add(QStringLiteral("QListWidget#measureList::item { border-radius:4px; }"));
    // Hover goes on the row widget, not on ::item: every row is covered edge to
    // edge by a setItemWidget widget, so the view's viewport never sees the mouse
    // move and ::item:hover could never match.
    add(QStringLiteral("QWidget#measureRow { background:transparent; border-radius:4px; }"));
    add(QStringLiteral("QWidget#measureRow:hover { background:%1; }").arg(css(t.hover)));
    // The measure-cursor toggle: solid accent when armed, so "clicks place points"
    // is unmistakable (the shared accent wash was barely visible on the panel).
    add(QStringLiteral("QToolButton#measureToggleBtn:checked { background:%1; border-color:%1; }")
            .arg(css(t.accent)));
    add(QStringLiteral("QToolButton#measureToggleBtn:checked:hover { background:%1; border-color:%1; }")
            .arg(css(t.accentHover)));
    // Compact stepper buttons that flank the Decimals field (replacing the
    // native spin-box arrows). Square-ish, with a bold +/- glyph.
    add(QStringLiteral("QToolButton#measureStepBtn { min-width:24px; min-height:22px;"
                       " padding:0; font-size:15px; font-weight:600; color:%1;"
                       " border:1px solid %2; }")
            .arg(css(t.ink), css(t.border)));
    add(QStringLiteral("QToolButton#measureStepBtn:hover { border-color:%1; }")
            .arg(css(t.borderStrong)));
    add(QStringLiteral("QToolButton#measureStepBtn:disabled { color:%1; border-color:%2; }")
            .arg(css(t.inkDisabled), css(t.borderDisabled)));
    // Square close/remove buttons need zero padding and no resting border. Include the panel
    // ancestor so specificity exceeds the panel-scoped toolbutton rule (0x102 versus 0x101).
    add(QStringLiteral("QToolButton#measureClearX,"
                       " QWidget#measurePanel QToolButton#measureClearX {"
                       " padding:0; color:%1; border-color:transparent; }")
            .arg(css(t.inkSoft)));
    add(QStringLiteral("QWidget#measurePanel QToolButton#measureClearX:hover {"
                       " color:%1; border-color:%2; }")
            .arg(css(t.inkPrimary), css(t.borderPopoverControl)));

    // ── Find card ───────────────────────────────────────────────────────────
    // Find in document floats over the page in the panel stack; its surface and
    // rim come from the floating-panel rule above.
    add(QStringLiteral("QWidget#findCard QToolButton { border:1px solid transparent;"
                       " border-radius:%1; padding:0; background:transparent; color:%2; }")
            .arg(rad, css(t.inkBody)));
    add(QStringLiteral("QWidget#findCard QToolButton:hover { background:%1; }").arg(css(t.hover)));
    add(QStringLiteral("QWidget#findCard QToolButton:pressed { background:%1; }")
            .arg(css(t.pressed)));
    add(QStringLiteral("QWidget#findCard QToolButton:disabled { background:transparent; color:%1; }")
            .arg(css(t.inkDisabled)));
    add(QStringLiteral("QWidget#findCard QToolButton:focus { border-color:%1; }")
            .arg(css(theme::legibleAccent(t.accent, dark))));
    add(QStringLiteral("QWidget#findCard QToolButton#findCardClose { color:%1; }")
            .arg(css(t.inkSoft)));
    add(QStringLiteral("QWidget#findCard QCheckBox { color:%1; }")
            .arg(css(dark ? t.inkSoft : t.ink)));
    add(QStringLiteral("QFrame#findCardSep { background:%1; border:none; }").arg(css(t.border)));
    add(QStringLiteral("QLabel#findCount { color:%1; }").arg(css(t.inkSoft)));
    // The field: a control edge on the card at rest, and a 2px accent ring while
    // it has focus. The ring takes 1px from the padding, so the text stays put.
    add(QStringLiteral("QLineEdit#findField { border-color:%1; }")
            .arg(css(t.borderPopoverControl)));
    add(QStringLiteral("QLineEdit#findField:focus, QLineEdit#recentSearch:focus {"
                       " border:2px solid %1; padding:3px 7px; }")
            .arg(css(theme::legibleAccent(t.accent, dark))));

    // ── Merge dialog ────────────────────────────────────────────────────────
    // Its row list (RowList) is shared with the Extract dialog, so each list rule,
    // and the summary and error lines, name both dialogs. Same item-view treatment as the measuring panel's list
    // (there is no QHeaderView anywhere in the app, which is why the column
    // captions are a plain widget above the list rather than a QTableWidget header).
    add(QStringLiteral("QLabel#mergeHint { color:%1; }").arg(css(t.inkSoft)));
    add(QStringLiteral("QWidget#rowListHeader QLabel { color:%1; font-weight:600; }")
            .arg(css(t.inkSoft)));
    add(QStringLiteral("QListWidget#mergeList, QListWidget#extractList,"
                       " QListView#extractStrip { background:%1; border:1px solid %2;"
                       " border-radius:6px; padding:2px; }")
            .arg(css(t.well), css(t.border))); // the Extract strip sits in the same well
    add(QStringLiteral("QListWidget#mergeList::item, QListWidget#extractList::item {"
                       " border-radius:4px; }"));
    add(QStringLiteral("QListWidget#mergeList::item:hover,"
                       " QListWidget#extractList::item:hover { background:%1; }")
            .arg(css(t.hover)));
    // Move Up / Move Down / Duplicate / Remove all act on the current row, so
    // which row is current has to be visible. Item views elsewhere in the app get
    // selection from the palette; this one draws its rows as item widgets, which
    // paint over it.
    add(QStringLiteral("QListWidget#mergeList::item:selected,"
                       " QListWidget#extractList::item:selected { background:%1; }")
            .arg(css(t.accentWash)));
    add(QStringLiteral("QLabel#rowListNum, QLabel#rowListOutput { color:%1; }")
            .arg(css(t.inkSoft)));
    add(QStringLiteral("QLabel#mergeSummary, QLabel#extractSummary { color:%1; }")
            .arg(css(t.ink)));
    // The one red in the dialog: a range that does not resolve blocks the Merge
    // or Extract button, so the reason has to read as a blocker, not as a hint.
    add(QStringLiteral("QLabel#mergeError, QLabel#extractError { color:%1; }")
            .arg(css(t.inkDanger)));
    add(QStringLiteral("QToolButton#rowListX { padding:0; }"));

    // ── Extract dialog ──────────────────────────────────────────────────────
    // The row list, summary and error line take the Merge rules above; the strip
    // paints its cells from the same tokens (ExtractStrip::paintCell). These cover
    // the rest.
    add(QStringLiteral("QLineEdit#extractRowSpec[invalid=\"true\"] { border-color:%1; }")
            .arg(css(t.inkDanger))); // beats QLineEdit:focus (id + attribute)
    // "of 31" sits among the column captions but reads like "/ 31": it has to
    // outrank the caption rule (id + two types) and undo its weight.
    add(QStringLiteral("QWidget#rowListHeader QLabel#extractOf { color:%1; font-weight:normal; }")
            .arg(css(t.inkFaint)));

    // OCR language manager: two compact, scan-friendly model lists. Row widgets
    // provide the name/code/size/action layout while all colour stays tokenized.
    add(QStringLiteral("QLabel#ocrLanguageHeading { color:%1; font-size:11px;"
                       " font-weight:600; }").arg(css(t.inkSoft)));
    add(QStringLiteral("QLabel#ocrLanguageMeta, QLabel#ocrLanguageCode,"
                       " QLabel#ocrLanguageSize { color:%1; }").arg(css(t.inkFaint)));
    add(QStringLiteral("QLabel#ocrLanguageName { color:%1; }").arg(css(t.inkPrimary)));
    add(QStringLiteral("QListWidget#ocrInstalledLanguages,"
                       " QListWidget#ocrAvailableLanguages { background:%1;"
                       " border:1px solid %2; border-radius:6px; padding:2px; }")
            .arg(css(t.well), css(t.border)));
    add(QStringLiteral("QListWidget#ocrInstalledLanguages::item,"
                       " QListWidget#ocrAvailableLanguages::item {"
                       " border-bottom:1px solid %1; }").arg(css(t.separator)));
    add(QStringLiteral("QWidget#ocrLanguageRow { background:transparent; }"));
    add(QStringLiteral("QWidget#ocrLanguageRow:hover { background:%1; }").arg(css(t.rowHover)));
    add(QStringLiteral("QPushButton#ocrLanguageAction { min-width:58px; padding:3px 9px; }"));

    // ── Docks / splitters (sidebar chrome) ──────────────────────────────────
    add(QStringLiteral("QDockWidget::title { background:%1; padding:5px 8px; border-bottom:1px solid %2; }")
            .arg(css(t.bar), css(dark ? t.borderBar : t.border)));
    add(QStringLiteral("QMainWindow::separator { background:%1; width:1px; height:1px; }")
            .arg(css(t.separator)));
    add(QStringLiteral("QSplitter::handle { background:%1; }").arg(css(t.separator)));

    return s;
}

namespace {

QColor &lastAppliedAccent()
{
    static QColor accent;
    return accent;
}

// The OS accent applyApp() last read from the platform palette in the light [0]
// and dark [1] scheme, invalid when the desktop reported none there. Empty for a
// scheme the application has not been in. Each scheme keeps its own reading:
// Windows uses another shade in each, and Qt's GTK and KDE themes report no
// accent at all for a scheme forced against the desktop's.
std::optional<QColor> &lastSystemAccent(bool dark)
{
    static std::optional<QColor> accent[2];
    return accent[dark ? 1 : 0];
}

// True while applyApp() installs its palette, whose change events are its own.
bool &applyingTheme()
{
    static bool applying = false;
    return applying;
}

} // namespace

void Theme::applyApp()
{
    if (!qApp)
        return;

    const Settings st = Settings::load();
    const QScopedValueRollback<bool> applying(applyingTheme(), true);

#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
    // Effective scheme, independent of any palette override applied below (Qt
    // tracks both the forced scheme from WindowManager::applyColorSchemeToQt and
    // the system's own switches).
    const Qt::ColorScheme scheme = QGuiApplication::styleHints()->colorScheme();
    const bool dark = (scheme == Qt::ColorScheme::Unknown)
                          ? theme::isDark(qApp->palette())
                          : scheme == Qt::ColorScheme::Dark;

    // Install the scheme's palette for painters not covered by QSS, so native
    // widgets match the tokens on every platform. It goes in twice. The first
    // time its accent roles are unset, so they resolve against the platform
    // palette and give the OS accent. The second time it carries the accent the
    // setting resolves to, the same one the stylesheet uses, so the style, item
    // views and page marks paint the same colour.
    const auto install = [dark](const QColor &accent) {
        qApp->setPalette(dark ? theme::darkPalette(accent) : theme::lightPalette(accent));
    };
    install(QColor());
    lastSystemAccent(dark) = platformAccent(qApp->palette());
    install(resolveAccent(st.accentColor, qApp->palette()));
#else
    lastSystemAccent(theme::isDark(qApp->palette())) = platformAccent(qApp->palette());
#endif

    const QPalette pal = qApp->palette();
    lastAppliedAccent() = resolveAccent(st.accentColor, pal);
    const QString assetDir = writeThemeAssets(theme::chrome(pal, lastAppliedAccent()));
    qApp->setStyleSheet(buildStyleSheet(pal, st.accentColor, assetDir));
}

QColor Theme::appliedAccent()
{
    return lastAppliedAccent();
}

QColor Theme::systemAccent(bool dark)
{
    // A scheme the application has not been in borrows the other scheme's reading.
    const std::optional<QColor> &own = lastSystemAccent(dark);
    const std::optional<QColor> &other = lastSystemAccent(!dark);
    const QColor os = own ? *own : other ? *other : platformAccent(QApplication::palette());
    return systemAccentFor(os, dark);
}

bool Theme::applying()
{
    return applyingTheme();
}

QColor Theme::iconInk(const QPalette &pal)
{
    return theme::chrome(pal).inkBody;
}

QColor Theme::accentColor(const QString &accentSetting, const QPalette &pal)
{
    return resolveAccent(accentSetting, pal);
}

namespace {

// Keeps a spin box's embedded editor inside the sheet's content rect.
//
// Qt 6.12.0 (qtbase 7d02beaf37, also on the 6.11 branch after 6.11.2) changed
// how the style sheet style sizes the editor: it cuts the field at the stepper
// buttons' x positions and starts it at x 0. Every styled spin box loses its
// border and left padding, and a NoButtons box, whose button rects are empty,
// gets a 1px editor that shows nothing. Qt 6.11.2 and older return the sheet's
// content rect minus the stepper column.
//
// QAbstractSpinBox re-applies the style's rect on every resize and style change,
// so the guard watches the editor's own Move/Resize events and corrects it. It
// moves the left edge in to the content rect and, for a NoButtons box, stretches
// the right edge out to it; the stepper column's cut stays Qt's. On Qt versions
// that already compute the content rect, the guard changes nothing.
class SpinBoxEditorGuard final : public QObject
{
public:
    explicit SpinBoxEditorGuard(QAbstractSpinBox *box)
        : QObject(box)
        , box_(box)
    {
        if (QLineEdit *edit = box->findChild<QLineEdit *>(QString(), Qt::FindDirectChildrenOnly)) {
            edit->installEventFilter(this);
            fit(edit);
        }
    }

    bool eventFilter(QObject *watched, QEvent *event) override
    {
        if (event->type() == QEvent::Move || event->type() == QEvent::Resize)
            fit(static_cast<QWidget *>(watched));
        return false;
    }

private:
    void fit(QWidget *edit) const
    {
        // The sheet's content rect (border + padding removed), the same rect a
        // QLineEdit puts its text in. Invalid when no rule styles the box.
        QStyleOption opt;
        opt.initFrom(box_);
        const QRect content = box_->style()->subElementRect(QStyle::SE_FrameContents, &opt, box_);
        if (!content.isValid())
            return;
        QRect want = edit->geometry();
        want.setLeft(qMax(want.left(), content.left()));
        if (box_->buttonSymbols() == QAbstractSpinBox::NoButtons)
            want.setRight(qMax(want.right(), content.right()));
        // The nested setGeometry re-enters fit() with want already in place.
        if (want.isValid() && want != edit->geometry())
            edit->setGeometry(want);
    }

    QAbstractSpinBox *box_;
};

} // namespace

void Theme::useTypedSpinBox(QAbstractSpinBox *box)
{
    if (!box)
        return;
    // No stepper arrows: the value is typed (or nudged with Up/Down and the
    // wheel, which QAbstractSpinBox keeps regardless of buttonSymbols). The
    // dynamic property is what drops the QSS stepper column and its reserved
    // padding - see the QAbstractSpinBox[noButtons] rules in buildStyleSheet.
    box->setButtonSymbols(QAbstractSpinBox::NoButtons);
    box->setProperty("noButtons", true);
    new SpinBoxEditorGuard(box);
}

void Theme::useSteppedSpinBox(QAbstractSpinBox *box)
{
    if (box)
        new SpinBoxEditorGuard(box);
}

} // namespace mervin
