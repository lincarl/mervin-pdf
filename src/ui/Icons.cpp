#include "ui/Icons.h"

#include <QAbstractButton>
#include <QByteArray>
#include <QColor>
#include <QEvent>
#include <QFile>
#include <QHash>
#include <QPainter>
#include <QPalette>
#include <QPixmap>
#include <QRect>
#include <QRectF>
#include <QSize>
#include <QSvgRenderer>
#include <QToolButton>

namespace mervin::icons {

namespace {

// Lucide draws every icon with a 2-unit stroke on a 24-unit grid; the app draws
// them a little lighter, closer to the weight of its text. One number, so the
// whole set's weight can be tuned in a single place.
constexpr double kLucideStroke = 2.0; // what the vendored files say
constexpr double kStroke = 1.75;      // what the app draws

// The sizes QIcon is populated with. Each is rendered natively from the SVG, not
// scaled from one pixmap, so a stroke never lands between pixels by accident.
constexpr int kSizes[] = {16, 20, 24, 32, 48};

// The Lucide file each glyph is drawn from, under resources/icons/lucide.
const char *lucideName(Glyph id)
{
    switch (id) {
    case Glyph::Open: return "folder-open";
    case Glyph::PrevPage: return "chevron-left";
    case Glyph::NextPage: return "chevron-right";
    case Glyph::ChevronDown: return "chevron-down";
    case Glyph::Search: return "search";
    case Glyph::ZoomOut: return "minus";
    case Glyph::ZoomIn: return "plus";
    case Glyph::FitMode: return "fullscreen";
    case Glyph::RotateLeft: return "rotate-ccw";
    case Glyph::RotateRight: return "rotate-cw";
    case Glyph::Print: return "printer";
    case Glyph::Copy: return "copy";
    case Glyph::Save: return "save";
    case Glyph::FillForm: return "pencil-line";
    case Glyph::Ocr: return "scan-text";
    case Glyph::Measure: return "ruler-dimension-line";
    case Glyph::Document: return "file";
    case Glyph::Menu: return "menu";
    case Glyph::FitPage: return "shrink";
    case Glyph::FitWidth: return "move-horizontal";
    case Glyph::FullScreen: return "maximize";
    case Glyph::ContinuousScroll: return "gallery-vertical";
    case Glyph::SinglePage: return "rectangle-vertical";
    case Glyph::TwoPageSpread: return "book-open";
    case Glyph::Outline: return "list";
    case Glyph::Thumbnails: return "layout-grid";
    case Glyph::Comments: return "message-square";
    case Glyph::SelectAll: return "square-dashed";
    case Glyph::HighlightFields: return "highlighter";
    case Glyph::UiTheme: return "moon";
    case Glyph::Sun: return "sun";
    case Glyph::DocumentTheme: return "contrast";
    case Glyph::AlwaysOnTop: return "pin";
    case Glyph::Settings: return "settings";
    case Glyph::Keyboard: return "keyboard";
    case Glyph::About: return "info";
    case Glyph::ExtractPages: return "file-output";
    case Glyph::SplitPages: return "split";
    case Glyph::MergePages: return "merge";
    case Glyph::Security: return "lock";
    case Glyph::Delete: return "trash-2";
    case Glyph::OpenInNewWindow: return "square-arrow-out-up-right";
    case Glyph::ShowAllWindows: return "app-window";
    case Glyph::Close: return "x";
    case Glyph::Broom: return "brush-cleaning";
    case Glyph::DragHandle: return "grip-vertical";
    case Glyph::FileText: return "file-text";
    case Glyph::Check: return "check";
    case Glyph::Star: return "star";
    }
    return "file";
}

// The SVG as vendored, read once per file from the Qt resource the icon library
// carries (see CMakeLists.txt, mervin_icons).
QByteArray svgSource(const char *name)
{
    static QHash<QByteArray, QByteArray> cache;
    const QByteArray key(name);
    auto it = cache.constFind(key);
    if (it != cache.constEnd())
        return *it;
    QFile f(QStringLiteral(":/icons/lucide/%1.svg").arg(QLatin1String(name)));
    const QByteArray svg = f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
    Q_ASSERT_X(!svg.isEmpty(), "mervin::icons", "a Lucide SVG is missing from the resource");
    cache.insert(key, svg);
    return svg;
}

// The markup for `name` in `color`. Lucide strokes with currentColor at 2 units
// and fills nothing; all three are substituted in the markup, so the vendored
// files stay untouched. Colours go in without their alpha (SVG colours are
// opaque); callers apply the stroke's alpha as painter opacity.
QByteArray tinted(const char *name, const QColor &color, double strokeWidth,
                  const QColor &fill = QColor())
{
    QByteArray svg = svgSource(name);
    svg.replace("currentColor", color.name(QColor::HexRgb).toLatin1());
    if (strokeWidth > 0 && strokeWidth != kLucideStroke)
        svg.replace("stroke-width=\"2\"",
                    "stroke-width=\"" + QByteArray::number(strokeWidth) + '"');
    if (fill.isValid())
        svg.replace("fill=\"none\"",
                    "fill=\"" + fill.name(QColor::HexRgb).toLatin1() + '"');
    return svg;
}

QPixmap rasterize(QSvgRenderer &renderer, const QColor &color, int sz)
{
    QPixmap pm(sz, sz);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setOpacity(color.alphaF());
    renderer.render(&p, QRectF(0, 0, sz, sz));
    return pm;
}

} // namespace

QIcon glyph(Glyph id, const QColor &color)
{
    QSvgRenderer renderer(tinted(lucideName(id), color, kStroke));
    QIcon icon;
    for (int sz : kSizes)
        icon.addPixmap(rasterize(renderer, color, sz));
    return icon;
}

QIcon glyphBadged(Glyph base, Glyph badge, const QColor &color)
{
    // Compose per rendered size: the base pictograph, then the badge at ~60%
    // over the bottom-right corner. The margin behind the badge is erased - not
    // filled with the menu surface colour - so the composite stays readable over
    // the hover wash and any other row state.
    const QIcon baseIcon  = glyph(base, color);
    const QIcon badgeIcon = glyph(badge, color);
    QIcon icon;
    for (const QSize &sq : baseIcon.availableSizes()) {
        const int sz = sq.width();
        QPixmap pm = baseIcon.pixmap(sz, sz); // QPainter::begin() detaches the copy
        QPainter p(&pm);
        p.setRenderHint(QPainter::SmoothPixmapTransform, true);
        const int bs = sz * 3 / 5;
        const QRect badgeBox(sz - bs, sz - bs, bs, bs);
        const int margin = qMax(2, sz / 8);
        p.setCompositionMode(QPainter::CompositionMode_Clear);
        p.fillRect(badgeBox.adjusted(-margin, -margin, 0, 0), Qt::black);
        p.setCompositionMode(QPainter::CompositionMode_SourceOver);
        p.drawPixmap(badgeBox, badgeIcon.pixmap(sz, sz));
        p.end();
        icon.addPixmap(pm);
    }
    return icon;
}

QPixmap glyphPixmap(Glyph id, const QColor &color, int sizePx, double strokeWidth,
                    const QColor &fill)
{
    const int sz = qMax(1, sizePx);
    if (strokeWidth <= 0 && !fill.isValid())
        return glyph(id, color).pixmap(sz, sz);
    QSvgRenderer renderer(tinted(lucideName(id), color, strokeWidth > 0 ? strokeWidth : kStroke,
                                 fill));
    return rasterize(renderer, color, sz);
}

namespace {

// Keeps a button's glyph in its current foreground colour (setButtonGlyph). A
// child of the button, so it goes when the button does.
class ButtonGlyph : public QObject
{
public:
    ButtonGlyph(QAbstractButton *button, Glyph glyph)
        : QObject(button)
        , button_(button)
        , glyph_(glyph)
    {
        button->installEventFilter(this);
        apply();
    }

