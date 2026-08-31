#pragma once

#include "recent/RecentEntry.h"

#include <QList>
#include <QString>
#include <QStringList>

namespace mervin {

// The recent-files history: an ordered list (most-recent first) of opened
// files, persisted as JSON. Owned by the primary (single-instance) UI process,
// which is the sole writer; secondary launches hand off and never touch it.
// Dedup and case/separator matching use a normalized path key (see PathKey.h);
// the original spelling is preserved for display. The list is trimmed to a
// retention cap (spec default 500) on every mutation so it never grows without
// bound.
class RecentStore
{
public:
    explicit RecentStore(int retention = 500);

    void setRetention(int retention); // re-trims immediately if lowered
    int retention() const { return retention_; }

    // Move `path` to the front, stamped with `whenMs`, deduping any prior entry
    // for the same file (normalized match). No-op for an empty path. Returns
    // true if the list changed.
    bool add(const QString &path, qint64 whenMs, int pageCount = 0);

    // Remove the entry for `path` if present. Returns true if one was removed.
    bool remove(const QString &path);

    // Remove listed entries whose paths no longer exist on disk. Existing files
    // are left untouched even if listed. Returns removed paths.
    QStringList removeMissingFiles(const QStringList &paths);

    // Toggle the favourite flag on an existing entry. Returns true if the entry
    // was found and the flag actually changed.
    bool setFavorite(const QString &path, bool favorite);

    const QList<RecentEntry> &entries() const { return entries_; }
    int count() const { return static_cast<int>(entries_.size()); }
    void clear();

    // JSON persistence. load() replaces in-memory state; a missing or corrupt
    // file leaves the store empty (a safe default). Both return false on
    // I/O / parse failure.
    bool load(const QString &file);
    bool save(const QString &file) const;

    // Default location: %APPDATA%/MervinPDF/recent.json.
    static QString defaultFile();

private:
    void trim();

    QList<RecentEntry> entries_; // most-recent first (front == newest)
    int retention_;
};

} // namespace mervin
