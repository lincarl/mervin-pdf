#pragma once

#include <QImage>
#include <QMetaType>
#include <QRect>
#include <QString>

#include <cstdint>

namespace mervin {

class Document;

// How a rendered page is toned, driven by the document-theme setting.
//  - Light:    the page as authored (classic white paper).
//  - Inverted: plain colour negative (QImage::invertPixels, applied by the viewer).
//  - Comfort:  a colour-aware dark mode applied by the render worker: white
//    paper -> dark grey, black ink -> light grey, coloured text and photo
//    content keep their authored hues (see ComfortTransform for the full
//    per-pixel and per-image rules).
enum class PageTheme { Light, Inverted, Comfort };

// A request to render one page at a given scale/rotation. Pushed onto the
// RenderEngine's internal queue (not sent across Qt signals), so it may carry a
// raw Document pointer (valid for the lifetime of the owning document).
struct RenderRequest
{
    Document *document = nullptr;
    quint64 requester = 0; // id of the viewer that issued this request (echoed in the result)
    int pageNo = 0;
    double scale = 1.0;   // 1.0 == 72 DPI (one point per pixel)
    int rotation = 0;     // 0 / 90 / 180 / 270 degrees, clockwise
    quint64 epoch = 0;    // requests older than the engine's current epoch are dropped
    quint64 token = 0;    // unique id for correlation
    // Sub-region to render, in device pixels relative to the page's full
    // (scaled+rotated) image top-left. Empty (the default) renders the whole
    // page; a non-empty clip renders only that band for deep-zoom tiling.
    QRect clip;
    // Comfort is applied in the worker; Light/Inverted render identically here
    // (the viewer inverts on receipt), so only Comfort changes the output.
    PageTheme theme = PageTheme::Light;
};

// The result of a render, delivered to the UI thread via a queued signal.
// resultReady is broadcast to every viewer, so `requester` identifies which one
// issued the request; a viewer ignores results whose requester is not its own.
struct RenderResult
{
    quint64 requester = 0; // viewer id copied from the originating request
    int pageNo = 0;
    double scale = 1.0;
    int rotation = 0;
    quint64 epoch = 0;
    quint64 token = 0;
    QImage image;
    bool ok = false;
    QString error;
};

} // namespace mervin

Q_DECLARE_METATYPE(mervin::RenderResult)
