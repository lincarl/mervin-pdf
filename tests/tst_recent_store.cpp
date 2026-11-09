#include "recent/RecentStore.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QStringList>
#include <QTemporaryDir>
#include <QTest>

using mervin::RecentEntry;
using mervin::RecentStore;

class TstRecentStore : public QObject
{
    Q_OBJECT

private slots:
    void addPrependsMostRecent();
    void addDedupesAndMovesToFront();
    void addKeepsTheKnownPageCount();
#ifdef Q_OS_WIN
    void dedupIsCaseInsensitiveOnWindows();
#endif
    void retentionTrimsOldest();
    void loweringRetentionRetrims();
    void loweringRetentionKeepsFavourites();
    void addingKeepsOldFavourites();
    void loadKeepsFavouritesPastTheRetention();
    void removeDeletesEntry();
    void removeEntriesRemovesOnlyThoseListed();
    void setMountRecordsTheCurrentMount();
    void setMissingMountsFillsOnlyEmptyMounts();
    void removedFromDiskNeedsAnAttachedVolume();
    void recordedMountJudgesTheVolume();
    void emptyPathIgnored();
    void jsonRoundTrip();
    void stableJsonAndMalformedFiles();
    void printPreferencesRoundTripAndFileIsolation();
    void printPreferencesSurviveReopenAndRemoval();
    void printPreferencesRejectMalformedFields();
    void loadMissingFileLeavesEmpty();
};

// Authored JSON protects compact keys, favourites and mount identity across versions.
void TstRecentStore::stableJsonAndMalformedFiles()
{
    QTemporaryDir dir;
    const QString path = dir.filePath(QStringLiteral("recent.json"));
    const QByteArray literal = R"([{"p":"/records/a.pdf","t":12345,"n":8,"f":true,"m":"/records"},{"p":"/records/b.pdf","t":12000}])";
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    QCOMPARE(file.write(literal), literal.size());
    file.close();
    RecentStore store;
    QVERIFY(store.load(path));
    QCOMPARE(store.count(), 2);
    const auto &first = store.entries().front();
    QCOMPARE(first.path, QStringLiteral("/records/a.pdf"));
    QCOMPARE(first.lastOpened, qint64(12345));
    QCOMPARE(first.pageCount, 8);
    QVERIFY(first.favorite);
    QCOMPARE(first.mount, QStringLiteral("/records"));
    QCOMPARE(store.entries().at(1).pageCount, 0);
    QVERIFY(!store.entries().at(1).favorite);
    QVERIFY(store.entries().at(1).mount.isEmpty());
    QVERIFY(!store.entries().at(1).printPreferences);
    QCOMPARE(QJsonDocument::fromJson(store.toJson()), QJsonDocument::fromJson(literal));
    for (const QByteArray invalid : {QByteArray("{}"), QByteArray("[broken")}) {
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
        QCOMPARE(file.write(invalid), invalid.size());
        file.close();
        QVERIFY(!store.load(path));
        QCOMPARE(store.count(), 0);
    }
}

void TstRecentStore::addPrependsMostRecent()
{
    RecentStore s;
    s.add(QStringLiteral("C:/a.pdf"), 100);
    s.add(QStringLiteral("C:/b.pdf"), 200);
    QCOMPARE(s.count(), 2);
    QCOMPARE(s.entries()[0].path, QStringLiteral("C:/b.pdf")); // newest first
    QCOMPARE(s.entries()[1].path, QStringLiteral("C:/a.pdf"));
}

void TstRecentStore::addDedupesAndMovesToFront()
{
    RecentStore s;
    s.add(QStringLiteral("C:/a.pdf"), 100);
    s.add(QStringLiteral("C:/b.pdf"), 200);
    s.add(QStringLiteral("C:/a.pdf"), 300); // re-open a.pdf
    QCOMPARE(s.count(), 2);                  // not duplicated
    QCOMPARE(s.entries()[0].path, QStringLiteral("C:/a.pdf"));
    QCOMPARE(s.entries()[0].lastOpened, qint64(300)); // timestamp refreshed
    QCOMPARE(s.entries()[1].path, QStringLiteral("C:/b.pdf"));
}

