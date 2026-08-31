#pragma once

#include <QImage>
#include <QRect>

#include <list>
#include <unordered_map>

namespace mervin {

// Simple byte-budgeted LRU cache of rendered page images, keyed by page number.
// The cache holds images for the *current* scale/rotation only; the viewer
// clear()s it whenever scale or rotation changes, so the page number is a
// sufficient key. Each entry records the canvas-space rectangle the image
// covers: the whole page rect for a normally-rendered page, or just the visible
// band for a deep-zoom clipped tile.
class PageCache
{
public:
    struct Entry
    {
        int pageNo;
        QImage image;
        QRect covered; // canvas-space region this image represents
    };

    explicit PageCache(qint64 budgetBytes = 256ll * 1024 * 1024)
        : budget_(budgetBytes) {}

    const Entry *get(int pageNo);
    void put(int pageNo, const QImage &image, const QRect &covered);
    // Drop a single page's cached image (e.g. after a form-field edit, so only the
    // touched page re-renders instead of clearing the whole cache). No-op if absent.
    void erase(int pageNo);
    void clear();

private:
    void evictIfNeeded();

    qint64 budget_;
    qint64 bytes_ = 0;
    std::list<Entry> lru_; // front = most recently used
    std::unordered_map<int, std::list<Entry>::iterator> index_;
};

} // namespace mervin
