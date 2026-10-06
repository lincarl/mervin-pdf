#include "render/PageCache.h"

namespace mervin {

const PageCache::Entry *PageCache::get(int pageNo)
{
    auto it = index_.find(pageNo);
    if (it == index_.end())
        return nullptr;
    lru_.splice(lru_.begin(), lru_, it->second); // move to front
    return &*it->second;
}

void PageCache::put(int pageNo, const QImage &image, const QRect &covered)
{
    auto it = index_.find(pageNo);
    if (it != index_.end()) {
        bytes_ -= it->second->image.sizeInBytes();
        it->second->image = image;
        it->second->covered = covered;
        bytes_ += image.sizeInBytes();
        lru_.splice(lru_.begin(), lru_, it->second);
        evictIfNeeded();
        return;
    }

    lru_.push_front(Entry{pageNo, image, covered});
    index_[pageNo] = lru_.begin();
    bytes_ += image.sizeInBytes();
    evictIfNeeded();
}

void PageCache::evictIfNeeded()
{
    while (bytes_ > budget_ && lru_.size() > 1) {
        Entry &back = lru_.back();
        bytes_ -= back.image.sizeInBytes();
        index_.erase(back.pageNo);
        lru_.pop_back();
    }
}

void PageCache::erase(int pageNo)
{
    auto it = index_.find(pageNo);
    if (it == index_.end())
        return;
    bytes_ -= it->second->image.sizeInBytes();
    lru_.erase(it->second);
    index_.erase(it);
}

void PageCache::clear()
{
    lru_.clear();
    index_.clear();
    bytes_ = 0;
}

} // namespace mervin
