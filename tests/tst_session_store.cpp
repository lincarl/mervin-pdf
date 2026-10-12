#include "session/SessionStore.h"

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QTest>

#include <functional>

using mervin::SessionStore;

namespace {

const QDate kToday(2026, 10, 12);

// An empty file standing in for a PDF; prepareRestore only checks that it exists.
bool touch(const QString &path)
{
    QFile f(path);
    return f.open(QIODevice::WriteOnly);
}

// A volume check for prepareRestore that treats `dir`/away as a detached drive and
// everything else as attached.
std::function<bool(const QString &)> awayIsDetached(const QTemporaryDir &dir)
{
    const QString away = dir.filePath(QStringLiteral("away")) + QLatin1Char('/');
    return [away](const QString &path) { return !path.startsWith(away); };
}

} // namespace

class TstSessionStore : public QObject
{
    Q_OBJECT

private slots:
    void roundTrip();
    void loadMissingIsEmpty();
    void emptyClears();
    void activePathRoundTrip();
    void activePathAbsentFromOlderFile();
    void activePathOutsideOpenSetIsDropped();
    void clearDropsActivePath();
    void removedFileIsDropped();
    void fileOnDetachedVolumeIsKept();
    void unmountedMountPointIsDetached();
    void keptFileExpires();
    void keptFileFoundAgainIsRestored();
    void keptFileOpenAgainLeavesTheList();
    void unavailableRoundTrip();
};

void TstSessionStore::roundTrip()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString file = dir.filePath(QStringLiteral("session.json"));

    SessionStore a;
    a.setPaths({QStringLiteral("C:/a.pdf"), QStringLiteral("C:/docs/b.pdf")});
    QVERIFY(a.save(file));

    SessionStore b;
    QVERIFY(b.load(file));
    QCOMPARE(b.paths().size(), 2);
    QCOMPARE(b.paths()[0], QStringLiteral("C:/a.pdf"));
    QCOMPARE(b.paths()[1], QStringLiteral("C:/docs/b.pdf"));
}

void TstSessionStore::loadMissingIsEmpty()
{
    SessionStore s;
    s.setPaths({QStringLiteral("C:/x.pdf")});
    QVERIFY(!s.load(QStringLiteral("C:/no/such/mervin-session-test.json")));
    QVERIFY(s.paths().isEmpty()); // load clears even on failure
}

void TstSessionStore::emptyClears()
{
    QTemporaryDir dir;
    const QString file = dir.filePath(QStringLiteral("session.json"));
    SessionStore a;
    a.setPaths({QStringLiteral("C:/a.pdf")});
    QVERIFY(a.save(file));
    a.clear();
    QVERIFY(a.save(file));

    SessionStore b;
    QVERIFY(b.load(file));
    QVERIFY(b.paths().isEmpty());
}

void TstSessionStore::activePathRoundTrip()
{
    // The document on screen is what restore opens first, so it has to survive the
    // round trip - otherwise every start reopens the wrong document first.
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString file = dir.filePath(QStringLiteral("session.json"));

    SessionStore a;
    a.setPaths({QStringLiteral("C:/a.pdf"), QStringLiteral("C:/docs/b.pdf")});
    a.setActivePath(QStringLiteral("C:/docs/b.pdf"));
    QVERIFY(a.save(file));

    SessionStore b;
    QVERIFY(b.load(file));
    QCOMPARE(b.paths().size(), 2);
    QCOMPARE(b.activePath(), QStringLiteral("C:/docs/b.pdf"));
}

void TstSessionStore::activePathAbsentFromOlderFile()
{
    // A session.json written before the field existed must still load, with no
    // active document rather than a refusal or a bogus one.
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString file = dir.filePath(QStringLiteral("session.json"));
    QFile f(file);
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write("{\"open\":[\"C:/a.pdf\",\"C:/b.pdf\"]}");
    f.close();

    SessionStore s;
    QVERIFY(s.load(file));
    QCOMPARE(s.paths().size(), 2);
    QCOMPARE(s.paths()[0], QStringLiteral("C:/a.pdf"));
    QVERIFY(s.activePath().isEmpty());
}