// A tab restored without loading its document does not know the page count yet.
// Recording it keeps the count Recent already shows, and a known count replaces it.
void TstRecentStore::addKeepsTheKnownPageCount()
{
    RecentStore s;
    s.add(QStringLiteral("C:/a.pdf"), 100, 12);
    s.add(QStringLiteral("C:/a.pdf"), 200, 0);
    QCOMPARE(s.entries()[0].pageCount, 12);
    QCOMPARE(s.entries()[0].lastOpened, qint64(200));
    s.add(QStringLiteral("C:/a.pdf"), 300, 5);
    QCOMPARE(s.entries()[0].pageCount, 5);
    s.add(QStringLiteral("C:/b.pdf"), 400, 0);
    QCOMPARE(s.entries()[0].pageCount, 0); // a new file has no count to keep
}

#ifdef Q_OS_WIN
void TstRecentStore::dedupIsCaseInsensitiveOnWindows()
{
    RecentStore s;
    s.add(QStringLiteral("C:/Docs/Report.pdf"), 100);
    s.add(QStringLiteral("c:\\docs\\report.pdf"), 200); // same file, different spelling
    QCOMPARE(s.count(), 1);
    QCOMPARE(s.entries()[0].lastOpened, qint64(200));
}
#endif

void TstRecentStore::retentionTrimsOldest()
{
    RecentStore s(3);
    for (int i = 0; i < 5; ++i)
        s.add(QStringLiteral("C:/f%1.pdf").arg(i), i);
    QCOMPARE(s.count(), 3);
    QCOMPARE(s.entries()[0].path, QStringLiteral("C:/f4.pdf")); // newest kept
    QCOMPARE(s.entries()[2].path, QStringLiteral("C:/f2.pdf")); // oldest dropped (f0, f1)
}

void TstRecentStore::loweringRetentionRetrims()
{
    RecentStore s(10);
    for (int i = 0; i < 6; ++i)
        s.add(QStringLiteral("C:/f%1.pdf").arg(i), i);
    QCOMPARE(s.count(), 6);
    s.setRetention(2);
    QCOMPARE(s.count(), 2);
    QCOMPARE(s.entries()[0].path, QStringLiteral("C:/f5.pdf"));
}

// Favourites never age out, and only the other entries count against the
// retention, so a lower retention still keeps that many of them.
void TstRecentStore::loweringRetentionKeepsFavourites()
{
    RecentStore s(10);
    for (int i = 0; i < 5; ++i)
        s.add(QStringLiteral("C:/f%1.pdf").arg(i), i);
    QVERIFY(s.setFavorite(QStringLiteral("C:/f0.pdf"), true)); // the oldest
    s.setRetention(2);
    QCOMPARE(s.count(), 3);
    QCOMPARE(s.entries()[0].path, QStringLiteral("C:/f4.pdf"));
    QCOMPARE(s.entries()[1].path, QStringLiteral("C:/f3.pdf"));
    QCOMPARE(s.entries()[2].path, QStringLiteral("C:/f0.pdf"));
    QVERIFY(s.entries()[2].favorite);

    // Unstarred, it counts again and goes at the next trim.
    QVERIFY(s.setFavorite(QStringLiteral("C:/f0.pdf"), false));
    s.setRetention(2);
    QCOMPARE(s.count(), 2);
    QCOMPARE(s.entries()[1].path, QStringLiteral("C:/f3.pdf"));
}

void TstRecentStore::addingKeepsOldFavourites()
{
    RecentStore s; // the default retention, 500
    s.add(QStringLiteral("C:/star.pdf"), 0);
    QVERIFY(s.setFavorite(QStringLiteral("C:/star.pdf"), true));
    for (int i = 1; i <= 500; ++i)
        s.add(QStringLiteral("C:/f%1.pdf").arg(i), i);
    QCOMPARE(s.count(), 501);
    QCOMPARE(s.entries().last().path, QStringLiteral("C:/star.pdf"));
    QVERIFY(s.entries().last().favorite);
    s.add(QStringLiteral("C:/f501.pdf"), 501);
    QCOMPARE(s.count(), 501); // f1, the oldest other entry, went instead
    QCOMPARE(s.entries()[499].path, QStringLiteral("C:/f2.pdf"));
    QCOMPARE(s.entries().last().path, QStringLiteral("C:/star.pdf"));
}

