#include "PrintLayout.h"

#include <QPainter>

#include <algorithm>
#include <cmath>

namespace mervin::printing {

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

QImage convertColour(const QImage &source, ColourMode mode)
{
    if (source.isNull() || mode == ColourMode::Colour)
        return source;

    // Work at the source raster size. The painter scales this raster directly,
    // avoiding a second image sized to the printer's output resolution.
    const QImage pixels = source.format() == QImage::Format_RGB32
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

    for (int y = 0; y < pixels.height(); ++y) {
        const auto *input = reinterpret_cast<const QRgb *>(pixels.constScanLine(y));
        auto *output = result.scanLine(y);
        for (int x = 0; x < pixels.width(); ++x) {
            const QRgb pixel = input[x];
            const int alpha = qAlpha(pixel);
            const int white = 255 * (255 - alpha);
            const int red = (qRed(pixel) * alpha + white + 127) / 255;
            const int green = (qGreen(pixel) * alpha + white + 127) / 255;
            const int blue = (qBlue(pixel) * alpha + white + 127) / 255;
            const int gray = qGray(red, green, blue);
            output[x] = mode == ColourMode::BlackAndWhite ? (gray < 128 ? 0 : 255) : gray;
        }
    }
    return result;
}

void paintPage(QPainter &painter, const QImage &image, const QSizeF &pagePoints,
               const QRectF &printableRect, const Settings &settings, bool smoothPreview)
{
    const QRectF destination = destinationRect(pagePoints, printableRect, settings);
    if (image.isNull() || destination.isEmpty())
        return;

    QImage converted = convertColour(image, settings.colourMode);
    if (converted.isNull())
        return;

    if (smoothPreview) {
        // Area sampling retains thin lines when a high-resolution monochrome
        // raster is reduced to the fitted sheet. QPainter's bilinear sampling
        // alone can skip entire strokes when reducing by several times.
        const QSize deviceSize = painter.deviceTransform().mapRect(destination).size().toSize();
        if (!deviceSize.isEmpty() && deviceSize.width() < converted.width()
            && deviceSize.height() < converted.height()) {
            converted = converted.scaled(deviceSize, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
        }
    }

    painter.save();
    painter.setClipRect(printableRect, Qt::IntersectClip);
    painter.setRenderHint(QPainter::SmoothPixmapTransform,
                          smoothPreview || settings.colourMode != ColourMode::BlackAndWhite);
    painter.drawImage(destination, converted, QRectF(converted.rect()));
    painter.restore();
}

} // namespace mervin::printing
