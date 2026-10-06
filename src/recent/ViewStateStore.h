#pragma once

#include "recent/ViewState.h"

#include <QHash>
#include <QString>

#include <optional>

namespace mervin {

// Per-file persisted view state (last page + zoom + rotation), keyed by a
// normalized path. Owned by the primary (single-instance) UI process - the sole
// writer - and JSON-persisted alongside the recent list. Capped to the same
// retention as the recent history, evicting the least-recently-updated entries
// so it never grows unbounded for files long gone from history.
class ViewStateStore
{
public:
    explicit ViewStateStore(int retention = 500);

    void setRetention(int retention); // re-prunes immediately if lowered
    int retention() const { return retention_; }

    // Record/replace the view state for `path`, stamped with `whenMs` for LRU
    // eviction. No-op for an empty path.
    void put(const QString &path, const ViewState &state, qint64 whenMs);

    // Stored state for `path`, or nullopt if none recorded.
    std::optional<ViewState> get(const QString &path) const;

    bool remove(const QString &path);
    int count() const { return static_cast<int>(states_.size()); }
    void clear();

    bool load(const QString &file);
    bool save(const QString &file) const;

    // Default location: %APPDATA%/MervinPDF/viewstate.json.
    static QString defaultFile();

private:
    struct Record
    {
        ViewState state;
        qint64 updated = 0; // epoch ms of last put; smallest is evicted first
    };

    void prune();

    QHash<QString, Record> states_; // key == normalizePathKey(path)
    int retention_;
};

} // namespace mervin