void TstSessionStore::activePathOutsideOpenSetIsDropped()
{
    // A recorded active document that is not in the open set is stale (the entry
    // was closed, or the file is gone). Ignore it instead of planning a restore
    // around a document that will not be opened.
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString file = dir.filePath(QStringLiteral("session.json"));

    SessionStore a;
    a.setPaths({QStringLiteral("C:/a.pdf")});
    a.setActivePath(QStringLiteral("C:/gone.pdf"));
    QVERIFY(a.save(file));

    SessionStore b;
    QVERIFY(b.load(file));
    QCOMPARE(b.paths().size(), 1);
    QVERIFY(b.activePath().isEmpty());
}

void TstSessionStore::clearDropsActivePath()
{
    SessionStore s;
    s.setPaths({QStringLiteral("C:/a.pdf")});
    s.setActivePath(QStringLiteral("C:/a.pdf"));
    s.setUnavailable({{QStringLiteral("D:/b.pdf"), kToday}});
    s.clear();
    QVERIFY(s.paths().isEmpty());
    QVERIFY(s.activePath().isEmpty());
    QVERIFY(s.unavailable().isEmpty());
}

void TstSessionStore::removedFileIsDropped()
{
    // Gone from an attached disk, so it was deleted, moved or renamed, including
    // along with its folder.
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString present = dir.filePath(QStringLiteral("present.pdf"));
    const QString removed = dir.filePath(QStringLiteral("removed.pdf"));
    const QString folderGone = dir.filePath(QStringLiteral("gone/plan.pdf"));
    QVERIFY(touch(present));

    SessionStore s;
    s.setPaths({removed, folderGone, present});
    s.setActivePath(removed);
    QCOMPARE(s.prepareRestore(kToday), QStringList{present});
    QCOMPARE(s.paths(), QStringList{present});
    QVERIFY(s.unavailable().isEmpty());
    QVERIFY(s.activePath().isEmpty());
}

void TstSessionStore::fileOnDetachedVolumeIsKept()
{
    // A session file from before "unavailable" existed, with a document on an
    // unplugged drive whose mount point stayed behind as an empty folder. It is
    // kept, unopened, dated from this start, and is not the active document.
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    QVERIFY(QDir(dir.path()).mkpath(QStringLiteral("away")));
    const QString present = dir.filePath(QStringLiteral("present.pdf"));
    const QString away = dir.filePath(QStringLiteral("away/plan.pdf"));
    QVERIFY(touch(present));
    const QString file = dir.filePath(QStringLiteral("session.json"));
    QFile f(file);
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write(QJsonDocument(QJsonObject{{QStringLiteral("open"), QJsonArray{away, present}},
                                      {QStringLiteral("active"), away}})
                .toJson());
    f.close();

    SessionStore s;
    QVERIFY(s.load(file));
    QCOMPARE(s.paths(), (QStringList{away, present}));
    QCOMPARE(s.activePath(), away);
    QVERIFY(s.unavailable().isEmpty());

    QCOMPARE(s.prepareRestore(kToday, awayIsDetached(dir)), QStringList{present});
    QCOMPARE(s.unavailable().size(), 1);
    QCOMPARE(s.unavailable().first().path, away);
    QCOMPARE(s.unavailable().first().since, kToday);
    QVERIFY(s.activePath().isEmpty());
}

void TstSessionStore::unmountedMountPointIsDetached()
{
    // The real volume check: an offline share, or a mount point with nothing
    // mounted on it (often an empty folder), keeps its documents.
#ifdef Q_OS_WIN
    const QString away = QStringLiteral("//mervin-test-no-such-server/share/plan.pdf");
#else
    const QString away = QStringLiteral("/mnt/mervin-test-absent/plan.pdf");
#endif
    SessionStore s;
    s.setPaths({away});
    QVERIFY(s.prepareRestore(kToday).isEmpty());
    QCOMPARE(s.unavailable().size(), 1);
    QCOMPARE(s.unavailable().first().path, away);
}

