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
    // for the same file (normalized match). `mount` is the file system the file
    // is on now (mountOf), kept as RecentEntry::mount. No-op for an empty path.
    // Returns true if the list changed.
    bool add(const QString &path, qint64 whenMs, int pageCount = 0,
             const QString &mount = QString());

    // Remove the entry for `path` if present. Returns true if one was removed.
    bool remove(const QString &path);

    // Remove listed entries whose paths no longer exist on disk. Existing files
    // are left untouched even if listed. Returns removed paths.
    QStringList removeMissingFiles(const QStringList &paths);

    // True when `path` is gone from a volume that is attached: the file, or a
    // folder above it, was deleted, moved or renamed. False while the file exists,
    // and false when its volume is away (isVolumeAttached), so automatic pruning
    // does not forget entries on an unplugged drive or an offline share.
    static bool isRemovedFromDisk(const QString &path, const QString &mount = QString());

    // Whether the volume holding `path` is attached. Session restore uses it too.
    // - Windows: the drive (D:/) or share (//server/share) must be reachable.
    // - Elsewhere: a GVFS path counts only while its share folder exists.
    //   Otherwise the deepest mounted file system above the path holds it, and
    //   must answer. The volume is away when a folder that /etc/fstab mounts a
    //   volume on, or an automount point, lies between that file system and the
    //   path, or when the path is under /media, /run/media, /mnt or /net with no
    //   volume mounted below that folder (mounts::holdingMount). Symbolic links
    //   are not followed, so pass a canonical path as Recent and the session do.
    //   With `mount`, the mount point the file was opened from (RecentEntry::mount),
    //   the volume is also away while another file system holds the path and that
    //   folder still exists. This covers a share mounted by hand outside fstab. A
    //   recorded mount never makes a volume attached that the rules above call away.
    // On an offline share the check can stall for seconds.
    static bool isVolumeAttached(const QString &path, const QString &mount = QString());
    // The mount point of the file system that holds `path` now, from the kernel's
    // mount list alone, without touching the path. "" on Windows or without that
    // list. Record it only while the file is there.
    static QString mountOf(const QString &path);
    // The mount recorded with the entry for `path`, or "" when there is none.
    QString recordedMount(const QString &path) const;
    // Paths with the same key and the same recorded mount get the same
    // isVolumeAttached() answer, so a caller can check each drive or share once:
    // "D:" or "//server/share" on Windows, the containing folder elsewhere.
    static QString volumeKey(const QString &path);

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
