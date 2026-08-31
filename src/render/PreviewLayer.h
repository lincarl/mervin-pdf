#pragma once

#include <QHash>
#include <QImage>
#include <QRect>
#include <QRectF>

#include <vector>

namespace mervin {

// The last images we managed to render for the pages on screen, frozen across a
// scale / layout change so the viewer can paint them stretched into the new page
// rects instead of flashing blank paper while the sharp re-render runs (a full
// page rasterization costs tens of ms at normal zoom and hundreds at deep zoom,
// so the gap is very visible).
//
// A tile records the region it covers as a FRACTION of the page rect it was
// rendered for, not in pixels. The fraction is scale-independent, so one tile
// maps correctly into that page's rect at any later scale and a burst of zoom
// steps needs no re-derivation - tiles from different zoom generations can sit
// side by side and each still lands in the right place.
//
// Only content-preserving layout changes may keep a tile: a different scale,
// device pixel ratio or page mode all leave the page's aspect and colours alone.
// Rotation (which flips the rect's aspect), a page-theme change (which re-tones
// the pixels) and a new document must clear the layer instead.
class PreviewLayer
{
public:
    struct Tile
    {
        QImage image;
        QRectF frac; // covered region as a fraction (0..1) of the page's rect
    };

    // Tiles are re-encoded on the way in, because unlike the render cache they
    // are read back through a scaling blit on every frame until the sharp render
    // lands:
    //  - to Format_RGB32. The render cache is RGB888 (three bytes per pixel),
    //    which the raster engine has no fast transformed-blend path for: the
    //    same stretch measures 3-4x slower from RGB888 than from RGB32.
    //  - down to at most kMaxTilePixels. A stand-in never needs more pixels than
    //    the window can show, and this bounds both the re-encode and the memory
    //    a frozen tile holds (a deep-zoom render can be 32 MPx on its own).
    // Cost is a few ms once per zoom gesture, against the tens to hundreds of ms
    // of rendering it covers for.
    static constexpr qint64 kMaxTilePixels = 8ll * 1024 * 1024;

    // Room for a couple of full-size tiles (kMaxTilePixels at 4 bytes == 32 MB)
    // or a screenful of smaller ones. Callers add the page the user is looking at
    // first, so it is the one that always fits.
    explicit PreviewLayer(qint64 budgetBytes = 64ll * 1024 * 1024)
        : budget_(budgetBytes) {}

    // Freeze `image` - which covers `covered` of page `pageNo`, laid out at
    // `pageRect` - as that page's preview, replacing any tile it holds. Returns
    // whether it was stored.
    //
    // Rejected when the image is null, the rects are degenerate, `covered` is not
    // inside `pageRect` (the stored fraction describes the whole image, so the
    // two must agree), or the byte budget is spent - except that an empty layer
    // always accepts its first tile.
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

    // Split a stretch into the part that is actually visible: `targetOut` is
    // `target` clipped to `clip`, and `sourceOut` the matching sub-rect of
    // `image`. Returns false when nothing shows.
    //
    // Needed because at deep zoom the full target rect is hundreds of thousands
    // of pixels across; handing that to the raster engine risks coordinate
    // overflow and would make the cost of the blit depend on the zoom level
    // instead of on the size of the window.
    //
    // `sourceOut` is in raw image pixels, NOT device-independent ones: the
    // QPainter::drawImage overload that takes an explicit source rect does not
    // apply the image's devicePixelRatio (qpainter.h defines the whole-image
    // convenience overload as QRect(0, 0, image.width(), image.height())).
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
