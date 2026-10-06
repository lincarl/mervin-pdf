#include "ui/UiThemePicker.h"

#include "ui/ThemeCard.h"
#include "ui/ThemeTokens.h"

#include "ui/Theme.h"

#include <QAbstractButton>
#include <QButtonGroup>
#include <QHBoxLayout>
#include <QImage>
#include <QPainter>
#include <QPainterPath>
#include <QPalette>

#include <algorithm>
#include <array>

namespace mervin {
namespace {

enum class PreviewScheme { Dark, Light, System };

const std::array<QString, 3> kSchemeValues = {
    QStringLiteral("dark"), QStringLiteral("light"), QStringLiteral("system")};

// The miniature window's size in its own units; the preview scales it to fit.
constexpr qreal kWindowWidth = 112;
constexpr qreal kWindowHeight = 78;

// One scheme's chrome colours, whichever scheme the application uses now.
// Without an accent, each scheme takes the one the "system" setting gives it (see
// Theme::systemAccent). A scheme the application has not been in yet is a guess
// from the current one's OS accent.
theme::Chrome schemeColors(bool dark, const QColor &accent)
{
    const QColor resolved = accent.isValid() ? accent : Theme::systemAccent(dark);
    if (dark)
        return theme::chrome(theme::darkPalette(resolved), resolved);
    QPalette light;
    light.setColor(QPalette::Window, Qt::white);
    light.setColor(QPalette::WindowText, Qt::black);
    light.setColor(QPalette::Text, Qt::black);
    return theme::chrome(light, resolved);
}

// A Mervin window in one scheme, in window units: toolbar, tab row, thumbnail
// sidebar, a page on the canvas and the status bar. The open tab and the current
// thumbnail carry the accent.
void paintWindow(QPainter &painter, const theme::Chrome &c)
{
    const QColor paper = theme::doc().paperNormal;
    const QColor pageInk(0x45, 0x52, 0x60);
    QColor lineInk = pageInk;
    lineInk.setAlphaF(0.6);
    QPainterPath outline;
    outline.addRoundedRect(QRectF(0.4, 0.4, kWindowWidth - 0.8, kWindowHeight - 0.8), 3, 3);
    painter.setClipPath(outline);
    painter.setPen(Qt::NoPen);

    painter.fillRect(QRectF(0, 0, kWindowWidth, 13), c.window);
    painter.setBrush(c.inkSoft);
    for (qreal x : {5.0, 12.0, 19.0, 30.0, 37.0, 44.0, 55.0, 62.0})
        painter.drawRoundedRect(QRectF(x, 4.5, 4, 4), 1, 1);

    painter.fillRect(QRectF(0, 13, kWindowWidth, 10), c.bar);
    painter.fillRect(QRectF(21, 14.5, 27, 8.5), c.window);
    painter.fillRect(QRectF(25, 18, 15, 1.6), c.accent);
    for (qreal x : {53.0, 72.0})
        painter.fillRect(QRectF(x, 18, 13, 1.6), c.inkSoft);

    painter.fillRect(QRectF(0, 23, 30, 50), c.bar);
    painter.fillRect(QRectF(30, 23, kWindowWidth - 30, 50), c.canvas);
    painter.fillRect(QRectF(0, 22.6, kWindowWidth, 0.8), c.separator);
    painter.fillRect(QRectF(29.6, 23, 0.8, 50), c.separator);
    for (qreal y : {28.0, 48.0, 68.0}) {
        painter.setPen(y == 28.0 ? QPen(c.accent, 1.2) : QPen(c.border, 0.8));
        painter.setBrush(paper);
        painter.drawRect(QRectF(9, y, 12, 16));
    }
    painter.setPen(Qt::NoPen);

    painter.fillRect(QRectF(49, 27, 44, 52), paper);
    painter.fillRect(QRectF(54, 32, 18, 2.4), pageInk);
    for (qreal y : {37.5, 41.0, 44.5})
        painter.fillRect(QRectF(54, y, y == 41.0 ? 28 : 34, 1.3), lineInk);
    painter.save();
    painter.setClipRect(QRectF(54, 49, 34, 13), Qt::IntersectClip);
    painter.fillRect(QRectF(54, 49, 34, 13), QColor(0x8c, 0xc5, 0xdf));
    painter.setBrush(QColor(0x67, 0x97, 0x8f));
    painter.drawPolygon(QPolygonF{QPointF(64, 62), QPointF(77, 52), QPointF(92, 62)});
    painter.setBrush(QColor(0x39, 0x71, 0x6a));
    painter.drawPolygon(QPolygonF{QPointF(50, 62), QPointF(63, 54), QPointF(76, 62)});
    painter.restore();
    painter.fillRect(QRectF(54, 66, 34, 1.3), lineInk);

    painter.fillRect(QRectF(0, 73, kWindowWidth, 5), c.status);
    painter.fillRect(QRectF(4, 75, 22, 1.2), c.inkFaint);

    painter.setClipping(false);
    painter.setBrush(Qt::NoBrush);
    painter.setPen(QPen(c.borderStrong, 0.8));
    painter.drawPath(outline);
}

// The window in one scheme as an image with `pixelRatio` device pixels per unit.
QImage windowImage(bool dark, const QColor &accent, qreal pixelRatio)
{
    QImage image((QSizeF(kWindowWidth, kWindowHeight) * pixelRatio).toSize(),
                 QImage::Format_ARGB32_Premultiplied);
    image.setDevicePixelRatio(pixelRatio);
    image.fill(Qt::transparent);
    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing);
    paintWindow(painter, schemeColors(dark, accent));
    return image;
}

void paintPreview(QPainter &painter, const QRectF &well, PreviewScheme preview,
                  const QColor &accent)
{
    const qreal scale = std::min({1.15, (well.width() - 16) / kWindowWidth,
                                 (well.height() - 18) / kWindowHeight});
    const QSizeF size = QSizeF(kWindowWidth, kWindowHeight) * scale;
    const QRectF frame(well.center() - QPointF(size.width(), size.height()) / 2, size);
    // Each scheme is painted whole into one image, so the two halves of Follow
    // system meet without a seam across the page they share.
    const qreal pixelRatio = painter.device()->devicePixelRatio() * scale;
    QImage window = windowImage(preview != PreviewScheme::Light, accent, pixelRatio);
    if (preview == PreviewScheme::System) {
        // Dark on the left, light on the right, split on a slant. The light half
        // replaces the dark one inside the image, so its edges match the Light card.
        QPainter split(&window);
        split.setRenderHint(QPainter::Antialiasing);
        QPainterPath lightSide;
        lightSide.addPolygon(QPolygonF{QPointF(kWindowWidth * 0.62, -1),
                                       QPointF(kWindowWidth + 1, -1),
                                       QPointF(kWindowWidth + 1, kWindowHeight + 1),
                                       QPointF(kWindowWidth * 0.38, kWindowHeight + 1)});
        split.setClipPath(lightSide);
        split.setCompositionMode(QPainter::CompositionMode_Source);
        split.drawImage(QPointF(0, 0), windowImage(false, accent, pixelRatio));
    }
    painter.drawImage(frame, window);
}

} // namespace

