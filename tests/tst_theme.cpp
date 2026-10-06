// Guard rails for the colour vocabulary (src/ui/ThemeTokens.h) and the one
// stylesheet built from it (src/ui/Theme.cpp).
//
// The load-bearing case is noStrayLiteralsInSheet(): it extracts every colour
// literal from the generated sheet and requires each one to be the css() of some
// token. That is what keeps colours out of Theme.cpp permanently - it fails the
// moment someone types a hex into a rule instead of adding a token.

#include "config/ConfigPaths.h"
#include "config/Settings.h"
#include "ui/Theme.h"
#include "ui/ThemeTokens.h"

#include <QApplication>
#include <QColor>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QLineEdit>
#include <QPalette>
#include <QProxyStyle>
#include <QRegularExpression>
#include <QScopeGuard>
#include <QSet>
#include <QToolButton>
#include <QSpinBox>
#include <QString>
#include <QStyle>
#include <QStyleFactory>
#include <QStyleOptionFrame>
#include <QTemporaryDir>
#include <QtTest>

#include <algorithm>
#include <cmath>
#include <memory>

using namespace mervin;

namespace {

// Every colour any token can produce, as it would appear in a stylesheet.
QSet<QString> tokenCssValues(const QPalette &pal, const QColor &accent)
{
    const theme::Chrome t = theme::chrome(pal, accent);
    QSet<QString> out;
    for (const QColor &c :
         {t.well, t.status, t.bar, t.window, t.fill, t.popover, t.canvas, t.inkPrimary, t.ink,
          t.inkBody, t.inkSoft, t.inkFaint, t.inkDisabled, t.inkMenuHeader, t.inkDanger, t.inkLink,
          t.border, t.borderStrong, t.borderBar, t.borderPush, t.borderDisabled, t.borderPopover,
          t.borderPopoverControl, t.hover, t.pressed, t.rowHover, t.separator, t.hairline,
          t.scrollThumb, t.scrollThumbHover, t.accent, t.accentHover, t.onAccent, t.accentWash})
        out.insert(theme::css(c));
    return out;
}

// A light palette that does not depend on the platform theme, so the assertions
// hold on any machine and in CI.
QPalette fixedLightPalette()
{
    QPalette p;
    p.setColor(QPalette::Window, QColor(Qt::white));
    p.setColor(QPalette::WindowText, QColor(0x1a, 0x1a, 0x1a));
    p.setColor(QPalette::Base, QColor(Qt::white));
    p.setColor(QPalette::Text, QColor(0x1a, 0x1a, 0x1a));
    p.setColor(QPalette::Accent, QColor(0x00, 0x67, 0xc0));
    p.setColor(QPalette::Highlight, QColor(0x00, 0x67, 0xc0));
    return p;
}

// The editor embedded in a spin box (QAbstractSpinBox::lineEdit() is protected).
QLineEdit *editorOf(const QAbstractSpinBox *box)
{
    return box->findChild<QLineEdit *>(QString(), Qt::FindDirectChildrenOnly);
}

// Where a plain QLineEdit's text area starts under the current sheet (its border
// plus left padding). A spin box's editor has to start at the same x, or typed
// values sit at a different inset than every other field.
int lineEditTextLeft(QLineEdit *reference)
{
    QStyleOptionFrame opt;
    opt.initFrom(reference);
    return reference->style()->subElementRect(QStyle::SE_LineEditContents, &opt, reference).left();
}

// WCAG contrast between `fg` and `bg`. A translucent foreground is composited over
// the background first, as the stylesheet draws it.
double contrastRatio(QColor fg, const QColor &bg)
{
    if (fg.alpha() < 255) {
        const double a = fg.alphaF();
        fg = QColor(qRound(fg.red() * a + bg.red() * (1 - a)),
                    qRound(fg.green() * a + bg.green() * (1 - a)),
                    qRound(fg.blue() * a + bg.blue() * (1 - a)));
    }
    const auto luminance = [](const QColor &c) {
        const auto lin = [](int v) {
            const double s = v / 255.0;
            return s <= 0.03928 ? s / 12.92 : std::pow((s + 0.055) / 1.055, 2.4);
        };
        return 0.2126 * lin(c.red()) + 0.7152 * lin(c.green()) + 0.0722 * lin(c.blue());
    };
    const double lf = luminance(fg);
    const double lb = luminance(bg);
    return (std::max(lf, lb) + 0.05) / (std::min(lf, lb) + 0.05);
}

// Fusion's palette, which Qt uses for the roles a platform theme leaves unset.
QPalette fusionPalette()
{
    const std::unique_ptr<QStyle> fusion(QStyleFactory::create(QStringLiteral("Fusion")));
    return fusion->standardPalette();
}

// A style that puts a desktop accent into the platform palette, as the Windows 11
// style does. It stands in for a desktop accent under the offscreen test platform,
// whose own palette is Fusion's. Its standard palette carries the accent too, as
// Breeze's does on KDE, so "the style's default" can't pass for no accent.
class DesktopAccentStyle : public QProxyStyle
{
public:
    explicit DesktopAccentStyle(const QColor &accent)
        : QProxyStyle(QStyleFactory::create(QStringLiteral("Fusion"))), accent_(accent)
    {
    }

