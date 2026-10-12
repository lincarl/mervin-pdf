#pragma once

#include <QByteArray>
#include <QList>
#include <QString>
#include <QStringList>

// The Linux mount list behind RecentStore::isVolumeAttached. The parsers and
// holdingMount() work on text and lists, so the tests can describe any system.
namespace mervin::mounts {

// A mounted file system, as /proc/self/mountinfo lists it.
struct Mount
{
    QString point;   // the folder it is mounted on, without a trailing slash
    QByteArray type; // "ext4", "cifs", "autofs" and so on
};

// The mounts in the text of /proc/self/mountinfo, in the order listed.
QList<Mount> parseMountInfo(const QByteArray &text);

// The folders the text of /etc/fstab mounts volumes on. Swap and other entries
// without a folder are left out.
QStringList parseFstab(const QByteArray &text);

// The mount point of the volume that holds `path`, a clean absolute path with '/'
// separators, or "" when that volume is not mounted. The holder is the deepest
// mounted file system above the path. The volume is away when a folder that
// `configured` lists, or an automount point, lies between the holder and the path,
// or when the path is under /media, /run/media, /mnt or /net and no volume is
// mounted below that folder.
QString holdingMount(const QString &path, const QList<Mount> &mounted,
                     const QStringList &configured);

// `folder` with the symbolic links above it followed, so an fstab line written
// through a link (/mnt/data where /mnt links to /var/mnt) names the folder the
// volume is mounted on, as the stored canonical paths do. The folder itself is
// not touched, so an automount point is not mounted. A parent that cannot be
// resolved is kept as written.
QString resolvedParent(const QString &folder);

} // namespace mervin::mounts