void TstSessionStore::keptFileExpires()
{
    // Kept for 30 days from the start that first found it missing, then dropped
    // without a check, even if it is back. A future date counts from today.
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    QVERIFY(QDir(dir.path()).mkpath(QStringLiteral("back")));
    const QString recent = dir.filePath(QStringLiteral("away/recent.pdf"));
    const QString expired = dir.filePath(QStringLiteral("back/expired.pdf"));
    const QString future = dir.filePath(QStringLiteral("away/future.pdf"));
    QVERIFY(touch(expired));

    SessionStore s;
    s.setUnavailable({{recent, kToday.addDays(-29)},
                      {expired, kToday.addDays(-30)},
                      {future, kToday.addDays(5)}});
    QVERIFY(s.prepareRestore(kToday, awayIsDetached(dir)).isEmpty());
    QCOMPARE(s.unavailable().size(), 2);
    QCOMPARE(s.unavailable().at(0).path, recent);
    QCOMPARE(s.unavailable().at(0).since, kToday.addDays(-29));
    QCOMPARE(s.unavailable().at(1).path, future);
    QCOMPARE(s.unavailable().at(1).since, kToday);
}

void TstSessionStore::keptFileFoundAgainIsRestored()
{
    // The drive is back. Its file reopens after the open documents, and one that is
    // gone from the returned folder is dropped. Files still away stay kept.
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    QVERIFY(QDir(dir.path()).mkpath(QStringLiteral("back")));
    const QString present = dir.filePath(QStringLiteral("present.pdf"));
    const QString back = dir.filePath(QStringLiteral("back/plan.pdf"));
    const QString removed = dir.filePath(QStringLiteral("back/removed.pdf"));
    const QString away = dir.filePath(QStringLiteral("away/plan.pdf"));
    QVERIFY(touch(present));
    QVERIFY(touch(back));

    const QDate since = kToday.addDays(-3);
    SessionStore s;
    s.setPaths({present});
    s.setUnavailable({{back, since}, {removed, since}, {away, since}});
    QCOMPARE(s.prepareRestore(kToday, awayIsDetached(dir)), (QStringList{present, back}));
    QCOMPARE(s.unavailable().size(), 1);
    QCOMPARE(s.unavailable().first().path, away);
    QCOMPARE(s.unavailable().first().since, since);
}

void TstSessionStore::keptFileOpenAgainLeavesTheList()
{
    // Opened during the session, a kept document is an ordinary tab, so closing it
    // later drops it. The others stay kept.
    const QString reopened = QStringLiteral("D:/plans/a.pdf");
    const QString away = QStringLiteral("D:/plans/b.pdf");
    SessionStore s;
    s.setUnavailable({{reopened, kToday}, {away, kToday}});
    s.setPaths({QStringLiteral("C:/c.pdf"), reopened});
    QCOMPARE(s.unavailable().size(), 1);
    QCOMPARE(s.unavailable().first().path, away);
    s.setPaths({QStringLiteral("C:/c.pdf")});
    QCOMPARE(s.unavailable().size(), 1);
}

void TstSessionStore::unavailableRoundTrip()
{
    // Kept files go in their own array, so "open" and "active" read as before.
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString file = dir.filePath(QStringLiteral("session.json"));
    const QString open = QStringLiteral("C:/a.pdf");
    const QString away = QStringLiteral("//server/share/b.pdf");

    SessionStore a;
    a.setPaths({open});
    a.setActivePath(open);
    a.setUnavailable({{away, QDate(2026, 9, 20)}});
    QVERIFY(a.save(file));

    QFile f(file);
    QVERIFY(f.open(QIODevice::ReadOnly));
    const QJsonObject root = QJsonDocument::fromJson(f.readAll()).object();
    QCOMPARE(root.value(QStringLiteral("open")).toArray(), QJsonArray{open});
    QCOMPARE(root.value(QStringLiteral("active")).toString(), open);

    SessionStore b;
    QVERIFY(b.load(file));
    QCOMPARE(b.paths(), QStringList{open});
    QCOMPARE(b.activePath(), open);
    QCOMPARE(b.unavailable().size(), 1);
    QCOMPARE(b.unavailable().first().path, away);
    QCOMPARE(b.unavailable().first().since, QDate(2026, 9, 20));
}

QTEST_GUILESS_MAIN(TstSessionStore)
#include "tst_session_store.moc"
