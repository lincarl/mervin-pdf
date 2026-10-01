#pragma once

#include <QHash>
#include <QImage>
#include <QRect>
#include <QRectF>

#include <vector>

namespace mervin {

// Freeze rendered tiles across scale, device-pixel-ratio and page-layout changes to avoid blank
// frames. Coverage is stored as a fraction of the original page rectangle, so tiles from
// different zoom generations remain correctly positioned. Clear on rotation, page-theme changes
// or document replacement.
class PreviewLayer
{
public:
    struct Tile
    {
        QImage image;
        QRectF frac; // covered region as a fraction (0..1) of the page's rect
    };

    // Store previews as RGB32 for fast transformed blits (measured 3-4x faster than RGB888),
    // capped at kMaxTilePixels to bound conversion cost and memory even at deep zoom.
    static constexpr qint64 kMaxTilePixels = 8ll * 1024 * 1024;

    // Room for a couple of full-size tiles (kMaxTilePixels at 4 bytes == 32 MB)
    // or a screenful of smaller ones. Callers add the page the user is looking at
    // first, so it is the one that always fits.
    explicit PreviewLayer(qint64 budgetBytes = 64ll * 1024 * 1024)
        : budget_(budgetBytes) {}

    // Replace pageNo's preview with image covering covered within pageRect. Reject
    // null/degenerate/out-of-page input or an exhausted byte budget; an empty layer always
    // accepts its first tile. Returns whether stored.
    bool add(int pageNo, const QImage &image, const QRect &covered, const QRect &pageRect);
    // Take over a tile that is already prepared and expressed as a fraction,
    // carrying one zoom generation's stand-in into the next.
    bool adopt(int pageNo, const Tile &tile);

    const Tile *tile(int pageNo) const;
    void erase(int pageNo);
    // Drop every tile whose page is not in `pages` (a small list - the pages at
    // or near the viewport), so tiles that can no longer be painted stop holding
    // memory.
    void retain(const std::vector<int> &pages);
    void clear();

    bool isEmpty() const { return tiles_.isEmpty(); }
    int tileCount() const { return tiles_.size(); }
    qint64 bytes() const { return bytes_; }

    // Where `t` belongs now: its covered fraction mapped into `pageRect`, the
    // page's rect at the current scale. A null rect when `pageRect` is empty.
    static QRectF targetRect(const Tile &t, const QRect &pageRect);

    // Clip target to the viewport and return its corresponding source slice; false if
    // invisible. This bounds deep-zoom blit cost and avoids raster coordinate overflow.
    // sourceOut uses raw image pixels: the explicit-source QPainter overload does not apply
    // devicePixelRatio.
    static bool clipToViewport(const QRectF &target, const QImage &image, const QRectF &clip,
                               QRectF *targetOut, QRectF *sourceOut);

private:
    // Downscale past kMaxTilePixels and convert to RGB32; see the constant.
    static QImage prepare(const QImage &image);
    qint64 heldBytes(int pageNo) const;

    qint64 budget_;
    qint64 bytes_ = 0;
    QHash<int, Tile> tiles_;
};

} // namespace mervin
