#include "ui/Icons.h"

#include <QAbstractButton>
#include <QApplication>
#include <QByteArray>
#include <QColor>
#include <QEvent>
#include <QFile>
#include <QGuiApplication>
#include <QHash>
#include <QIconEngine>
#include <QPainter>
#include <QPalette>
#include <QPixmap>
#include <QPixmapCache>
#include <QRect>
#include <QRectF>
#include <QSize>
#include <QStyle>
#include <QStyleOption>
#include <QSvgRenderer>
#include <QToolButton>

#include <algorithm>
#include <optional>

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

// kStroke is one logical pixel at the menus' 16 px. Icons from there up to the
// toolbar's 20 px keep that line instead of growing with the icon (1.2 units at
// 20 px), so the toolbar matches the menus at every display scale. Smaller and
// larger icons draw kStroke.
constexpr int kMenuIconPx = 16;
constexpr int kToolBarIconPx = 20;

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

// The stroke, in Lucide units, for an icon `logicalPx` square drawn on
// `devicePx` device pixels. The logical size picks the line and the device size
// the floor.
double strokeAt(int logicalPx, int devicePx)
{
    const bool onePixel = logicalPx >= kMenuIconPx && logicalPx <= kToolBarIconPx;
    return strokeFor(onePixel ? 24.0 / logicalPx : kStroke, devicePx);
}

// The sizes glyph() icons report through QIcon::availableSizes(). They draw at
// any size; these are the ones the tests and the contact sheet check.
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
    case Glyph::Restore: return "undo-2";
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
    if (id == Glyph::SplitPages || id == Glyph::MergePages) {
        // Lucide's split and merge point up. Turned a quarter clockwise about the
        // centre, they point right, and the grid lines stay on the pixel grid.
        p.translate(sz / 2.0, sz / 2.0);
        p.rotate(90);
        p.translate(-sz / 2.0, -sz / 2.0);
    }
    QSvgRenderer renderer(tinted(lucideName(id), color, strokeWidth, fill));
    renderer.render(&p, bounds);
    return pm;
}

// A mode's pixmap derived from the normal one by the application style, as QIcon's
// pixmap engine does for a mode it holds no pixmap for, so a disabled glyph dims
// like any other icon. A QGuiApplication has no style and keeps the normal look.
QPixmap styled(QIcon::Mode mode, const QPixmap &normal)
{
    if (!qobject_cast<QApplication *>(QCoreApplication::instance()))
        return normal;
    QStyleOption option;
    option.palette = QGuiApplication::palette();
    const QPixmap generated = QApplication::style()->generatedIconPixmap(mode, normal, &option);
    return generated.isNull() ? normal : generated;
}

// The engine behind glyph(), glyphBadged() and setButtonGlyph(). It draws each
// request from the SVG at the exact device size asked for, the logical size times
// the device pixel ratio, so 125%, 150% and 175% get native renders instead of a
// larger render shrunk to fit, and the line follows the logical size (strokeAt).
// Renders are kept in QPixmapCache. An engine is bound to one ink; icons are
// rebuilt when the theme changes. It ignores QIcon::addPixmap().
class GlyphEngine : public QIconEngine
{
public:
    // A valid `disabledInk` draws the Disabled mode; otherwise the style derives it.
    GlyphEngine(Glyph id, const QColor &ink, const QColor &disabledInk = QColor(),
                std::optional<Glyph> badge = std::nullopt)
        : id_(id)
        , badge_(badge)
        , ink_(ink)
        , disabledInk_(disabledInk)
    {
    }

    QIconEngine *clone() const override { return new GlyphEngine(*this); }
    bool isNull() override { return false; }

    // Glyphs are square, so the largest square that fits.
    QSize actualSize(const QSize &size, QIcon::Mode, QIcon::State) override
    {
        const int side = std::min(size.width(), size.height());
        return side > 0 ? QSize(side, side) : QSize();
    }

    QList<QSize> availableSizes(QIcon::Mode, QIcon::State) override
    {
        QList<QSize> sizes;
        for (int sz : kSizes)
            sizes.append(QSize(sz, sz));
        return sizes;
    }

    void paint(QPainter *painter, const QRect &rect, QIcon::Mode mode, QIcon::State state) override
    {
        const QPaintDevice *device = painter->device();
        const qreal dpr = device ? device->devicePixelRatio()
                          : qGuiApp ? qGuiApp->devicePixelRatio()
                                    : 1.0;
        painter->drawPixmap(rect, scaledPixmap(rect.size(), mode, state, dpr));
    }

