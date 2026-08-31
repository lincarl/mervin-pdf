#include "recent/RecentStore.h"

#include <QFile>
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
#ifdef Q_OS_WIN
    void dedupIsCaseInsensitiveOnWindows();
#endif
    void retentionTrimsOldest();
    void loweringRetentionRetrims();
    void removeDeletesEntry();
    void removeMissingFilesDeletesAbsentEntries();
    void removeMissingFilesOnlyRemovesRequestedEntries();
    void emptyPathIgnored();
    void jsonRoundTrip();
    void loadMissingFileLeavesEmpty();
};

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

void TstRecentStore::removeMissingFilesDeletesAbsentEntries()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString existing = dir.filePath(QStringLiteral("existing.pdf"));
    QFile file(existing);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.close();

    const QString missing = dir.filePath(QStringLiteral("missing.pdf"));

    RecentStore s;
    s.add(existing, 100);
    s.add(missing, 200);

    QCOMPARE(s.removeMissingFiles({existing, missing}), QStringList{missing});
    QCOMPARE(s.count(), 1);
    QCOMPARE(s.entries()[0].path, existing);
    QVERIFY(s.removeMissingFiles({missing}).isEmpty());
}

void TstRecentStore::removeMissingFilesOnlyRemovesRequestedEntries()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString missingA = dir.filePath(QStringLiteral("missing-a.pdf"));
    const QString missingB = dir.filePath(QStringLiteral("missing-b.pdf"));

    RecentStore s;
    s.add(missingA, 100);
    s.add(missingB, 200);

    QCOMPARE(s.removeMissingFiles({missingA}), QStringList{missingA});
    QCOMPARE(s.count(), 1);
    QCOMPARE(s.entries()[0].path, missingB);

    QCOMPARE(s.removeMissingFiles({missingB}), QStringList{missingB});
    QCOMPARE(s.count(), 0);
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
    a.add(QStringLiteral("C:/b.pdf"), 222);
    QVERIFY(a.save(file));

    RecentStore b;
    QVERIFY(b.load(file));
    QCOMPARE(b.count(), 2);
    QCOMPARE(b.entries()[0].path, QStringLiteral("C:/b.pdf"));
    QCOMPARE(b.entries()[0].lastOpened, qint64(222));
    QCOMPARE(b.entries()[1].path, QStringLiteral("C:/a.pdf"));
}

void TstRecentStore::loadMissingFileLeavesEmpty()
{
    RecentStore s;
    s.add(QStringLiteral("C:/a.pdf"), 100);
    QVERIFY(!s.load(QStringLiteral("C:/no/such/file-mervin-test.json")));
    QCOMPARE(s.count(), 0); // load clears even on failure - a safe empty default
}

QTEST_GUILESS_MAIN(TstRecentStore)
#include "tst_recent_store.moc"
