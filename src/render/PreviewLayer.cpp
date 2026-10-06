#include "render/PreviewLayer.h"

#include <algorithm>
#include <cmath>

namespace mervin {

bool PreviewLayer::add(int pageNo, const QImage &image, const QRect &covered, const QRect &pageRect)
{
    if (image.isNull() || pageRect.width() <= 0 || pageRect.height() <= 0 || covered.isEmpty())
        return false;
    // The fraction we store stands for the WHOLE image, so a region reaching
    // outside the page would be drawn squashed. The viewer never asks for one
    // (it intersects every render region with the page rect), so refuse rather
    // than silently mis-scale.
    if (!pageRect.contains(covered))
        return false;

    // Check the budget against what the prepared tile will cost BEFORE preparing
    // it: re-encoding a tile we would only throw away is several ms wasted on the
    // zoom path.
    const qint64 pixels = std::min<qint64>(qint64(image.width()) * image.height(), kMaxTilePixels);
    if (!tiles_.isEmpty() && bytes_ - heldBytes(pageNo) + pixels * 4 > budget_)
        return false;

    Tile t;
    t.image = prepare(image);
    t.frac = QRectF(double(covered.x() - pageRect.x()) / pageRect.width(),
                    double(covered.y() - pageRect.y()) / pageRect.height(),
                    double(covered.width()) / pageRect.width(),
                    double(covered.height()) / pageRect.height());
    return adopt(pageNo, t);
}

bool PreviewLayer::adopt(int pageNo, const Tile &tile)
{
    if (tile.image.isNull() || tile.frac.width() <= 0.0 || tile.frac.height() <= 0.0)
        return false;

    // The newest tile always wins. It was rendered at the scale we are coming
    // from, so the magnification it is drawn at stays around the size of the zoom
    // step; keeping an older, wider one instead would pin a stand-in that grows
    // more magnified with every step and can never be replaced.
    const qint64 size = tile.image.sizeInBytes();
    const qint64 replaced = heldBytes(pageNo);
    if (!tiles_.isEmpty() && bytes_ - replaced + size > budget_)
        return false;

    tiles_.insert(pageNo, tile);
    bytes_ += size - replaced;
    return true;
}

QImage PreviewLayer::prepare(const QImage &image)
{
    QImage out = image;
    const qint64 pixels = qint64(out.width()) * out.height();
    if (pixels > kMaxTilePixels) {
        const double k = std::sqrt(double(kMaxTilePixels) / double(pixels));
        out = out.scaled(std::max(1, static_cast<int>(out.width() * k)),
                         std::max(1, static_cast<int>(out.height() * k)),
                         Qt::IgnoreAspectRatio, // the factor already keeps the aspect
                         Qt::FastTransformation);
    }
    if (out.format() != QImage::Format_RGB32)
        out = out.convertToFormat(QImage::Format_RGB32);
    return out;
}

qint64 PreviewLayer::heldBytes(int pageNo) const
{
    const auto it = tiles_.constFind(pageNo);
    return it == tiles_.constEnd() ? 0 : it.value().image.sizeInBytes();
}

const PreviewLayer::Tile *PreviewLayer::tile(int pageNo) const
{
    const auto it = tiles_.constFind(pageNo);
    return it == tiles_.constEnd() ? nullptr : &it.value();
}

void PreviewLayer::erase(int pageNo)
{
    const auto it = tiles_.constFind(pageNo);
    if (it == tiles_.constEnd())
        return;
    bytes_ -= it.value().image.sizeInBytes();
    tiles_.erase(it);
}

void PreviewLayer::retain(const std::vector<int> &pages)
{
    for (auto it = tiles_.begin(); it != tiles_.end();) {
        if (std::find(pages.begin(), pages.end(), it.key()) != pages.end()) {
            ++it;
            continue;
        }
        bytes_ -= it.value().image.sizeInBytes();
        it = tiles_.erase(it);
    }
}

void PreviewLayer::clear()
{
    tiles_.clear();
    bytes_ = 0;
}

QRectF PreviewLayer::targetRect(const Tile &t, const QRect &pageRect)
{
    if (pageRect.width() <= 0 || pageRect.height() <= 0)
        return {};
    return QRectF(pageRect.x() + t.frac.x() * pageRect.width(),
                  pageRect.y() + t.frac.y() * pageRect.height(),
                  t.frac.width() * pageRect.width(), t.frac.height() * pageRect.height());
}

bool PreviewLayer::clipToViewport(const QRectF &target, const QImage &image, const QRectF &clip,
                                  QRectF *targetOut, QRectF *sourceOut)
{
    const QSize imageSize = image.size(); // raw pixels; see the header on dpr
    if (target.width() <= 0.0 || target.height() <= 0.0 || imageSize.isEmpty())
        return false;
    const QRectF visible = target.intersected(clip);
    if (visible.width() <= 0.0 || visible.height() <= 0.0)
        return false;

    // Image pixels per target pixel, then the slice of the image that maps onto
    // the visible part of the target.
    const double kx = imageSize.width() / target.width();
    const double ky = imageSize.height() / target.height();
    QRectF source((visible.left() - target.left()) * kx, (visible.top() - target.top()) * ky,
                  visible.width() * kx, visible.height() * ky);
    // Rounding can push the slice a hair past the last row/column.
    source = source.intersected(QRectF(QPointF(0, 0), QSizeF(imageSize)));
    if (source.width() <= 0.0 || source.height() <= 0.0)
        return false;

    *targetOut = visible;
    *sourceOut = source;
    return true;
}

} // namespace mervin
