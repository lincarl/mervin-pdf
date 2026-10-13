// The Linux mount list behind RecentStore::isVolumeAttached (src/recent/MountTable).
// holdingMount() is checked against a described system, since a test cannot mount
// or unmount volumes. The parsers and holdingMount() are plain text and list work,
// so they run on every platform.
#include "recent/MountTable.h"

#include <QTest>

using mervin::mounts::Mount;

namespace {

// One mountinfo line for a file system of `type` mounted on `point`, written as
// the kernel writes it.
QByteArray line(const QByteArray &point, const QByteArray &type,
                const QByteArray &options = "rw")
{
    return "40 22 0:51 / " + point + " rw,relatime shared:7 - " + type + " src " + options + "\n";
}

// The system the holdingMount cases describe:
// - the system disk on /, a second disk on /home and one on /mnt itself;
// - /data in fstab and not mounted (an internal disk mounted on demand), and
//   /archive in fstab and mounted;
// - /srv/nas, an fstab share mounted on first use (x-systemd.automount), not
//   mounted now;
// - /nas, an autofs map that mounts each share below it, with /nas/docs mounted;
// - a USB drive in /media, with its mount point's space escaped.
const QByteArray kMountInfo = line("/", "ext4") + line("/home", "ext4") + line("/mnt", "ext4")
    + line("/archive", "xfs") + line("/srv/nas", "autofs", "rw,fd=4,pgrp=1,direct")
    + line("/nas", "autofs", "rw,fd=5,pgrp=1,indirect")
    + line("/nas/docs", "nfs4") + line("/media/ann/My\\040Disk", "vfat");

const QByteArray kFstab = "# <file system> <mount point> <type> <options> <dump> <pass>\n"
                          "UUID=1 / ext4 defaults 0 1\n"
                          "UUID=2 /home ext4 defaults 0 2\n"
                          "UUID=3 /data ext4 noauto,nofail 0 2\n"
                          "UUID=4 /archive xfs defaults 0 2\n"
                          "//nas/share /srv/nas cifs x-systemd.automount 0 0\n";

} // namespace

class TstMountTable : public QObject
{
    Q_OBJECT
private slots:
    void parseMountInfoReadsFoldersAndTypes();
    void parseFstabReadsFolders();
    void deepestMountIsTheFileSystemBelow();
    void holdingMount_data();
    void holdingMount();
    void mountedAutomountHoldsItsFiles();
    void answeringFolder_data();
    void answeringFolder();
};

void TstMountTable::parseMountInfoReadsFoldersAndTypes()
{
    {
        const QList<Mount> mounted = mervin::mounts::parseMountInfo(kMountInfo);
        for (const Mount &m : mounted)
            QCOMPARE(m.mapsFolders, m.point == QLatin1String("/nas"));
    }
    const QList<Mount> mounts = mervin::mounts::parseMountInfo(
        // Optional fields before the "-", none, and several.
        "22 1 8:2 / / rw,relatime shared:1 - ext4 /dev/sda2 rw,errors=remount-ro\n"
        "36 35 98:0 /mnt1 /mnt2 rw,noatime - ext3 /dev/root rw\n"
        "41 22 0:52 / /media/ann/My\\040Disk rw shared:9 master:3 - vfat /dev/sdb1 rw\n"
        "no separator here, so this line is skipped\n"
        "\n");
    QCOMPARE(mounts.size(), 3);
    QCOMPARE(mounts.at(0).point, QStringLiteral("/"));
    QCOMPARE(mounts.at(0).type, QByteArray("ext4"));
    QCOMPARE(mounts.at(1).point, QStringLiteral("/mnt2"));
    QCOMPARE(mounts.at(1).type, QByteArray("ext3"));
    QCOMPARE(mounts.at(2).point, QStringLiteral("/media/ann/My Disk"));
    QCOMPARE(mounts.at(2).type, QByteArray("vfat"));
}

void TstMountTable::parseFstabReadsFolders()
{
    const QStringList folders = mervin::mounts::parseFstab(
        "# /etc/fstab: static file system information.\n"
        "\n"
        "UUID=1\t/\text4\tdefaults\t0\t1\n"
        "  /dev/sdb1   /data/   ext4 noauto 0 2\n"
        "LABEL=Photos /srv/My\\040Photos ext4 defaults 0 2\n"
        "/swap.img\tnone\tswap\tsw\t0\t0\n"
        "UUID=5 swap swap defaults 0 0\n"
        "   # /old /old ext4 defaults 0 2\n");
    QCOMPARE(folders, QStringList({QStringLiteral("/"), QStringLiteral("/data"),
                                   QStringLiteral("/srv/My Photos")}));
}

// The file system holding a path now, with no rule about fstab or removable folders.
void TstMountTable::deepestMountIsTheFileSystemBelow()
{
    using mervin::mounts::deepestMount;
    const QList<Mount> mounted = mervin::mounts::parseMountInfo(kMountInfo);
    QCOMPARE(deepestMount(QStringLiteral("/home/ann/plan.pdf"), mounted), QStringLiteral("/home"));
    QCOMPARE(deepestMount(QStringLiteral("/opt/plan.pdf"), mounted), QStringLiteral("/"));
    QCOMPARE(deepestMount(QStringLiteral("/nas/docs/plan.pdf"), mounted),
             QStringLiteral("/nas/docs"));
    QCOMPARE(deepestMount(QStringLiteral("/media/ann/My Disk/plan.pdf"), mounted),
             QStringLiteral("/media/ann/My Disk"));
    QCOMPARE(deepestMount(QStringLiteral("/mnt/backup/plan.pdf"), mounted), QStringLiteral("/mnt"));
    // An automount point with nothing mounted on it holds no files.
    QCOMPARE(deepestMount(QStringLiteral("/srv/nas/plan.pdf"), mounted), QStringLiteral("/"));
    QCOMPARE(deepestMount(QStringLiteral("/srv/nas/plan.pdf"),
                          mervin::mounts::parseMountInfo(kMountInfo + line("/srv/nas", "cifs"))),
             QStringLiteral("/srv/nas"));
    QCOMPARE(deepestMount(QStringLiteral("/home/ann/plan.pdf"), {}), QStringLiteral("/"));
}