    QPalette standardPalette() const override
    {
        QPalette pal = QProxyStyle::standardPalette();
        pal.setColor(QPalette::Accent, accent_);
        pal.setColor(QPalette::Highlight, accent_);
        return pal;
    }

    using QProxyStyle::polish;
    void polish(QPalette &pal) override
    {
        QProxyStyle::polish(pal);
        pal.setColor(QPalette::Accent, accent_);
        pal.setColor(QPalette::Highlight, accent_);
    }

private:
    QColor accent_;
};

// The difference between two hues in degrees, around the colour wheel.
int hueDistance(const QColor &a, const QColor &b)
{
    const int d = std::abs(a.hslHue() - b.hslHue());
    return std::min(d, 360 - d);
}

// Applies the real application sheet to qApp for the lifetime of the returned guard.
auto applyAppSheet()
{
    qApp->setStyleSheet(Theme::buildStyleSheet(theme::darkPalette(QColor(0x4f, 0x8c, 0xff)),
                                               QStringLiteral("system"), QStringLiteral("/glyphs")));
    return qScopeGuard([] { qApp->setStyleSheet(QString()); });
}

} // namespace

class TstTheme : public QObject
{
    Q_OBJECT

private slots:
    void cssRendersBothForms();
    void isDarkAgreesWithThePalettes();
    void transparentPaletteFollowsTheApplication();
    void defaultAccentDiffersByTheme();
    void palettesLeaveTheSystemAccentToThePlatform_data();
    void palettesLeaveTheSystemAccentToThePlatform();
    void systemAccentReadsTheDesktopColour();
    void systemAccentStaysLegible();
    void applyAppInstallsTheChosenAccent_data();
    void applyAppInstallsTheChosenAccent();
    void themesMeetContrast_data();
    void themesMeetContrast();
    void accentTextPicksTheLegibleInk();
    void pageMarksStayVisibleOnThePaper();
    void disabledChecksKeepAVisibleMark();
    void darkPaletteKeepsLabelsLegible();
    void noStrayLiteralsInSheet_data();
    void noStrayLiteralsInSheet();
    void typedSpinBoxRuleIsEmitted();
    void spinBoxEditorsShowTheirValues();
    void spinBoxEditorsRecoverFromQt612Geometry();
    void comboArrowIsDeclared();
    void settingsMenuSelectionDiffersFromHover();
};

// css() has to produce exactly the two forms the sheets used before the tokens
// existed: a bare hex when opaque, rgba() with three decimals when not.
void TstTheme::cssRendersBothForms()
{
    QCOMPARE(theme::css(QColor(0x21, 0x28, 0x34)), QStringLiteral("#212834"));
    QColor wash(Qt::white);
    wash.setAlphaF(0.10);
    QCOMPARE(theme::css(wash), QStringLiteral("rgba(255,255,255,0.100)"));
}

void TstTheme::isDarkAgreesWithThePalettes()
{
    QVERIFY(theme::isDark(theme::darkPalette(QColor(0x4f, 0x8c, 0xff))));
    QVERIFY(!theme::isDark(theme::lightPalette(QColor())));
    QVERIFY(!theme::isDark(fixedLightPalette()));
}

// The stylesheet gives a "background: transparent" widget a transparent Window
// colour, whose lightness is 0. Read literally, every light-mode tool button would
// count as dark and tint its glyphs with the dark theme's pale ink, as the toolbar's
// Open/Save/Document dropdown arrows did. Such a palette follows the application.
void TstTheme::transparentPaletteFollowsTheApplication()
{
    const QPalette saved = qApp->palette();
    const auto restore = qScopeGuard([&saved] {
        qApp->setStyleSheet(QString());
        qApp->setPalette(saved);
    });

    QPalette transparent = fixedLightPalette();
    transparent.setColor(QPalette::Window, Qt::transparent);
    qApp->setPalette(theme::darkPalette(QColor()));
    QVERIFY(theme::isDark(transparent));
    qApp->setPalette(theme::lightPalette(QColor()));
    QVERIFY(!theme::isDark(transparent));

    // The real case: a toolbar button polished by the light sheet.
    qApp->setStyleSheet(Theme::buildStyleSheet(qApp->palette(), QStringLiteral("system"),
                                               QStringLiteral("/glyphs")));
    QToolButton button;
    button.ensurePolished();
    QVERIFY(!theme::isDark(button.palette()));
    QCOMPARE(Theme::iconInk(button.palette()), theme::chrome(qApp->palette()).inkBody);
}

