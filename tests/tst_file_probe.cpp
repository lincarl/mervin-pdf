// FileProbe checks files on background threads for the UI thread. A share that
// stops answering cannot be made here, so these cover the answers and the
// bookkeeping; a check that stalls is covered by hand (see the pull request).
#include "io/FileProbe.h"

#include <QDir>
#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

using mervin::FileCheck;
using mervin::FileFacts;
using mervin::FileProbe;

namespace {

bool write(const QString &path, const QByteArray &data)
{
    QFile f(path);
    return f.open(QIODevice::WriteOnly) && f.write(data) == data.size();
}

// Paths answered so far, in order.
QStringList answered(const QSignalSpy &spy)
{
    QStringList paths;
    for (const QList<QVariant> &args : spy)
        paths << args.at(0).value<FileCheck>().path;
    return paths;
}

// A file checked without a recorded mount.
FileCheck plain(const QString &path)
{
    return {path, {}};
}

} // namespace

class TstFileProbe : public QObject
{
    Q_OBJECT

private slots:
    void answersWhatIsOnDisk();
    void fileBeingCheckedIsNotQueuedTwice();
    void freshAnswerIsAnnouncedAgain();
    void volumeThatIsAwayIsNotRemoved();
    void everyFileIsCheckedOnAVolumeThatIsAway();
    void theRecordedMountMakesAnotherCheck();
};

void TstFileProbe::answersWhatIsOnDisk()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString present = dir.filePath(QStringLiteral("present.pdf"));
    const QString deleted = dir.filePath(QStringLiteral("deleted.pdf"));
    const QString inGoneFolder = dir.filePath(QStringLiteral("gone/plan.pdf"));
    QVERIFY(write(present, "12345"));

    FileProbe probe;
    QSignalSpy spy(&probe, &FileProbe::checked);
    probe.check({plain(present), plain(deleted), plain(inGoneFolder)});
    QVERIFY(probe.isChecking(plain(present)));
    QTRY_COMPARE(spy.size(), 3);
    QVERIFY(!probe.isChecking(plain(present)));

    const FileFacts there = probe.facts(plain(present)).value();
    QVERIFY(there.exists);
    QCOMPARE(there.size, 5);
#ifndef Q_OS_WIN
    QVERIFY(!there.mount.isEmpty());
#endif
    // Gone from a folder that is still there, so deleted, moved or renamed.
    const FileFacts gone = probe.facts(plain(deleted)).value();
    QVERIFY(!gone.exists);
    QVERIFY(gone.removed());
    QVERIFY(gone.goneFromFolder());
    // Its folder went too, which can also be another volume in its place.
    const FileFacts folderGone = probe.facts(plain(inGoneFolder)).value();
    QVERIFY(folderGone.removed());
    QVERIFY(!folderGone.goneFromFolder());
    QVERIFY(!probe.facts(plain(dir.filePath(QStringLiteral("never-asked.pdf")))).has_value());
}

void TstFileProbe::fileBeingCheckedIsNotQueuedTwice()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString a = dir.filePath(QStringLiteral("a.pdf"));
    const QString b = dir.filePath(QStringLiteral("b.pdf"));
    QVERIFY(write(a, "a"));

    FileProbe probe;
    QSignalSpy spy(&probe, &FileProbe::checked);
    // Another spelling of the same file is the same file.
    probe.check({plain(a), plain(dir.path() + QStringLiteral("/./a.pdf"))});
    probe.check({plain(a), plain(b)});
    QTRY_COMPARE(spy.size(), 2);
    QTest::qWait(50);
    QCOMPARE(answered(spy), (QStringList{a, b}));
}

void TstFileProbe::freshAnswerIsAnnouncedAgain()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString file = dir.filePath(QStringLiteral("plan.pdf"));
    QVERIFY(write(file, "plan"));

    FileProbe probe;
    QSignalSpy spy(&probe, &FileProbe::checked);
    probe.check({plain(file)});
    QTRY_COMPARE(spy.size(), 1);
    QVERIFY(QFile::remove(file));

    // Answered moments ago, so the same answer comes again, later, without a check.
    probe.check({plain(file)}, 60000);
    QCOMPARE(spy.size(), 1);
    QVERIFY(!probe.isChecking(plain(file)));
    QTRY_COMPARE(spy.size(), 2);
    QVERIFY(spy.at(1).at(1).value<FileFacts>().exists);

    // Asked for afresh, it is checked again.
    probe.check({plain(file)});
    QTRY_COMPARE(spy.size(), 3);
    QVERIFY(!probe.facts(plain(file))->exists);
}

void TstFileProbe::volumeThatIsAwayIsNotRemoved()
{
    // A drive or share that is not there. Its files are missing but not removed,
    // so neither Recent nor the session forgets them.
#ifdef Q_OS_WIN
    const QString share = QStringLiteral("//mervin-test-no-such-server/share/");
#else
    const QString share = QStringLiteral("/mnt/mervin-test-absent/");
#endif
    FileProbe probe;
    QSignalSpy spy(&probe, &FileProbe::checked);
    probe.check({plain(share + QStringLiteral("a.pdf")), plain(share + QStringLiteral("b.pdf"))});
    // Checking the share that is away can take a while on Windows.
    QTRY_COMPARE_WITH_TIMEOUT(spy.size(), 2, 30000);
    for (const QString &name : {QStringLiteral("a.pdf"), QStringLiteral("b.pdf")}) {
        const FileFacts facts = probe.facts(plain(share + name)).value();
        QVERIFY(!facts.exists);
        QVERIFY(!facts.volumeAttached);
        QVERIFY(!facts.removed());
    }
}

void TstFileProbe::everyFileIsCheckedOnAVolumeThatIsAway()
{
#ifndef Q_OS_WIN
    // A share mounted by hand on share/, unmounted now, leaves a plain folder, so
    // its files' volume is away. A file that is there all the same, checked after
    // one that is not, is still found: only the volume check is shared.
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString share = dir.filePath(QStringLiteral("share"));
    QVERIFY(QDir().mkpath(share));
    const QString missing = share + QStringLiteral("/plan.pdf");
    const QString there = share + QStringLiteral("/there.pdf");
    QVERIFY(write(there, "there"));

    FileProbe probe;
    QSignalSpy spy(&probe, &FileProbe::checked);
    probe.check({{missing, share}, {there, share}});
    QTRY_COMPARE(spy.size(), 2);
    const FileFacts away = probe.facts({missing, share}).value();
    QVERIFY(!away.exists);
    QVERIFY(!away.volumeAttached);
    QVERIFY(probe.facts({there, share})->exists);
#endif
}

void TstFileProbe::theRecordedMountMakesAnotherCheck()
{
#ifndef Q_OS_WIN
    // The volume check depends on the mount recorded for the file, so the same
    // path with another mount gets an answer of its own.
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString share = dir.filePath(QStringLiteral("share"));
    QVERIFY(QDir().mkpath(share));
    const QString missing = share + QStringLiteral("/plan.pdf");

    FileProbe probe;
    QSignalSpy spy(&probe, &FileProbe::checked);
    probe.check({plain(missing), {missing, share}});
    QTRY_COMPARE(spy.size(), 2);
    QVERIFY(probe.facts(plain(missing))->removed());
    QVERIFY(!probe.facts({missing, share})->removed());
#endif
}

QTEST_GUILESS_MAIN(TstFileProbe)
#include "tst_file_probe.moc"