void TstMountTable::holdingMount_data()
{
    QTest::addColumn<QString>("path");
    QTest::addColumn<QString>("holder"); // "" when the volume is away

    QTest::newRow("system disk") << "/opt/docs/plan.pdf" << "/";
    QTest::newRow("second disk") << "/home/ann/plan.pdf" << "/home";
    QTest::newRow("fstab, mounted") << "/archive/plan.pdf" << "/archive";
    // Session restore's folder check cannot catch this case, because the file sits in
    // the empty folder an unmounted volume leaves behind.
    QTest::newRow("fstab, not mounted, top folder") << "/data/plan.pdf" << "";
    QTest::newRow("fstab, not mounted, below") << "/data/2026/plan.pdf" << "";
    QTest::newRow("folder that only starts like fstab's") << "/database/plan.pdf" << "/";
    QTest::newRow("automount, not mounted") << "/srv/nas/plan.pdf" << "";
    QTest::newRow("automount map, share mounted") << "/nas/docs/plan.pdf" << "/nas/docs";
    QTest::newRow("automount map, share not mounted") << "/nas/photos/plan.pdf" << "";
    QTest::newRow("removable, mounted") << "/media/ann/My Disk/plan.pdf" << "/media/ann/My Disk";
    QTest::newRow("removable, unplugged") << "/media/ann/USB/plan.pdf" << "";
    // /mnt is a disk of its own here, and nothing is mounted below it.
    QTest::newRow("below /mnt, nothing mounted") << "/mnt/backup/plan.pdf" << "";
    QTest::newRow("folder that only starts like /media") << "/mediafiles/plan.pdf" << "/";
}

void TstMountTable::holdingMount()
{
    QFETCH(QString, path);
    QFETCH(QString, holder);
    const QList<Mount> mounted = mervin::mounts::parseMountInfo(kMountInfo);
    const QStringList configured = mervin::mounts::parseFstab(kFstab);
    QCOMPARE(mervin::mounts::holdingMount(path, mounted, configured), holder);
    // Without fstab only the automount and removable cases stay away.
    const QString unlisted = mervin::mounts::holdingMount(path, mounted, {});
    QCOMPARE(unlisted, path.startsWith(QLatin1String("/data/")) ? QStringLiteral("/") : holder);
}

// Once the share is mounted on the automount point, the share holds the files.
void TstMountTable::mountedAutomountHoldsItsFiles()
{
    const QList<Mount> mounted =
        mervin::mounts::parseMountInfo(kMountInfo + line("/srv/nas", "cifs"));
    const QStringList configured = mervin::mounts::parseFstab(kFstab);
    QCOMPARE(mervin::mounts::holdingMount(QStringLiteral("/srv/nas/plan.pdf"), mounted, configured),
             QStringLiteral("/srv/nas"));
    // Nothing is mounted, not even the system disk, so only an fstab folder is away.
    QCOMPARE(mervin::mounts::holdingMount(QStringLiteral("/opt/plan.pdf"), {}, configured),
             QStringLiteral("/"));
    QCOMPARE(mervin::mounts::holdingMount(QStringLiteral("/data/plan.pdf"), {}, configured),
             QString());
}

void TstMountTable::answeringFolder_data()
{
    QTest::addColumn<QString>("path");
    QTest::addColumn<QString>("folder");

    QTest::newRow("system disk") << "/opt/docs/plan.pdf" << "/";
    QTest::newRow("second disk") << "/home/ann/plan.pdf" << "/home";
    QTest::newRow("share mounted below an automount map") << "/nas/docs/plan.pdf" << "/nas/docs";
    // Each share of the map has a folder of its own, mounted or not, so one that
    // stops answering holds up only its own files.
    QTest::newRow("automount map, share not mounted") << "/nas/photos/2026/plan.pdf"
                                                      << "/nas/photos";
    QTest::newRow("automount point itself") << "/nas" << "/nas";
    // A systemd automount mounts its share on the point itself.
    QTest::newRow("fstab automount, not mounted") << "/srv/nas/2026/plan.pdf" << "/srv/nas";
    QTest::newRow("removable") << "/media/ann/My Disk/plan.pdf" << "/media/ann/My Disk";
    QTest::newRow("GVFS share") << "/run/user/1000/gvfs/smb-share:server=nas,share=docs/a/plan.pdf"
                                << "/run/user/1000/gvfs/smb-share:server=nas,share=docs";
}

void TstMountTable::answeringFolder()
{
    QFETCH(QString, path);
    QFETCH(QString, folder);
    const QList<Mount> mounted = mervin::mounts::parseMountInfo(
        kMountInfo + line("/run/user/1000/gvfs", "fuse.gvfsd-fuse"));
    QCOMPARE(mervin::mounts::answeringFolder(path, mounted), folder);
    QCOMPARE(mervin::mounts::answeringFolder(path, {}), QStringLiteral("/"));
}

QTEST_GUILESS_MAIN(TstMountTable)
#include "tst_mount_table.moc"