UiThemePicker::UiThemePicker(QWidget *parent) : QWidget(parent)
{
    setObjectName(QStringLiteral("uiThemePicker"));
    setAccessibleName(tr("UI theme"));

    auto *row = new QHBoxLayout(this);
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(10);
    choices_ = new QButtonGroup(this);
    choices_->setExclusive(true);

    const std::array<QString, 3> labels = {tr("Dark"), tr("Light"), tr("Follow system")};
    const std::array<QString, 3> descriptions = {
        tr("Dark window, toolbars and dialogs"),
        tr("Light window, toolbars and dialogs"),
        tr("Switches between light and dark with the operating system")};
    const std::array<QString, 3> names = {QStringLiteral("uiThemeDark"),
                                        QStringLiteral("uiThemeLight"),
                                        QStringLiteral("uiThemeSystem")};
    for (int i = 0; i < 3; ++i) {
        const auto preview = static_cast<PreviewScheme>(i);
        auto *choice = new ThemeCard(
            labels[i],
            [this, preview](QPainter &painter, const QRectF &well) {
                paintPreview(painter, well, preview, accent_);
            },
            this);
        choice->setObjectName(names[i]);
        choice->setAccessibleDescription(descriptions[i]);
        choices_->addButton(choice, i);
        row->addWidget(choice, 1);
    }
    setScheme(kSchemeValues[0]);
}

void UiThemePicker::setScheme(const QString &scheme)
{
    const auto it = std::find(kSchemeValues.begin(), kSchemeValues.end(), scheme);
    // An unknown value follows the system, as WindowManager treats it.
    const int id = it == kSchemeValues.end() ? 2 : int(it - kSchemeValues.begin());
    choices_->button(id)->setChecked(true);
}

QString UiThemePicker::scheme() const
{
    const int id = choices_->checkedId();
    return id >= 0 ? kSchemeValues[id] : kSchemeValues[0];
}

void UiThemePicker::setAccent(const QColor &accent)
{
    if (accent == accent_)
        return;
    accent_ = accent;
    for (QAbstractButton *choice : choices_->buttons())
        choice->update();
}

} // namespace mervin
