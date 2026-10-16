#pragma once

#include "recent/ViewState.h"

#include <QHash>
#include <QSet>
#include <QString>
#include <QStringList>

#include <optional>

namespace mervin {

// Per-file persisted view state (last page + zoom + rotation), keyed by a
// normalized path. Owned by the primary (single-instance) UI process - the sole
// writer - and JSON-persisted alongside the recent list. Capped to the same
// retention as the recent history, evicting the least-recently-updated entries
// so it never grows unbounded for files long gone from history. Kept paths
// (setKept), the Recent favourites, are never evicted and do not count against
// the cap, as in RecentStore.
class ViewStateStore
{
public:
    explicit ViewStateStore(int retention = 500);

    void setRetention(int retention); // re-prunes immediately if lowered
    int retention() const { return retention_; }

    // Replace the kept paths. A path no longer kept counts again from the next
    // prune, by put(), setRetention() or load(), so unmarking evicts nothing now.
    void setKept(const QStringList &paths);

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
    // What save() writes, for a write on another thread (appfiles::write).
    QByteArray toJson() const;

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
    QSet<QString> kept_;            // setKept() paths, by normalizePathKey
    int retention_;
};

} // namespace mervin