    QPixmap pixmap(const QSize &size, QIcon::Mode mode, QIcon::State state) override
    {
        return scaledPixmap(size, mode, state, 1.0);
    }

    // QIcon::pixmap() passes `size` in logical pixels and `scale` as the ratio.
    QPixmap scaledPixmap(const QSize &size, QIcon::Mode mode, QIcon::State,
                         qreal scale) override
    {
        // Rounded as QIcon::pixmap() rounds when it sets the result's ratio.
        const QSize scaled = size * scale;
        const int logical = std::min(size.width(), size.height());
        const int device = std::min(scaled.width(), scaled.height());
        if (logical <= 0 || device <= 0)
            return QPixmap();
        const bool disabledInk = mode == QIcon::Disabled && disabledInk_.isValid();
        const bool derived = mode != QIcon::Normal && !disabledInk;
        const QColor ink = disabledInk ? disabledInk_ : ink_;
        // Everything the pixels depend on. A derived mode also follows the palette.
        const QString key = QString::asprintf(
            "mervin-glyph:%d:%d:%08x:%d:%d:%g:%d:%llx", int(id_), badge_ ? int(*badge_) : -1,
            ink.rgba(), logical, device, scale, derived ? int(mode) : -1,
            derived ? static_cast<unsigned long long>(QGuiApplication::palette().cacheKey())
                    : 0ULL);
        QPixmap pm;
        if (QPixmapCache::find(key, &pm))
            return pm;
        pm = render(logical, device, scale, ink);
        if (derived)
            pm = styled(mode, pm);
        pm.setDevicePixelRatio(scale);
        QPixmapCache::insert(key, pm);
        return pm;
    }

private:
    // The glyph in `ink`, `device` pixels square, with its badge if it has one.
    QPixmap render(int logical, int device, qreal scale, const QColor &ink) const
    {
        const double stroke = strokeAt(logical, device);
        QPixmap pm = rasterize(id_, ink, device, stroke);
        if (!badge_)
            return pm;
        // The badge at about 60% over the bottom-right corner, laid out in logical
        // pixels. The margin behind it is erased, not filled with the menu surface
        // colour, so the composite stays readable over the hover wash and any other
        // row state.
        pm.setDevicePixelRatio(scale);
        QPixmap badge = rasterize(*badge_, ink, device, stroke);
        badge.setDevicePixelRatio(scale);
        QPainter p(&pm);
        p.setRenderHint(QPainter::SmoothPixmapTransform, true);
        const int bs = logical * 3 / 5;
        const QRect badgeBox(logical - bs, logical - bs, bs, bs);
        const int margin = std::max(2, logical / 8);
        p.setCompositionMode(QPainter::CompositionMode_Clear);
        p.fillRect(badgeBox.adjusted(-margin, -margin, 0, 0), Qt::black);
        p.setCompositionMode(QPainter::CompositionMode_SourceOver);
        p.drawPixmap(badgeBox, badge);
        return pm;
    }

    Glyph id_;
    std::optional<Glyph> badge_;
    QColor ink_;
    QColor disabledInk_;
};

} // namespace

QIcon glyph(Glyph id, const QColor &color)
{
    return QIcon(new GlyphEngine(id, color));
}

QIcon glyphBadged(Glyph base, Glyph badge, const QColor &color)
{
    return QIcon(new GlyphEngine(base, color, QColor(), badge));
}

QPixmap glyphPixmap(Glyph id, const QColor &color, int sizePx, double strokeWidth,
                    const QColor &fill, qreal devicePixelRatio)
{
    const int sz = qMax(1, sizePx);
    if (strokeWidth <= 0 && !fill.isValid()) {
        // Drawn natively at the device size with the line glyph() gives a sizePx
        // icon, the same pixels glyph() draws. Shrinking a larger render thinned a
        // 12 px chevron's line below a pixel. Without a ratio this uses the
        // application's, the highest of all screens, as QIcon::pixmap(size) does.
        const qreal dpr = devicePixelRatio > 0 ? devicePixelRatio
                          : qGuiApp       ? qGuiApp->devicePixelRatio()
                                          : 1.0;
        const int device = qMax(1, qRound(sz * dpr));
        QPixmap pm = rasterize(id, color, device, strokeAt(sz, device));
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
        button_->setIcon(QIcon(new GlyphEngine(glyph_,
                                               pal.color(QPalette::Active, QPalette::ButtonText),
                                               pal.color(QPalette::Disabled, QPalette::ButtonText))));
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
