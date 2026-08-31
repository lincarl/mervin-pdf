// Guard rails for the colour vocabulary (src/ui/ThemeTokens.h) and the one
// stylesheet built from it (src/ui/Theme.cpp).
//
// The load-bearing case is noStrayLiteralsInSheet(): it extracts every colour
// literal from the generated sheet and requires each one to be the css() of some
// token. That is what keeps colours out of Theme.cpp permanently - it fails the
// moment someone types a hex into a rule instead of adding a token.

#include "ui/Theme.h"
#include "ui/ThemeTokens.h"

#include <QApplication>
#include <QColor>
#include <QPalette>
#include <QRegularExpression>
#include <QSet>
#include <QString>
#include <QtTest>

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

} // namespace

class TstTheme : public QObject
{
    Q_OBJECT

private slots:
    void cssRendersBothForms();
    void isDarkAgreesWithThePalettes();
    void defaultAccentDiffersByTheme();
    void darkPaletteKeepsLabelsLegible();
    void noStrayLiteralsInSheet_data();
    void noStrayLiteralsInSheet();
    void typedSpinBoxRuleIsEmitted();
    void comboArrowIsDeclared();
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
    QVERIFY(!theme::isDark(fixedLightPalette()));
}

// A single fallback would put the light brand blue on the dark chrome.
void TstTheme::defaultAccentDiffersByTheme()
{
    QCOMPARE(theme::defaultAccent(true), QColor(0x4f, 0x8c, 0xff));
    QCOMPARE(theme::defaultAccent(false), QColor(0x00, 0x67, 0xc0));
    // An explicit accent must survive resolution untouched, and "system" must pick
    // the palette's own accent rather than a fallback.
    const QColor custom(0x12, 0x34, 0x56);
    const QPalette dark = theme::darkPalette(custom);
    QCOMPARE(Theme::accentColor(QStringLiteral("system"), dark), custom);
    QCOMPARE(Theme::accentColor(QStringLiteral("#ABCDEF"), dark), QColor(0xAB, 0xCD, 0xEF));
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

QTEST_MAIN(TstTheme)
#include "tst_theme.moc"