// A single fallback would put a mid blue under dark button text, or a pale one on white.
void TstTheme::defaultAccentDiffersByTheme()
{
    QCOMPARE(theme::defaultAccent(true), QColor(0x88, 0xc0, 0xd0));
    QCOMPARE(theme::defaultAccent(false), QColor(0x09, 0x69, 0xda));
    // An explicit accent must survive resolution untouched, and "system" must pick
    // the palette's own accent rather than a fallback.
    const QColor custom(0x4f, 0x8c, 0xff);
    const QPalette dark = theme::darkPalette(custom);
    QCOMPARE(Theme::accentColor(QStringLiteral("system"), dark), custom);
    QCOMPARE(Theme::accentColor(QStringLiteral("#ABCDEF"), dark), QColor(0xAB, 0xCD, 0xEF));
}

void TstTheme::palettesLeaveTheSystemAccentToThePlatform_data()
{
    QTest::addColumn<bool>("dark");
    QTest::newRow("dark") << true;
    QTest::newRow("light") << false;
}

// With the accent setting on "system", neither palette may pin an accent, or the
// OS accent could never come through. Dark used to pin Nord's frost blue, so "Use
// the system accent colour" changed nothing there. A chosen accent is set like any role.
void TstTheme::palettesLeaveTheSystemAccentToThePlatform()
{
    QFETCH(bool, dark);
    const auto paletteWith = [dark](const QColor &accent) {
        return dark ? theme::darkPalette(accent) : theme::lightPalette(accent);
    };
    const QPalette system = paletteWith(QColor());
    for (QPalette::ColorRole role : {QPalette::Accent, QPalette::Highlight, QPalette::HighlightedText})
        QVERIFY2(!system.isBrushSet(QPalette::Active, role), "an accent role is pinned");
    QVERIFY(system.isBrushSet(QPalette::Active, QPalette::Window));

    // Installed, the palette takes the platform's accent roles.
    const QPalette saved = qApp->palette();
    const auto restore = qScopeGuard([&saved] { qApp->setPalette(saved); });
    qApp->setPalette(QPalette()); // nothing set: exactly the platform palette
    const QPalette platform = qApp->palette();
    qApp->setPalette(system);
    QCOMPARE(theme::isDark(qApp->palette()), dark);
    for (QPalette::ColorRole role : {QPalette::Accent, QPalette::Highlight})
        QCOMPARE(qApp->palette().color(role), platform.color(role));

    // An accent that reads in the scheme, so "system" takes it as it is.
    const QColor custom = dark ? QColor(0x4f, 0x8c, 0xff) : QColor(0x12, 0x34, 0x56);
    const QPalette chosen = paletteWith(custom);
    QCOMPARE(chosen.color(QPalette::Accent), custom);
    QCOMPARE(Theme::accentColor(QStringLiteral("system"), chosen), custom);
}

// Qt's Windows theme reports the OS accent as Accent, and Plasma's platform theme
// sets both roles. Qt's GTK and built-in KDE themes report it only as Highlight and
// leave Accent at Fusion's #308cc6 on every desktop, which "system" used to show. Without a desktop accent (Fusion's
// in both roles, or the white Qt makes up for a KDE profile without colours), each
// theme keeps its design accent.
void TstTheme::systemAccentReadsTheDesktopColour()
{
    const QPalette fusion = fusionPalette();
    const auto resolved = [](bool dark, const QColor &accent, const QColor &highlight) {
        QPalette pal = dark ? theme::darkPalette(QColor()) : theme::lightPalette(QColor());
        pal.setColor(QPalette::Accent, accent);
        pal.setColor(QPalette::Highlight, highlight);
        return Theme::accentColor(QStringLiteral("system"), pal);
    };
    const QColor yaruOrange(0xe9, 0x54, 0x20);
    const QColor windowsDarkShade(0x76, 0xb9, 0xed);
    const QColor breezeBlue(0x3d, 0xae, 0xe9);
    QCOMPARE(resolved(false, fusion.color(QPalette::Accent), yaruOrange), yaruOrange);
    QCOMPARE(resolved(true, windowsDarkShade, QColor(0x00, 0x78, 0xd4)), windowsDarkShade);
    QCOMPARE(resolved(true, breezeBlue, breezeBlue), breezeBlue);
    for (bool dark : {true, false}) {
        const QColor fusionHighlight = fusion.color(QPalette::Highlight);
        QCOMPARE(resolved(dark, fusion.color(QPalette::Accent), fusionHighlight),
                 theme::defaultAccent(dark));
        QCOMPARE(resolved(dark, QColor(Qt::white), fusionHighlight), theme::defaultAccent(dark));
    }
}

