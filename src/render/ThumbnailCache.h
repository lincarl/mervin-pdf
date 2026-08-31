#pragma once

#include <QHash>
#include <QList>
#include <QPixmap>

namespace mervin {

// A small, count-bounded cache of page thumbnails keyed by 0-based page index.
// Eviction is simple FIFO - thumbnails are cheap to re-render, so an exact LRU
// is unnecessary.
class ThumbnailCache
{
public:
    explicit ThumbnailCache(int maxItems = 256) : max_(maxItems) {}

    bool contains(int page) const { return map_.contains(page); }
    QPixmap get(int page) const { return map_.value(page); }

    void put(int page, const QPixmap &pm)
    {
        if (!map_.contains(page))
            order_.append(page);
        map_.insert(page, pm);
        while (order_.size() > max_) {
            const int evict = order_.takeFirst();
            map_.remove(evict);
        }
    }

    void clear()
    {
        map_.clear();
        order_.clear();
    }

private:
    QHash<int, QPixmap> map_;
    QList<int> order_;
    int max_;
};

} // namespace mervin
