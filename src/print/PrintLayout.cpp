#include "PrintLayout.h"

#include <QPainter>

#include <algorithm>
#include <cmath>

namespace mervin::printing {

namespace {

// MuPDF clamps device coordinates to 2^24 (fz_round_rect), so a page raster cannot
// address more pixels than this on a side.
constexpr double kMaxRasterSide = 16777216.0;
// fz_round_rect's tolerance, so rounding noise does not add a row or column.
constexpr double kRoundingSlack = 0.001;

} // namespace

QRectF destinationRect(const QSizeF &pagePoints, const QRectF &printableRect,
                      const Settings &settings)
{
    if (!std::isfinite(pagePoints.width()) || !std::isfinite(pagePoints.height())
        || !std::isfinite(printableRect.x()) || !std::isfinite(printableRect.y())
        || !std::isfinite(printableRect.width()) || !std::isfinite(printableRect.height())
        || pagePoints.isEmpty() || printableRect.isEmpty()) {
        return {};
    }

    qreal scale = 1.0;
    switch (settings.scaleMode) {
    case ScaleMode::FitToPage:
        scale = std::min(printableRect.width() / pagePoints.width(),
                         printableRect.height() / pagePoints.height());
        break;
    case ScaleMode::ActualSize:
        break;
    case ScaleMode::Custom:
        scale = settings.scalePercent / 100.0;
        break;
    }

    const QSizeF size = pagePoints * scale;
    if (size.isEmpty() || !std::isfinite(size.width()) || !std::isfinite(size.height()))
        return {};

    QPointF origin = printableRect.topLeft();
    if (settings.alignment == Alignment::Centered
        || settings.alignment == Alignment::CenteredHorizontally) {
        origin.rx() += (printableRect.width() - size.width()) / 2.0;
    }
    if (settings.alignment == Alignment::Centered
        || settings.alignment == Alignment::CenteredVertically) {
        origin.ry() += (printableRect.height() - size.height()) / 2.0;
    }
    return {origin, size};
}

RasterPlan rasterPlan(const QSizeF &pagePoints, const QRectF &printableRect,
                      const Settings &settings, double dpi)
{
    const QRectF destination = destinationRect(pagePoints, printableRect, settings);
    if (destination.isEmpty() || !std::isfinite(dpi) || dpi <= 0)
        return {};
    // Raster pixels per sheet point. The page's scale on the sheet then sets how
    // many raster pixels each page point needs.
    const double density = dpi / 72.0;
    const QSizeF full = destination.size() * density;
    if (!(full.width() <= kMaxRasterSide && full.height() <= kMaxRasterSide))
        return {};
    const QRectF visible = (destination & printableRect).translated(-destination.topLeft());
    if (visible.isEmpty())
        return {};
    const QPoint topLeft(int(std::floor(visible.left() * density + kRoundingSlack)),
                         int(std::floor(visible.top() * density + kRoundingSlack)));
    const QPoint end(int(std::ceil(visible.right() * density - kRoundingSlack)),
                     int(std::ceil(visible.bottom() * density - kRoundingSlack)));
    const QRect bounds(0, 0, int(std::ceil(full.width() - kRoundingSlack)),
                       int(std::ceil(full.height() - kRoundingSlack)));
    RasterPlan plan;
    plan.source = QRect(topLeft, end - QPoint(1, 1)) & bounds;
    if (plan.source.isEmpty())
        return {};
    plan.scale = density * destination.width() / pagePoints.width();
    plan.destination = destination;
    return plan;
}

QRectF rasterTarget(const QRectF &destination, const QSizeF &pagePoints, double scale,
                    const QRect &pixels)
{
    if (pagePoints.isEmpty() || !(scale > 0))
        return {};
    // Raster pixel p covers page point p / scale, which destination scales again.
    const double unit = destination.width() / pagePoints.width() / scale;
    return {destination.topLeft() + QPointF(pixels.topLeft()) * unit,
            QSizeF(pixels.size()) * unit};
}

QList<QRect> rasterBands(const QRect &source, qint64 maxPixels)
{
    if (source.isEmpty())
        return {};
    const qint64 height = source.height();
    const qint64 rows = std::clamp<qint64>(maxPixels / source.width(), 1, height);
    const qint64 count = (height + rows - 1) / rows;
    QList<QRect> bands;
    bands.reserve(count);
    int top = source.top();
    for (qint64 band = 1; band <= count; ++band) {
        const int bottom = source.top() + int(height * band / count);
        bands.append(QRect(source.left(), top, source.width(), bottom - top));
        top = bottom;
    }
    return bands;
}

QImage convertColour(const QImage &source, ColourMode mode)
{
    if (source.isNull() || mode == ColourMode::Colour)
        return source;

    // Work at the source raster size. The painter scales this raster directly,
    // avoiding a second image sized to the printer's output resolution. Renders
    // arrive as RGB888, which is read in place. Other formats are read as ARGB32.
    const bool rgb = source.format() == QImage::Format_RGB888;
    const QImage pixels = rgb || source.format() == QImage::Format_RGB32
            || source.format() == QImage::Format_ARGB32
        ? source : source.convertToFormat(QImage::Format_ARGB32);
    if (pixels.isNull())
        return {};

    QImage result(source.size(), QImage::Format_Grayscale8);
    if (result.isNull())
        return {};
    result.setDevicePixelRatio(source.devicePixelRatio());
    result.setDotsPerMeterX(source.dotsPerMeterX());
    result.setDotsPerMeterY(source.dotsPerMeterY());

    const auto gray = [mode](int red, int green, int blue) {
        const int value = qGray(red, green, blue);
        return uchar(mode == ColourMode::BlackAndWhite ? (value < 128 ? 0 : 255) : value);
    };
    for (int y = 0; y < pixels.height(); ++y) {
        auto *output = result.scanLine(y);
        if (rgb) {
            const uchar *input = pixels.constScanLine(y);
            for (int x = 0; x < pixels.width(); ++x, input += 3)
                output[x] = gray(input[0], input[1], input[2]);
            continue;
        }
        const auto *input = reinterpret_cast<const QRgb *>(pixels.constScanLine(y));
        for (int x = 0; x < pixels.width(); ++x) {
            const QRgb pixel = input[x];
            const int alpha = qAlpha(pixel);
            const int white = 255 * (255 - alpha);
            output[x] = gray((qRed(pixel) * alpha + white + 127) / 255,
                             (qGreen(pixel) * alpha + white + 127) / 255,
                             (qBlue(pixel) * alpha + white + 127) / 255);
        }
    }
    return result;
}

QImage reduceToDevice(const QImage &image, const QSize &deviceSize)
{
    if (image.isNull() || deviceSize.isEmpty() || deviceSize.width() >= image.width()
        || deviceSize.height() >= image.height())
        return image;
    return image.scaled(deviceSize, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
}

void drawImage(QPainter &painter, const QImage &prepared, const QRectF &target,
               const QRectF &printableRect, ColourMode mode, bool smoothPreview)
{
    if (prepared.isNull() || target.isEmpty())
        return;
    painter.save();
    painter.setClipRect(printableRect, Qt::IntersectClip);
    painter.setRenderHint(QPainter::SmoothPixmapTransform,
                          smoothPreview || mode != ColourMode::BlackAndWhite);
    painter.drawImage(target, prepared, QRectF(prepared.rect()));
    painter.restore();
}

} // namespace mervin::printing