    bool eventFilter(QObject *, QEvent *event) override
    {
        // A stylesheet sets the palette while polishing, and a theme switch
        // re-polishes: either way the ink may have changed.
        switch (event->type()) {
        case QEvent::Polish:
        case QEvent::PaletteChange:
        case QEvent::StyleChange:
            apply();
            break;
        default:
            break;
        }
        return false;
    }

private:
    void apply()
    {
        const QPalette pal = button_->palette();
        QIcon icon = glyph(glyph_, pal.color(QPalette::Active, QPalette::ButtonText));
        const QIcon off = glyph(glyph_, pal.color(QPalette::Disabled, QPalette::ButtonText));
        for (const QSize &sz : off.availableSizes())
            icon.addPixmap(off.pixmap(sz), QIcon::Disabled);
        button_->setIcon(icon);
    }

    QAbstractButton *button_;
    Glyph glyph_;
};

} // namespace

void setButtonGlyph(QAbstractButton *button, Glyph glyph, int iconPx)
{
    button->setText(QString());
    button->setIconSize(QSize(iconPx, iconPx));
    if (auto *tool = qobject_cast<QToolButton *>(button))
        tool->setToolButtonStyle(Qt::ToolButtonIconOnly);
    new ButtonGlyph(button, glyph);
}

QPixmap spinChevron(bool down, const QColor &color, int sizePx)
{
    QSvgRenderer renderer(tinted(down ? "chevron-down" : "chevron-up", color, kStroke));
    return rasterize(renderer, color, qMax(1, sizePx));
}

} // namespace mervin::icons