// A file with more entries than the retention, such as one saved before the
// retention was lowered, keeps its favourites when it is read.
void TstRecentStore::loadKeepsFavouritesPastTheRetention()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString file = dir.filePath(QStringLiteral("recent.json"));
    RecentStore a(10);
    for (int i = 0; i < 5; ++i)
        a.add(QStringLiteral("C:/f%1.pdf").arg(i), i);
    QVERIFY(a.setFavorite(QStringLiteral("C:/f1.pdf"), true));
    QVERIFY(a.save(file));

    RecentStore b(2);
    QVERIFY(b.load(file));
    QCOMPARE(b.count(), 3);
    QCOMPARE(b.entries()[0].path, QStringLiteral("C:/f4.pdf"));
    QCOMPARE(b.entries()[1].path, QStringLiteral("C:/f3.pdf"));
    QCOMPARE(b.entries()[2].path, QStringLiteral("C:/f1.pdf"));
    QVERIFY(b.entries()[2].favorite);
}

void TstRecentStore::removeDeletesEntry()
{
    RecentStore s;
    s.add(QStringLiteral("C:/a.pdf"), 100);
    s.add(QStringLiteral("C:/b.pdf"), 200);
    QVERIFY(s.remove(QStringLiteral("C:/a.pdf")));
    QCOMPARE(s.count(), 1);
    QCOMPARE(s.entries()[0].path, QStringLiteral("C:/b.pdf"));
    QVERIFY(!s.remove(QStringLiteral("C:/missing.pdf")));
}

void TstRecentStore::removeEntriesRemovesOnlyThoseListed()
{
    // The checks happen elsewhere, so the store removes what it is told to, in
    // any spelling of the path, without looking at the disk.
    RecentStore s;
    s.add(QStringLiteral("C:/a.pdf"), 100);
    s.add(QStringLiteral("C:/b.pdf"), 200);
    s.add(QStringLiteral("C:/c.pdf"), 300);

    QCOMPARE(s.removeEntries({QStringLiteral("C:/b.pdf"), QStringLiteral("C:/x.pdf")}),
             QStringList{QStringLiteral("C:/b.pdf")});
    QCOMPARE(s.count(), 2);
    QCOMPARE(s.entries()[0].path, QStringLiteral("C:/c.pdf"));
    QCOMPARE(s.entries()[1].path, QStringLiteral("C:/a.pdf"));
    QCOMPARE(s.removeEntries({QStringLiteral("C:/./a.pdf")}), QStringList{QStringLiteral("C:/a.pdf")});
    QVERIFY(s.removeEntries({}).isEmpty());
    QCOMPARE(s.count(), 1);
}

void TstRecentStore::setMountRecordsTheCurrentMount()
{
    RecentStore s;
    s.add(QStringLiteral("/home/ann/nas/a.pdf"), 100, 0, QStringLiteral("/home/ann/nas"));
    QVERIFY(!s.setMount(QStringLiteral("/home/ann/nas/a.pdf"), QStringLiteral("/home/ann/nas")));
    QVERIFY(s.setMount(QStringLiteral("/home/ann/nas/a.pdf"), QStringLiteral("/")));
    QCOMPARE(s.recordedMount(QStringLiteral("/home/ann/nas/a.pdf")), QStringLiteral("/"));
    QVERIFY(!s.setMount(QStringLiteral("/home/ann/other.pdf"), QStringLiteral("/")));
    QCOMPARE(s.count(), 1);
}

// Checks of listed files fill in the mount of entries an older version wrote.
// They never replace a recorded one; only an open does (add, setMount).
void TstRecentStore::setMissingMountsFillsOnlyEmptyMounts()
{
    RecentStore s;
    s.add(QStringLiteral("/home/ann/nas/old.pdf"), 100);
    s.add(QStringLiteral("/home/ann/nas/new.pdf"), 200, 0, QStringLiteral("/home/ann/nas"));
    s.add(QStringLiteral("/home/ann/notes.pdf"), 300);

    QVERIFY(!s.setMissingMounts({}));
    QVERIFY(!s.setMissingMounts({{QStringLiteral("/home/ann/nas/old.pdf"), QString()}}));
    QVERIFY(!s.setMissingMounts({{QStringLiteral("/home/ann/nas/new.pdf"), QStringLiteral("/")},
                                 {QStringLiteral("/home/ann/gone.pdf"), QStringLiteral("/")}}));
    QCOMPARE(s.recordedMount(QStringLiteral("/home/ann/nas/new.pdf")), QStringLiteral("/home/ann/nas"));
    QCOMPARE(s.count(), 3);

    QVERIFY(s.setMissingMounts({{QStringLiteral("/home/ann/nas/./old.pdf"), QStringLiteral("/home/ann/nas")},
                                {QStringLiteral("/home/ann/nas/new.pdf"), QStringLiteral("/")}}));
    QCOMPARE(s.recordedMount(QStringLiteral("/home/ann/nas/old.pdf")), QStringLiteral("/home/ann/nas"));
    QCOMPARE(s.recordedMount(QStringLiteral("/home/ann/nas/new.pdf")), QStringLiteral("/home/ann/nas"));
    QCOMPARE(s.recordedMount(QStringLiteral("/home/ann/notes.pdf")), QString());
    QVERIFY(!s.setMissingMounts({{QStringLiteral("/home/ann/nas/old.pdf"), QStringLiteral("/")}}));
}