// A desktop may tune its accent for the other scheme. Under a contrast theme
// Windows reports its light mode shade, a dark blue at 1.8:1 on Nord. The OS accent
// moves to the 3:1 the design accents keep, in the same hue; one that already reads
// is left alone.
void TstTheme::systemAccentStaysLegible()
{
    const QColor accents[] = {QColor(0x00, 0x5a, 0x9e), QColor(0x74, 0x60, 0xd4),
                              QColor(0x99, 0xeb, 0xff), QColor(0xe9, 0x54, 0x20),
                              QColor(0xff, 0xd4, 0x00), QColor(0x5b, 0x2c, 0x8f)};
    for (bool dark : {true, false}) {
        const theme::Chrome t =
            theme::chrome(dark ? theme::darkPalette(QColor()) : theme::lightPalette(QColor()), QColor());
        for (const QColor &os : accents) {
            const QColor c = theme::legibleAccent(os, dark);
            const QString what = QStringLiteral("%1 in %2 became %3")
                                     .arg(os.name(), dark ? QStringLiteral("dark") : QStringLiteral("light"),
                                          c.name());
            QVERIFY2(contrastRatio(c, t.window) >= 3.0 && contrastRatio(c, t.well) >= 3.0,
                     qPrintable(what));
            QVERIFY2(hueDistance(c, os) <= 4, qPrintable(what));
            if (contrastRatio(os, t.window) >= 3.0 && contrastRatio(os, t.well) >= 3.0)
                QCOMPARE(c, os);
        }
    }
    const QColor lightModeShade(0x00, 0x5a, 0x9e);
    QPalette contrastTheme = theme::darkPalette(QColor());
    contrastTheme.setColor(QPalette::Accent, lightModeShade);
    const QColor resolved = Theme::accentColor(QStringLiteral("system"), contrastTheme);
    QVERIFY(resolved != lightModeShade);
    QCOMPARE(resolved, theme::legibleAccent(lightModeShade, true));
}

void TstTheme::applyAppInstallsTheChosenAccent_data()
{
    QTest::addColumn<bool>("dark");
    QTest::newRow("dark") << true;
    QTest::newRow("light") << false;
}

// applyApp() reads the OS accent from the platform palette, then installs the
// accent the setting picks, so painters that read the palette (the style, item
// views, page marks) agree with the stylesheet. Dark used to install Nord's frost
// blue whatever the desktop reported. The OS accent stays known under a custom
// accent, which Settings previews when "system" is ticked again.
void TstTheme::applyAppInstallsTheChosenAccent()
{
    QFETCH(bool, dark);
    QTemporaryDir profile;
    QVERIFY(profile.isValid());
    ConfigPaths::setOverrideDir(profile.path());
    const QString styleName = QApplication::style()->name();
    const QPalette saved = qApp->palette();
    const auto restore = qScopeGuard([&styleName, &saved] {
        ConfigPaths::setOverrideDir({});
        qApp->setStyleSheet(QString());
        QStyle *style = QStyleFactory::create(styleName);
        QApplication::setStyle(style ? style : QStyleFactory::create(QStringLiteral("Fusion")));
        qApp->setPalette(saved);
    });

    // A desktop purple too dark for Nord, so dark also goes through the contrast floor.
    const QColor desktop(0x5b, 0x3c, 0xc4);
    QApplication::setStyle(new DesktopAccentStyle(desktop));
    qApp->setPalette(dark ? theme::darkPalette(QColor()) : theme::lightPalette(QColor()));
    const QColor expected = theme::legibleAccent(desktop, dark);
    const auto applyWith = [](const QString &accent) {
        Settings st = Settings::load();
        st.accentColor = accent;
        QVERIFY(st.save());
        Theme::applyApp();
    };

    applyWith(QStringLiteral("system"));
    const QPalette &pal = qApp->palette();
    QCOMPARE(theme::isDark(pal), dark);
    QVERIFY(pal.isBrushSet(QPalette::Active, QPalette::Accent));
    QCOMPARE(pal.color(QPalette::Accent), expected);
    QCOMPARE(pal.color(QPalette::Highlight), expected);
    QCOMPARE(theme::chrome(pal).accent, expected);
    QCOMPARE(Theme::appliedAccent(), expected);
    QCOMPARE(Theme::systemAccent(dark), expected);
    QCOMPARE(Theme::systemAccent(!dark), theme::legibleAccent(desktop, !dark));

    const QColor custom(0x12, 0x34, 0x56);
    applyWith(custom.name());
    QCOMPARE(qApp->palette().color(QPalette::Accent), custom);
    QCOMPARE(Theme::appliedAccent(), custom);
    QCOMPARE(Theme::systemAccent(dark), expected);

    // The custom accent installed last time must not pass for the OS accent.
    applyWith(QStringLiteral("system"));
    QCOMPARE(qApp->palette().color(QPalette::Accent), expected);
    QCOMPARE(Theme::systemAccent(dark), expected);

    // A hand-edited value that is no colour gets the design accent, in the palette
    // as well as the stylesheet.
    applyWith(QStringLiteral("#12345"));
    QCOMPARE(qApp->palette().color(QPalette::Accent), theme::defaultAccent(dark));
    QCOMPARE(Theme::appliedAccent(), theme::defaultAccent(dark));

    // Each scheme keeps the accent the desktop reported there: Windows uses
    // another shade in each, and GTK may report none for a forced scheme.
    const QColor otherShade(0x1f, 0x7a, 0x4d);
    QApplication::setStyle(new DesktopAccentStyle(otherShade));
    qApp->setPalette(dark ? theme::lightPalette(QColor()) : theme::darkPalette(QColor()));
    applyWith(QStringLiteral("system"));
    QCOMPARE(Theme::systemAccent(!dark), theme::legibleAccent(otherShade, !dark));
    QCOMPARE(Theme::systemAccent(dark), expected);
}

