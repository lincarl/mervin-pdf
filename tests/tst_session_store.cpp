#include "io/FileProbe.h"
#include "session/SessionStore.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QTest>

#include <functional>

using mervin::FileCheck;
using mervin::FileFacts;
using mervin::FileProbe;
using mervin::SessionStore;

namespace {

const QDate kToday(2026, 10, 12);

using Found = std::function<std::optional<FileFacts>(const QString &)>;

// An empty file standing in for a PDF; restore only checks that it exists.
bool touch(const QString &path)
{
    QFile f(path);
    return f.open(QIODevice::WriteOnly);
}

// A volume check that treats `dir`/away as a detached drive and everything else
// as attached.
std::function<bool(const QString &)> awayIsDetached(const QTemporaryDir &dir)
{
    const QString away = dir.filePath(QStringLiteral("away")) + QLatin1Char('/');
    return [away](const QString &path) { return !path.startsWith(away); };
}

// The answers prepareRestore gets for the files filesToCheck() lists, checked on
// this thread the way FileProbe checks them. `attached`, when given, stands in for
// the volume check. `recordedMount` is Recent's, as WindowManager passes it.
Found checked(SessionStore &s, const std::function<bool(const QString &)> &attached = {},
              const std::function<QString(const QString &)> &recordedMount = {})
{
    QHash<QString, FileFacts> answers;
    for (const FileCheck &file : s.filesToCheck(kToday, recordedMount)) {
        FileFacts facts = FileProbe::examine(file);
        if (!facts.exists && attached) {
            facts.volumeAttached = attached(file.path);
            facts.folderExists = facts.volumeAttached && QFileInfo(file.path).absoluteDir().exists();
        }
        answers.insert(file.path, facts);
    }
    return [answers](const QString &path) -> std::optional<FileFacts> {
        const auto it = answers.constFind(path);
        return it == answers.cend() ? std::nullopt : std::optional<FileFacts>(*it);
    };
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
    void fileInMissingFolderIsKept();
    void readableFileIsRestoredWhateverTheVolumeCheck();
    void fileWithoutAnAnswerIsKept();
    void checksListOpenThenKeptFiles();
    void unmountedMountPointIsDetached();
    void eachRecordedMountIsCheckedOnItsOwn();
    void sessionMountOutlivesRecent();
    void keptFileExpires();
    void keptFileFoundAgainIsRestored();
    void keptFileOpenAgainLeavesTheList();
    void unavailableRoundTrip();
    void mountsRoundTrip();
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
    // Gone from a folder that is still there on an attached disk, so it was
    // deleted, moved or renamed.
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString present = dir.filePath(QStringLiteral("present.pdf"));
    const QString removed = dir.filePath(QStringLiteral("removed.pdf"));
    QVERIFY(touch(present));

    SessionStore s;
    s.setPaths({removed, present});
    s.setActivePath(removed);
    QCOMPARE(s.prepareRestore(kToday, checked(s)), QStringList{present});
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

    QCOMPARE(s.prepareRestore(kToday, checked(s, awayIsDetached(dir))), QStringList{present});
    QCOMPARE(s.unavailable().size(), 1);
    QCOMPARE(s.unavailable().first().path, away);
    QCOMPARE(s.unavailable().first().since, kToday);
    QVERIFY(s.activePath().isEmpty());
}

void TstSessionStore::fileInMissingFolderIsKept()
{
    // The volume check says attached, but the folder is missing: it may be a share
    // mounted where the check does not look, such as in the home folder.
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString inMissingFolder = dir.filePath(QStringLiteral("nas/docs/plan.pdf"));
    SessionStore s;
    s.setPaths({inMissingFolder});
    QVERIFY(s.prepareRestore(kToday, checked(s, [](const QString &) { return true; })).isEmpty());
    QCOMPARE(s.unavailable().size(), 1);
    QCOMPARE(s.unavailable().first().path, inMissingFolder);
}

void TstSessionStore::readableFileIsRestoredWhateverTheVolumeCheck()
{
    // A file that is there reopens even where the volume check knows of no mounted
    // volume, as for a folder directly under /mnt on the system disk.
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString present = dir.filePath(QStringLiteral("present.pdf"));
    QVERIFY(touch(present));
    SessionStore s;
    s.setPaths({present});
    QCOMPARE(s.prepareRestore(kToday, checked(s, [](const QString &) { return false; })),
             QStringList{present});
    QVERIFY(s.unavailable().isEmpty());
}

void TstSessionStore::fileWithoutAnAnswerIsKept()
{
    // A share that stopped answering gives no answer in time. Its documents are
    // kept like those on an unplugged drive, the others restore as usual, and
    // none is dropped.
    const QString answered = QStringLiteral("C:/docs/a.pdf");
    const QString silent = QStringLiteral("//server/share/b.pdf");
    const QString keptSilent = QStringLiteral("//server/share/c.pdf");
    SessionStore s;
    s.setPaths({silent, answered});
    s.setActivePath(silent);
    s.setUnavailable({{keptSilent, kToday.addDays(-4)}});
    const Found found = [&](const QString &path) -> std::optional<FileFacts> {
        if (path != answered)
            return std::nullopt;
        FileFacts there;
        there.exists = true;
        return there;
    };
    QCOMPARE(s.prepareRestore(kToday, found), QStringList{answered});
    QCOMPARE(s.unavailable().size(), 2);
    QCOMPARE(s.unavailable().at(0).path, silent);
    QCOMPARE(s.unavailable().at(0).since, kToday);
    QCOMPARE(s.unavailable().at(1).path, keptSilent);
    QCOMPARE(s.unavailable().at(1).since, kToday.addDays(-4));
    QVERIFY(s.activePath().isEmpty());
}

void TstSessionStore::checksListOpenThenKeptFiles()
{
    // The open documents first, then kept ones that have not expired, each with
    // the mount it is judged by, the session's or else Recent's, which the
    // session keeps from then on.
    const QString open = QStringLiteral("/home/ann/nas/a.pdf");
    const QString kept = QStringLiteral("/home/ann/nas/b.pdf");
    const QString expired = QStringLiteral("/home/ann/nas/c.pdf");
    const QString nas = QStringLiteral("/home/ann/nas");
    SessionStore s;
    s.setMount(open, nas);
    s.setUnavailable({{kept, kToday.addDays(-29)}, {expired, kToday.addDays(-30)}});
    s.setPaths({open});
    const QList<FileCheck> files = s.filesToCheck(kToday, [&](const QString &path) {
        return path == kept ? nas : QString();
    });
    QCOMPARE(files.size(), 2);
    QCOMPARE(files.at(0).path, open);
    QCOMPARE(files.at(0).mount, nas);
    QCOMPARE(files.at(1).path, kept);
    QCOMPARE(files.at(1).mount, nas);
    QCOMPARE(s.mount(kept), nas);
}

// Two documents in one folder, one with a recorded mount that no longer holds the
// folder, as a share mounted by hand leaves it. The document opened from the share
// is kept and the other one is dropped, in either order.
void TstSessionStore::eachRecordedMountIsCheckedOnItsOwn()
{
#ifndef Q_OS_WIN
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString share = dir.filePath(QStringLiteral("share"));
    QVERIFY(QDir().mkpath(share));
    const QString fromShare = share + QStringLiteral("/plan.pdf");
    const QString local = share + QStringLiteral("/deleted.pdf");
    const auto recordedMount = [&](const QString &path) {
        return path == fromShare ? share : QString();
    };
    for (const QStringList &order : {QStringList{local, fromShare}, QStringList{fromShare, local}}) {
        SessionStore s;
        s.setPaths(order);
        QVERIFY(s.prepareRestore(kToday, checked(s, {}, recordedMount)).isEmpty());
        QCOMPARE(s.unavailable().size(), 1);
        QCOMPARE(s.unavailable().first().path, fromShare);
    }
#endif
}

// The session keeps each document's mount, so a document from a share mounted by
// hand stays start after start once Recent no longer lists it. A mount Recent gave
// for a session saved before sessions recorded mounts is kept from then on. A
// missing document in the same folder with no mount is dropped.
void TstSessionStore::sessionMountOutlivesRecent()
{
#ifndef Q_OS_WIN
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString file = dir.filePath(QStringLiteral("session.json"));
    const QString share = dir.filePath(QStringLiteral("share"));
    QVERIFY(QDir().mkpath(share));
    const QString fromShare = share + QStringLiteral("/plan.pdf");
    const QString older = share + QStringLiteral("/older.pdf");
    const QString deleted = share + QStringLiteral("/deleted.pdf");

    SessionStore saved;
    saved.setMount(fromShare, share);
    saved.setPaths({fromShare, older, deleted});
    QVERIFY(saved.save(file));
    for (int start = 1; start <= 2; ++start) {
        // Recent lists only the older document, and forgets it after the first start.
        const auto recordedMount = [&](const QString &path) {
            return start == 1 && path == older ? share : QString();
        };
        SessionStore s;
        QVERIFY(s.load(file));
        QVERIFY(s.prepareRestore(kToday, checked(s, {}, recordedMount)).isEmpty());
        QCOMPARE(s.unavailable().size(), 2);
        QCOMPARE(s.unavailable().at(0).path, fromShare);
        QCOMPARE(s.unavailable().at(1).path, older);
        QCOMPARE(s.mount(older), share);
        QVERIFY(s.save(file));
    }
#endif
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
    QVERIFY(s.prepareRestore(kToday, checked(s)).isEmpty());
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
    QVERIFY(s.prepareRestore(kToday, checked(s, awayIsDetached(dir))).isEmpty());
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
    QCOMPARE(s.prepareRestore(kToday, checked(s, awayIsDetached(dir))), (QStringList{present, back}));
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

// The mounts of open and kept documents are saved. A closed document's is not.
void TstSessionStore::mountsRoundTrip()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString file = dir.filePath(QStringLiteral("session.json"));
    const QString nas = QStringLiteral("/home/ann/nas");
    const QString open = nas + QStringLiteral("/a.pdf");
    const QString away = nas + QStringLiteral("/b.pdf");
    const QString closed = nas + QStringLiteral("/c.pdf");

    SessionStore a;
    for (const QString &path : {open, away, closed})
        a.setMount(path, nas);
    a.setUnavailable({{away, kToday}});
    a.setPaths({open});
    QVERIFY(a.save(file));

    SessionStore b;
    QVERIFY(b.load(file));
    QCOMPARE(b.mount(open), nas);
    QCOMPARE(b.mount(away), nas);
    QVERIFY(b.mount(closed).isEmpty());
}

QTEST_GUILESS_MAIN(TstSessionStore)
#include "tst_session_store.moc"
