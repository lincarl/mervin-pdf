#pragma once

#include "recent/RecentEntry.h"

#include <QList>
#include <QString>
#include <QStringList>

namespace mervin {

// JSON recent history, newest first, with a bounded retention count. The primary UI process is
// the sole writer. PathKey normalizes matching while original spelling is retained for display.
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

    // True when `path` is gone from a volume that is attached: the file, or a
    // folder above it, was deleted, moved or renamed. False while the file exists,
    // and false when its volume is away, so automatic pruning does not forget
    // entries on an unplugged drive or an offline share:
    // - Windows: the drive (D:/) or share (//server/share) must be reachable.
    // - Elsewhere: a path under /media, /run/media, /mnt or /net counts only while
    //   a volume is mounted below that folder, and a GVFS path only while its share
    //   folder exists; other paths are on the system's own disks.
    static bool isRemovedFromDisk(const QString &path);

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