void TstTheme::themesMeetContrast_data()
{
    QTest::addColumn<bool>("dark");
    QTest::newRow("dark") << true;
    QTest::newRow("light") << false;
}

// The text pairs each scheme was designed to: body text at 7:1, secondary and meta
// text (page totals, the status bar, menu captions) at 4.5:1, the default button's
// label at 4.5:1 in both states, and an accent that reads as a focus ring. Dark also
// caps body text, since the point of that theme is comfort, not maximum contrast.
// The previous dark theme failed four of these.
void TstTheme::themesMeetContrast()
{
    QFETCH(bool, dark);
    const QPalette pal = dark ? theme::darkPalette(QColor()) : theme::lightPalette(QColor());
    const theme::Chrome t = theme::chrome(pal, theme::defaultAccent(dark));
    QCOMPARE(t.dark, dark);

    const struct {
        const char *what;
        QColor fg, bg;
        double min;
    } pairs[] = {
        {"body text on window", t.ink, t.window, 7.0},
        {"input text in well", t.inkPrimary, t.well, 7.0},
        {"toolbar ink on window", t.inkBody, t.window, 4.5},
        {"tab ink on bar", t.inkBody, t.bar, 4.5},
        {"secondary on window", t.inkSoft, t.window, 4.5},
        {"meta on window", t.inkFaint, t.window, 4.5},
        {"meta on status bar", t.inkFaint, t.status, 4.5},
        {"menu caption on popover", t.inkMenuHeader, t.popover, 4.5},
        {"menu text on popover", t.ink, t.popover, 7.0},
        {"link on popover", t.inkLink, t.popover, 4.5},
        {"danger on window", t.inkDanger, t.window, 4.5},
        {"button text on accent", t.onAccent, t.accent, 4.5},
        {"button text on accent hover", t.onAccent, t.accentHover, 4.5},
        {"accent on window", t.accent, t.window, 3.0},
        {"accent in well", t.accent, t.well, 3.0},
    };
    for (const auto &p : pairs) {
        const double r = contrastRatio(p.fg, p.bg);
        QVERIFY2(r >= p.min, qPrintable(QStringLiteral("%1: %2:1, want %3:1")
                                            .arg(QLatin1String(p.what)).arg(r, 0, 'f', 2)
                                            .arg(p.min)));
    }
    if (dark) {
        const double body = contrastRatio(t.ink, t.window);
        QVERIFY2(body <= 14.5, qPrintable(QStringLiteral("dark body text glares at %1:1").arg(body)));
    }
}

// The default button's label takes whichever of white or the theme's dark ink reads
// better on the accent. White on the old #4f8cff measured 3.2:1; a light accent gets
// dark text, a deep one white, in both themes and for custom accents too.
void TstTheme::accentTextPicksTheLegibleInk()
{
    const QColor accents[] = {QColor(0x4f, 0x8c, 0xff), QColor(0x88, 0xc0, 0xd0),
                              QColor(0x09, 0x69, 0xda), QColor(0x00, 0x78, 0xd4),
                              QColor(0xff, 0xd4, 0x00), QColor(0x5b, 0x2c, 0x8f)};
    for (bool dark : {true, false}) {
        const QPalette pal = dark ? theme::darkPalette(QColor()) : theme::lightPalette(QColor());
        for (const QColor &accent : accents) {
            const theme::Chrome t = theme::chrome(pal, accent);
            const double chosen = contrastRatio(t.onAccent, accent);
            const double white = contrastRatio(QColor(Qt::white), accent);
            QVERIFY2(chosen >= white && chosen >= 4.0,
                     qPrintable(QStringLiteral("%1 in %2: label %3 at %4:1, white %5:1")
                                    .arg(accent.name(), dark ? QStringLiteral("dark") : QStringLiteral("light"),
                                         t.onAccent.name())
                                    .arg(chosen, 0, 'f', 2).arg(white, 0, 'f', 2)));
            // The palette's selected-text role agrees with the sheet's button label.
            const QPalette withAccent = dark ? theme::darkPalette(accent) : theme::lightPalette(accent);
            QCOMPARE(withAccent.color(QPalette::HighlightedText), t.onAccent);
        }
    }
}