// Automatic pruning forgets a file that was deleted or moved, including with its
// folder, but never one whose drive or share is away.
void TstRecentStore::removedFromDiskNeedsAnAttachedVolume()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString existing = dir.filePath(QStringLiteral("existing.pdf"));
    QFile file(existing);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.close();

    QVERIFY(!RecentStore::isRemovedFromDisk(existing));
    QVERIFY(!RecentStore::isRemovedFromDisk(QString()));
    QVERIFY(RecentStore::isRemovedFromDisk(dir.filePath(QStringLiteral("deleted.pdf"))));
    QVERIFY(RecentStore::isRemovedFromDisk(dir.filePath(QStringLiteral("deleted-folder/plan.pdf"))));
    QVERIFY(QFile::remove(existing));
    QVERIFY(RecentStore::isRemovedFromDisk(existing));

#ifdef Q_OS_WIN
    QVERIFY(!RecentStore::isRemovedFromDisk(
        QStringLiteral("//mervin-test-no-such-server/share/report.pdf")));
#else
    // Unplugged removable media, an unmounted mount point (often an empty folder)
    // and a GNOME network share that is not mounted.
    QVERIFY(!RecentStore::isRemovedFromDisk(
        QStringLiteral("/media/mervin-test-no-such-user/USB/report.pdf")));
    QVERIFY(!RecentStore::isRemovedFromDisk(QStringLiteral("/mnt/mervin-test-absent/plan.pdf")));
    QVERIFY(!RecentStore::isRemovedFromDisk(
        QStringLiteral("/run/user/0/gvfs/smb-share:server=nas,share=docs/plan.pdf")));
#endif
}

// A recorded mount adds one case to the mount list rules. While the folder the
// file was opened from is still there and another file system holds the path, the
// volume is away. It never makes a volume attached that those rules call away.
void TstRecentStore::recordedMountJudgesTheVolume()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString gone = dir.filePath(QStringLiteral("gone.pdf"));
    const QString mount = RecentStore::mountOf(gone);
    RecentStore s;
    s.add(gone, 100, 0, mount);
    QCOMPARE(s.recordedMount(gone), mount);
    QCOMPARE(s.recordedMount(dir.filePath(QStringLiteral("other.pdf"))), QString());
#ifdef Q_OS_WIN
    QVERIFY(mount.isEmpty());
#else
    QVERIFY(mount.startsWith(QLatin1Char('/')));
    QVERIFY(RecentStore::isVolumeAttached(gone, mount));
    QVERIFY(RecentStore::isRemovedFromDisk(gone, mount));
    // As if a share had been mounted on the folder by hand and is unmounted now.
    QVERIFY(!RecentStore::isVolumeAttached(gone, dir.path()));
    QVERIFY(!RecentStore::isRemovedFromDisk(gone, dir.path()));
    // A mount folder that went with its volume, as udisks and kio-fuse leave it,
    // falls back to the mount list, which finds the file deleted.
    const QString goneFolder = dir.filePath(QStringLiteral("kio-fuse-AbCdEf"));
    QVERIFY(RecentStore::isRemovedFromDisk(gone, goneFolder));
    // Under /media the mount list keeps an unplugged drive away, also when the file
    // system recorded for it holds the path now.
    const QString usb = QStringLiteral("/media/mervin-test-no-such-user/USB/report.pdf");
    QVERIFY(!RecentStore::isRemovedFromDisk(usb, RecentStore::mountOf(usb)));
    // Opening the file again records the mount again.
    s.add(gone, 200, 0, dir.path());
    QCOMPARE(s.recordedMount(gone), dir.path());
#endif
}

void TstRecentStore::emptyPathIgnored()
{
    RecentStore s;
    QVERIFY(!s.add(QString(), 100));
    QCOMPARE(s.count(), 0);
}

