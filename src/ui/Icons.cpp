#include "ui/Icons.h"

#include <QAbstractButton>
#include <QByteArray>
#include <QColor>
#include <QEvent>
#include <QFile>
#include <QGuiApplication>
#include <QHash>
#include <QPainter>
#include <QPalette>
#include <QPixmap>
#include <QRect>
#include <QRectF>
#include <QSize>
#include <QSvgRenderer>
#include <QToolButton>

#include <algorithm>

// Q_INIT_RESOURCE must run outside a namespace so the static library's resource
// initializer resolves to the generated global symbol.
static void initializeApplicationIconResources()
{
    Q_INIT_RESOURCE(app_icon);
}

namespace mervin::icons {

QIcon applicationIcon()
{
    static const QIcon icon = [] {
        initializeApplicationIconResources();
        QIcon result;
        for (int size : {16, 20, 24, 30, 32, 36, 40, 48, 60, 64, 72, 80, 96, 128, 256}) {
            const QString path = size == 256
                ? QStringLiteral(":/icons/mervin-icon.png")
                : QStringLiteral(":/icons/mervin-icon/%1.png").arg(size);
            const QPixmap frame(path);
            Q_ASSERT_X(!frame.isNull(), "mervin::icons::applicationIcon",
                       "an application icon frame is missing from the resource");
            result.addPixmap(frame);
        }
        return result;
    }();
    return icon;
}

namespace {

// Lucide draws every icon with a 2-unit stroke on a 24-unit grid; the app draws
// them lighter, close to the weight of its text. One number, so the whole set's
// weight can be tuned in a single place.
constexpr double kLucideStroke = 2.0; // what the vendored files say
constexpr double kStroke = 1.5;       // what the app draws

// The thinnest line a render may get, in device pixels. At 1 px or less Qt's
// raster engine switches to its hairline stroker, which drops round caps, so the
// dots in list bullets, the info "i" and the keyboard keys vanish. At 1.5 units a
// 16 px render lands exactly on 1 px, so small renders are thickened to stay above.
constexpr double kMinStrokePx = 1.07;

// The stroke, in Lucide units, for a render `sizePx` device pixels square.
double strokeFor(double stroke, int sizePx)
{
    return std::max(stroke, kMinStrokePx * 24.0 / std::max(1, sizePx));
}

// The sizes QIcon is populated with. Each is rendered natively from the SVG, not
// scaled from one pixmap, so a stroke never lands between pixels by accident.
constexpr int kSizes[] = {16, 20, 24, 32, 48};

// The Lucide file each glyph is drawn from, under resources/icons/lucide.
// Fit glyphs add a square around this arrow when rasterized.
const char *lucideName(Glyph id)
{
    switch (id) {
    case Glyph::Open: return "folder-open";
    case Glyph::PrevPage: return "chevron-left";
    case Glyph::NextPage: return "chevron-right";
    case Glyph::ChevronDown: return "chevron-down";
    case Glyph::ChevronUp: return "chevron-up";
    case Glyph::Search: return "search";
    case Glyph::ZoomOut: return "minus";
    case Glyph::ZoomIn: return "plus";
    case Glyph::FitPage: return "move-vertical";
    case Glyph::FitWidth: return "move-horizontal";
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
    case Glyph::Appearance: return "palette";
    case Glyph::Viewing: return "eye";
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

QPixmap rasterize(Glyph id, const QColor &color, int sz, double strokeWidth,
                  const QColor &fill = QColor())
{
    QPixmap pm(sz, sz);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setOpacity(color.alphaF());
    QRectF bounds(0, 0, sz, sz);
    if (id == Glyph::FitPage || id == Glyph::FitWidth) {
        QSvgRenderer square(tinted("square", color, strokeWidth, fill));
        square.render(&p, bounds);
        // Inset the arrow geometry while keeping its stroke as heavy as the
        // square. Render both SVGs natively at each requested device size.
        constexpr double arrowScale = 0.65;
        const double inset = sz * (1.0 - arrowScale) / 2.0;
        bounds.adjust(inset, inset, -inset, -inset);
        strokeWidth /= arrowScale;
    }
    QSvgRenderer renderer(tinted(lucideName(id), color, strokeWidth, fill));
    renderer.render(&p, bounds);
    return pm;
}

} // namespace

QIcon glyph(Glyph id, const QColor &color)
{
    QIcon icon;
    for (int sz : kSizes)
        icon.addPixmap(rasterize(id, color, sz, strokeFor(kStroke, sz)));
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
                    const QColor &fill, qreal devicePixelRatio)
{
    const int sz = qMax(1, sizePx);
    if (strokeWidth <= 0 && !fill.isValid()) {
        // Drawn natively at the device size rather than scaled from the nearest
        // QIcon render: shrinking the 16 px render to a 12 px chevron thinned its
        // line below a pixel. Without a ratio this uses the application's, as
        // QIcon::pixmap(size) does - the highest of all screens.
        const qreal dpr = devicePixelRatio > 0 ? devicePixelRatio
                          : qGuiApp       ? qGuiApp->devicePixelRatio()
                                          : 1.0;
        const int device = qMax(1, qRound(sz * dpr));
        QPixmap pm = rasterize(id, color, device, strokeFor(kStroke, device));
        pm.setDevicePixelRatio(dpr);
        return pm;
    }
    return rasterize(id, color, sz,
                     strokeWidth > 0 ? strokeWidth : strokeFor(kStroke, sz), fill);
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
    const int sz = qMax(1, sizePx);
    return rasterize(down ? Glyph::ChevronDown : Glyph::ChevronUp, color, sz,
                     strokeFor(kStroke, sz));
}

} // namespace mervin::icons