// Measurements and the annotation outline are drawn in the accent on the page.
// Nord's frost accent is 1.8:1 on white paper, so marks fall back to the light
// theme's accent there; an accent that already reads is kept, custom ones too.
void TstTheme::pageMarksStayVisibleOnThePaper()
{
    const theme::Doc &d = theme::doc();
    const QColor frost = theme::defaultAccent(true);
    const QColor slate = theme::defaultAccent(false);
    for (const QColor &paper : {d.paperNormal, d.paperInverted, d.paperComfort}) {
        for (const QColor &accent : {frost, slate, QColor(0xff, 0xd4, 0x00), QColor(0x5b, 0x2c, 0x8f)}) {
            const QColor mark = theme::pageAccent(accent, paper);
            const double r = contrastRatio(mark, paper);
            QVERIFY2(r >= 3.0, qPrintable(QStringLiteral("%1 on %2 paper resolves to %3 at %4:1")
                                              .arg(accent.name(), paper.name(), mark.name())
                                              .arg(r, 0, 'f', 2)));
            if (contrastRatio(accent, paper) >= 3.0)
                QCOMPARE(mark, accent);
        }
    }
    QCOMPARE(theme::pageAccent(frost, d.paperNormal), slate);
    QCOMPARE(theme::pageAccent(frost, d.paperInverted), frost);
}

// A disabled checked box loses the accent fill to the faint disabled wash, where
// the accent's dark text colour vanished (1.5:1). Its mark gets its own image.
void TstTheme::disabledChecksKeepAVisibleMark()
{
    const QPalette pal = theme::darkPalette(QColor());
    const QString sheet = Theme::buildStyleSheet(pal, QStringLiteral("system"), QStringLiteral("/glyphs"));
    QVERIFY(sheet.contains(QStringLiteral("QCheckBox::indicator:checked:disabled {"
                                          " image:url(/glyphs/check_box_off.png); }")));
    QVERIFY(sheet.contains(QStringLiteral("QRadioButton::indicator:checked:disabled {"
                                          " image:url(/glyphs/radio_dot_off.png); }")));
    const theme::Chrome t = theme::chrome(pal);
    QColor wash = t.borderDisabled;
    const double a = wash.alphaF();
    const QColor fill(qRound(wash.red() * a + t.window.red() * (1 - a)),
                      qRound(wash.green() * a + t.window.green() * (1 - a)),
                      qRound(wash.blue() * a + t.window.blue() * (1 - a)));
    QVERIFY2(contrastRatio(t.inkFaint, fill) >= 3.0, "the disabled mark's ink is too faint");
}

// The regression this whole change came from: a floating panel's label ink is
// QPalette::Light whenever an ancestor's background role is Dark or Shadow, and
// Light in the slate palette is #2a313a - unreadable on the popover surface. The
// role is anchored at the viewport now, but the palette must ALSO keep Light far
// enough from the surface that the failure would be visible rather than subtle.
void TstTheme::darkPaletteKeepsLabelsLegible()
{
    const QPalette pal = theme::darkPalette(QColor(0x4f, 0x8c, 0xff));
    const theme::Chrome t = theme::chrome(pal);

    const auto luminance = [](const QColor &c) {
        const auto lin = [](int v) {
            const double s = v / 255.0;
            return s <= 0.03928 ? s / 12.92 : std::pow((s + 0.055) / 1.055, 2.4);
        };
        return 0.2126 * lin(c.red()) + 0.7152 * lin(c.green()) + 0.0722 * lin(c.blue());
    };
    const auto ratio = [&luminance](const QColor &a, const QColor &b) {
        const double la = luminance(a);
        const double lb = luminance(b);
        return (std::max(la, lb) + 0.05) / (std::min(la, lb) + 0.05);
    };

    // Every ink a panel label can legitimately resolve to clears AA body text on
    // the popover surface.
    QVERIFY2(ratio(pal.color(QPalette::WindowText), t.popover) > 4.5, "WindowText on popover");
    QVERIFY2(ratio(t.inkPrimary, t.popover) > 4.5, "inkPrimary on popover");
    QVERIFY2(ratio(t.inkBody, t.popover) > 4.5, "inkBody on popover");
    QVERIFY2(ratio(t.inkSoft, t.popover) > 4.5, "inkSoft on popover");
    // ...and the bevel role that the bug substituted does not, which is why it has
    // to be reached through a role anchor and never by accident.
    QVERIFY2(ratio(pal.color(QPalette::Light), t.popover) < 2.0,
             "QPalette::Light must stay a bevel colour, not a text colour");
}

void TstTheme::noStrayLiteralsInSheet_data()
{
    QTest::addColumn<QPalette>("palette");
    QTest::addColumn<QColor>("accent");
    QTest::newRow("dark") << theme::darkPalette(QColor(0x4f, 0x8c, 0xff))
                          << QColor(0x4f, 0x8c, 0xff);
    QTest::newRow("light") << fixedLightPalette() << QColor(0x00, 0x67, 0xc0);
}