void TstRecentStore::jsonRoundTrip()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString file = dir.filePath(QStringLiteral("recent.json"));

    RecentStore a;
    a.add(QStringLiteral("C:/a.pdf"), 111);
    a.add(QStringLiteral("C:/b.pdf"), 222, 0, QStringLiteral("/media/ann/USB"));
    QVERIFY(a.save(file));

    RecentStore b;
    QVERIFY(b.load(file));
    QCOMPARE(b.count(), 2);
    QCOMPARE(b.entries()[0].path, QStringLiteral("C:/b.pdf"));
    QCOMPARE(b.entries()[0].lastOpened, qint64(222));
    QCOMPARE(b.entries()[0].mount, QStringLiteral("/media/ann/USB"));
    QCOMPARE(b.entries()[1].path, QStringLiteral("C:/a.pdf"));
    QCOMPARE(b.entries()[1].mount, QString());
}

void TstRecentStore::loadMissingFileLeavesEmpty()
{
    RecentStore s;
    s.add(QStringLiteral("C:/a.pdf"), 100);
    QVERIFY(!s.load(QStringLiteral("C:/no/such/file-mervin-test.json")));
    QCOMPARE(s.count(), 0); // load clears even on failure - a safe empty default
}

// Stores all selections per file while leaving recent ordering and open times untouched.
void TstRecentStore::printPreferencesRoundTripAndFileIsolation()
{
    using namespace mervin::printing;
    Preferences first;
    first.printerName = QStringLiteral("Office printer");
    first.printToFile = true;
    first.paperKey = QStringLiteral("CustomStock");
    first.paperPoints = QSizeF(510.25, 737.5);
    first.orientation = QPageLayout::Landscape;
    first.pageSelection = PageSelection::Custom;
    first.fromPage = 2;
    first.toPage = 8;
    first.customPages = QStringLiteral("8,2-4,8");
    first.copies = 3;
    first.settings.scaleMode = ScaleMode::FitToPage;
    first.settings.scalePercent = 67;
    first.settings.alignment = Alignment::CenteredHorizontally;
    first.settings.colourMode = ColourMode::BlackAndWhite;
    first.settings.qualityDpi = 600;
    first.duplex = DuplexMode::ShortSide;
    Preferences second;
    second.settings.scaleMode = ScaleMode::Custom;
    second.settings.scalePercent = 125;
    second.pageSelection = PageSelection::Current;

    RecentStore store;
    QVERIFY(store.add(QStringLiteral("/records/first.pdf"), 100, 8));
    QVERIFY(store.add(QStringLiteral("/records/second.pdf"), 200, 5));
    QVERIFY(store.setPrintPreferences(QStringLiteral("/records/./first.pdf"), first));
    QVERIFY(store.setPrintPreferences(QStringLiteral("/records/second.pdf"), second));
    QCOMPARE(store.entries().first().path, QStringLiteral("/records/second.pdf"));
    QCOMPARE(store.entries().first().lastOpened, qint64(200));
    QCOMPARE(store.entries().last().lastOpened, qint64(100));
    QVERIFY(store.printPreferences(QStringLiteral("/records/first.pdf")) == first);
    QVERIFY(store.printPreferences(QStringLiteral("/records/second.pdf")) == second);

    const QJsonObject saved = QJsonDocument::fromJson(store.toJson()).array().last().toObject()
                                 .value(QStringLiteral("print")).toObject();
    QCOMPARE(saved.value(QStringLiteral("scaleMode")).toString(), QStringLiteral("fit"));
    QCOMPARE(saved.value(QStringLiteral("scalePercent")).toInt(), 67);
    QCOMPARE(saved.value(QStringLiteral("pageSelection")).toString(), QStringLiteral("custom"));
    QCOMPARE(saved.value(QStringLiteral("colourMode")).toString(), QStringLiteral("black-and-white"));
    QCOMPARE(saved.value(QStringLiteral("duplex")).toString(), QStringLiteral("short-side"));
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("recent.json"));
    QVERIFY(store.save(path));
    RecentStore loaded;
    QVERIFY(loaded.load(path));
    QVERIFY(loaded.printPreferences(QStringLiteral("/records/first.pdf")) == first);
    QVERIFY(loaded.printPreferences(QStringLiteral("/records/second.pdf")) == second);
}

