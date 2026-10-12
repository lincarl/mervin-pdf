// The Linux mount list behind RecentStore::isVolumeAttached (src/recent/MountTable).
// holdingMount() is checked against a described system, since a test cannot mount
// or unmount volumes. The parsers and holdingMount() are plain text and list work,
// so they run on every platform. resolvedParent() needs symbolic links.
#include "recent/MountTable.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QTest>

using mervin::mounts::Mount;

namespace {

// One mountinfo line for a file system of `type` mounted on `point`, written as
// the kernel writes it.
QByteArray line(const QByteArray &point, const QByteArray &type)
{
    return "40 22 0:51 / " + point + " rw,relatime shared:7 - " + type + " src rw\n";
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
    + line("/archive", "xfs") + line("/srv/nas", "autofs") + line("/nas", "autofs")
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
    void holdingMount_data();
    void holdingMount();
    void mountedAutomountHoldsItsFiles();
    void resolvedParentFollowsLinks();
};

void TstMountTable::parseMountInfoReadsFoldersAndTypes()
{
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

// An fstab line written through a link names the folder its volume is mounted on.
void TstMountTable::resolvedParentFollowsLinks()
{
    // Not on Windows, where links need extra rights and the volume check does not
    // use fstab. A skip would fail CI.
#ifndef Q_OS_WIN
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString root = QFileInfo(dir.path()).canonicalFilePath();
    QVERIFY(QDir(root).mkpath(QStringLiteral("var/mnt")));
    QVERIFY(QFile::link(root + QStringLiteral("/var/mnt"), root + QStringLiteral("/mnt")));
    // The folder itself is a link here, and is not followed.
    QVERIFY(QFile::link(root + QStringLiteral("/elsewhere"), root + QStringLiteral("/var/mnt/usb")));

    using mervin::mounts::resolvedParent;
    QCOMPARE(resolvedParent(root + QStringLiteral("/mnt/data")),
             root + QStringLiteral("/var/mnt/data"));
    QCOMPARE(resolvedParent(root + QStringLiteral("/var/mnt/data")),
             root + QStringLiteral("/var/mnt/data"));
    QCOMPARE(resolvedParent(root + QStringLiteral("/mnt/usb")),
             root + QStringLiteral("/var/mnt/usb"));
    QCOMPARE(resolvedParent(root + QStringLiteral("/missing/data")),
             root + QStringLiteral("/missing/data"));
    QCOMPARE(resolvedParent(QStringLiteral("/data")), QStringLiteral("/data"));
    QCOMPARE(resolvedParent(QStringLiteral("/")), QStringLiteral("/"));

    // The resolved line keeps a canonical path on the unmounted volume away.
    const QList<Mount> mounted = mervin::mounts::parseMountInfo(line("/", "ext4"));
    const QString stored = root + QStringLiteral("/var/mnt/data/plan.pdf");
    QCOMPARE(mervin::mounts::holdingMount(stored, mounted, {root + QStringLiteral("/mnt/data")}),
             QStringLiteral("/"));
    QCOMPARE(mervin::mounts::holdingMount(
                 stored, mounted, {resolvedParent(root + QStringLiteral("/mnt/data"))}),
             QString());
#endif
}

QTEST_GUILESS_MAIN(TstMountTable)
#include "tst_mount_table.moc"