// No colour may be written into a rule: every literal in the sheet has to be a
// token. Without this the vocabulary silently rots back into scattered hexes.
void TstTheme::noStrayLiteralsInSheet()
{
    QFETCH(QPalette, palette);
    QFETCH(QColor, accent);

    const QString sheet =
        Theme::buildStyleSheet(palette, accent.name(QColor::HexRgb), QStringLiteral("/glyphs"));
    QVERIFY(!sheet.isEmpty());

    const QSet<QString> allowed = tokenCssValues(palette, accent);
    static const QRegularExpression literal(
        QStringLiteral("#[0-9a-fA-F]{6}\\b|rgba?\\([^)]*\\)"));

    QStringList stray;
    int seen = 0;
    auto it = literal.globalMatch(sheet);
    while (it.hasNext()) {
        const QString found = it.next().captured(0);
        ++seen;
        if (!allowed.contains(found))
            stray << found;
    }
    // Guard against the check quietly becoming vacuous: the sheet is built almost
    // entirely out of colours, so a handful of matches means the regex broke.
    QVERIFY2(seen > 50, "the literal scan found almost nothing - the regex is wrong");
    if (!stray.isEmpty())
        qWarning() << "colour literals not backed by a token:" << stray;
    QVERIFY2(stray.isEmpty(),
             "every colour in the sheet must come from theme::chrome() - add a token "
             "in ThemeTokens.cpp instead of writing a literal into a rule");
}

// Theme::useTypedSpinBox sets a dynamic property; the matching rule has to exist
// or the 22px stepper reservation stays and squeezes the value out of the field.
void TstTheme::typedSpinBoxRuleIsEmitted()
{
    const QString sheet = Theme::buildStyleSheet(theme::darkPalette(QColor(Qt::blue)),
                                                 QStringLiteral("system"),
                                                 QStringLiteral("/glyphs"));
    QVERIFY(sheet.contains(QStringLiteral("QAbstractSpinBox[noButtons=\"true\"]")));
    QVERIFY(sheet.contains(QStringLiteral("padding-right:8px")));
    // The spin box's embedded line edit must be reset, or the shared input rule
    // draws a second border and 8px of padding inside the frame.
    QVERIFY(sheet.contains(QStringLiteral("QSpinBox QLineEdit")));
}

// Under the real sheet, every spin box editor must be wide enough for its value
// and start at the same inset as a QLineEdit. Qt 6.12.0 broke both: a typed
// (NoButtons) box got a 1px editor, so Settings showed blank "Recent files" fields,
// and a stepper box's text sat against its left border. On Qt 6.12 this test fails
// without the Theme helpers' editor guard.
void TstTheme::spinBoxEditorsShowTheirValues()
{
    const auto resetSheet = applyAppSheet();

    QWidget host;
    auto *form = new QFormLayout(&host);
    // Settings' snug form keeps fields at their size hint, the tightest case.
    form->setFieldGrowthPolicy(QFormLayout::FieldsStayAtSizeHint);
    auto *reference = new QLineEdit(QStringLiteral("500"), &host);
    auto *count = new QSpinBox(&host);
    count->setRange(1, 1000000);
    count->setValue(500);
    Theme::useTypedSpinBox(count);
    auto *length = new QDoubleSpinBox(&host);
    length->setRange(0.001, 1.0e9);
    length->setDecimals(3);
    length->setValue(1000.0);
    Theme::useTypedSpinBox(length);
    auto *copies = new QSpinBox(&host);
    copies->setRange(1, 999);
    copies->setValue(999);
    Theme::useSteppedSpinBox(copies);
    form->addRow(QStringLiteral("Line edit"), reference);
    form->addRow(QStringLiteral("Typed int"), count);
    form->addRow(QStringLiteral("Typed double"), length);
    form->addRow(QStringLiteral("Stepper"), copies);
    host.show();
    QVERIFY(QTest::qWaitForWindowExposed(&host));

    const int inset = lineEditTextLeft(reference);
    QVERIFY2(inset >= 8, "the sheet's 8px input padding is missing from QLineEdit");
    for (QAbstractSpinBox *box : {static_cast<QAbstractSpinBox *>(count),
                                  static_cast<QAbstractSpinBox *>(length),
                                  static_cast<QAbstractSpinBox *>(copies)}) {
        const QLineEdit *edit = editorOf(box);
        QVERIFY(edit);
        const QByteArray what = box->text().toUtf8() + " editor " + QByteArray::number(edit->x())
                                + "+" + QByteArray::number(edit->width());
        QVERIFY2(edit->width() >= edit->fontMetrics().horizontalAdvance(box->text()), what.constData());
        QVERIFY2(edit->x() == inset, what.constData());
        QVERIFY2(edit->geometry().right() < box->width(), what.constData());
    }

    // Typed, not stepped: no stepper column, but Up still steps the value.
    QCOMPARE(count->buttonSymbols(), QAbstractSpinBox::NoButtons);
    count->setFocus();
    QTest::keyClick(count, Qt::Key_Up);
    QCOMPARE(count->value(), 501);
}