// Reopening retains accepted choices, while removal and unknown paths never recreate history.
void TstRecentStore::printPreferencesSurviveReopenAndRemoval()
{
    mervin::printing::Preferences preferences;
    preferences.settings.scalePercent = 50;
    const QString path = QStringLiteral("/records/first.pdf");
    RecentStore store;
    QVERIFY(!store.printPreferences(path));
    QVERIFY(!store.setPrintPreferences(path, preferences));
    QVERIFY(!store.setPrintPreferences(QString(), preferences));
    QVERIFY(!store.printPreferences(QString()));
    QCOMPARE(store.count(), 0);
    QVERIFY(store.add(path, 100, 12));
    QVERIFY(store.setFavorite(path, true));
    QVERIFY(store.setPrintPreferences(path, preferences));
    const QByteArray unchanged = store.toJson();
    QVERIFY(!store.setPrintPreferences(path, preferences));
    QCOMPARE(store.toJson(), unchanged);
    QVERIFY(store.add(QStringLiteral("/records/second.pdf"), 200));
    QVERIFY(store.add(path, 300));
    QVERIFY(store.printPreferences(path) == preferences);
    QCOMPARE(store.entries().first().pageCount, 12);
    QCOMPARE(store.entries().first().lastOpened, qint64(300));
    QVERIFY(store.entries().first().favorite);
    QVERIFY(store.remove(path));
    QVERIFY(!store.printPreferences(path));
    QVERIFY(!store.setPrintPreferences(path, preferences));
    QCOMPARE(store.count(), 1);
    QVERIFY(store.add(path, 400));
    QVERIFY(!store.printPreferences(path));
}

// Invalid optional values cannot create excessive copies or replace valid unrelated choices.
void TstRecentStore::printPreferencesRejectMalformedFields()
{
    using namespace mervin::printing;
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("recent.json"));
    const QByteArray literal = R"([
        {"p":"/old.pdf","t":1},
        {"p":"/wrong-type.pdf","t":2,"print":42},
        {"p":"/mixed.pdf","t":3,"print":{
            "printerName":12,"printToFile":"true","orientation":"landscape",
            "pageSelection":"range","fromPage":4.5,"toPage":-1,"customPages":false,
            "copies":1000000,"scaleMode":"custom","scalePercent":401,
            "alignment":"diagonal","colourMode":"grayscale","qualityDpi":301,
            "twoSided":true,"paper":{"key":"huge","widthPoints":1e100,"heightPoints":842}
        }}])";
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    QCOMPARE(file.write(literal), literal.size());
    file.close();
    RecentStore store;
    QVERIFY(store.load(path));
    QVERIFY(!store.printPreferences(QStringLiteral("/old.pdf")));
    QVERIFY(!store.printPreferences(QStringLiteral("/wrong-type.pdf")));
    const auto parsed = store.printPreferences(QStringLiteral("/mixed.pdf"));
    QVERIFY(parsed);
    Preferences expected;
    expected.orientation = QPageLayout::Landscape;
    expected.pageSelection = PageSelection::Range;
    expected.settings.scaleMode = ScaleMode::Custom;
    expected.settings.colourMode = ColourMode::Grayscale;
    expected.duplex = DuplexMode::LongSide;
    QVERIFY(*parsed == expected);

    const Preferences bounded = Preferences::fromJson(QJsonObject{
        {QStringLiteral("printerName"), QString(4097, QLatin1Char('x'))},
        {QStringLiteral("customPages"), QString(32768, QLatin1Char('1'))},
        {QStringLiteral("copies"), 2.5},
        {QStringLiteral("fromPage"), 9},
        {QStringLiteral("toPage"), 3},
        {QStringLiteral("scaleMode"), QStringLiteral("future-mode")},
    });
    QVERIFY(bounded.printerName.isEmpty());
    QVERIFY(bounded.customPages.isEmpty());
    QCOMPARE(bounded.copies, 1);
    QCOMPARE(bounded.fromPage, 9);
    QCOMPARE(bounded.toPage, 9);
    QCOMPARE(bounded.settings.scaleMode, ScaleMode::FitToPage);

    for (const DuplexMode mode : {DuplexMode::None, DuplexMode::Auto,
                                  DuplexMode::LongSide, DuplexMode::ShortSide}) {
        Preferences duplex;
        duplex.duplex = mode;
        QCOMPARE(Preferences::fromJson(duplex.toJson()).duplex, mode);
    }
    QCOMPARE(Preferences::fromJson(QJsonObject{{QStringLiteral("duplex"), "invalid"}}).duplex,
             DuplexMode::None);
}

QTEST_GUILESS_MAIN(TstRecentStore)
#include "tst_recent_store.moc"