// The same check on any Qt version: force the geometry Qt 6.12.0 computes (a 1px
// typed editor at x 0, a stepper editor at x 0) and require the helpers to put the
// editors back. A spin box that skipped the helpers keeps the broken geometry,
// which is what the check is there to catch.
void TstTheme::spinBoxEditorsRecoverFromQt612Geometry()
{
    const auto resetSheet = applyAppSheet();

    QWidget host;
    auto *form = new QFormLayout(&host);
    form->setFieldGrowthPolicy(QFormLayout::FieldsStayAtSizeHint);
    auto *typed = new QSpinBox(&host);
    typed->setRange(1, 1000000);
    typed->setValue(500);
    Theme::useTypedSpinBox(typed);
    auto *stepper = new QSpinBox(&host);
    stepper->setRange(1, 999);
    stepper->setValue(999);
    Theme::useSteppedSpinBox(stepper);
    auto *unguarded = new QSpinBox(&host);
    unguarded->setRange(1, 1000000);
    unguarded->setValue(500);
    unguarded->setButtonSymbols(QAbstractSpinBox::NoButtons);
    unguarded->setProperty("noButtons", true);
    form->addRow(QStringLiteral("Typed"), typed);
    form->addRow(QStringLiteral("Stepper"), stepper);
    form->addRow(QStringLiteral("Unguarded"), unguarded);
    host.show();
    QVERIFY(QTest::qWaitForWindowExposed(&host));

    QLineEdit *typedEdit = editorOf(typed);
    QLineEdit *stepperEdit = editorOf(stepper);
    QLineEdit *unguardedEdit = editorOf(unguarded);
    const QRect typedRect = typedEdit->geometry();
    const QRect stepperRect = stepperEdit->geometry();
    const int textWidth = typedEdit->fontMetrics().horizontalAdvance(typed->text());

    typedEdit->setGeometry(QRect(0, typedRect.y(), 1, typedRect.height()));
    QCOMPARE(typedEdit->geometry(), typedRect);
    QVERIFY(typedEdit->width() >= textWidth);

    stepperEdit->setGeometry(QRect(QPoint(0, stepperRect.y()), stepperRect.bottomRight()));
    QCOMPARE(stepperEdit->x(), stepperRect.x());

    // Negative control: without the guard the collapsed editor stays collapsed,
    // so the width check above really does detect a hidden value.
    const QRect unguardedRect = unguardedEdit->geometry();
    unguardedEdit->setGeometry(QRect(0, unguardedRect.y(), 1, unguardedRect.height()));
    QVERIFY(unguardedEdit->width() < textWidth);
}

// Styling QComboBox::drop-down suppresses Qt's native arrow, so a combo shows no
// affordance at all unless the sheet supplies one.
void TstTheme::comboArrowIsDeclared()
{
    const QPalette pal = theme::darkPalette(QColor(Qt::blue));
    const QString withGlyphs =
        Theme::buildStyleSheet(pal, QStringLiteral("system"), QStringLiteral("/glyphs"));
    QVERIFY(withGlyphs.contains(QStringLiteral("QComboBox::down-arrow")));
    QVERIFY(withGlyphs.contains(QStringLiteral("/glyphs/combo_arrow.png")));

    // With no glyph directory (headless), the drop-down must be left alone so Qt
    // still draws its own arrow rather than nothing.
    const QString bare = Theme::buildStyleSheet(pal, QStringLiteral("system"));
    QVERIFY(!bare.contains(QStringLiteral("down-arrow")));
}

// The Settings menu keeps a selection. Its selected row needs a fill clearly
// above the hover wash, or the two look alike (they were 1.04:1 in the mockups).
void TstTheme::settingsMenuSelectionDiffersFromHover()
{
    const QPalette pal = theme::darkPalette(QColor(0x4f, 0x8c, 0xff));
    const theme::Chrome t = theme::chrome(pal);
    const QString sheet =
        Theme::buildStyleSheet(pal, QStringLiteral("system"), QStringLiteral("/glyphs"));
    QVERIFY(sheet.contains(QStringLiteral("QListWidget#settingsNav::item:selected { background:%1;")
                               .arg(theme::css(t.pressed))));
    QVERIFY(sheet.contains(QStringLiteral("QListWidget#settingsNav::item:hover { background:%1; }")
                               .arg(theme::css(t.rowHover))));
    QVERIFY(t.pressed.alphaF() - t.rowHover.alphaF() >= 0.04);
}

QTEST_MAIN(TstTheme)
#include "tst_theme.moc"
