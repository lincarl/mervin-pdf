// The app's own files are written on a background thread (io/AppFiles), and style
// sheet images are kept in memory (io/MemoryFiles), so the UI thread never touches
// the disk for them.
#include "io/AppFiles.h"
#include "io/MemoryFiles.h"

#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QTest>

namespace appfiles = mervin::appfiles;
namespace memoryfiles = mervin::memoryfiles;

namespace {

QByteArray read(const QString &path)
{
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray("<missing>");
}

} // namespace

class TstAppFiles : public QObject
{
    Q_OBJECT

private slots:
    void writesLandInTheOrderAsked();
    void writeCreatesItsFolder();
    void aWaitedWriteReportsItsResult();
    void publishedFilesReadFromMemory();
};

void TstAppFiles::writesLandInTheOrderAsked()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString a = dir.filePath(QStringLiteral("a.json"));
    const QString b = dir.filePath(QStringLiteral("b.json"));

    // The newest write of a file wins, a removal after a write removes it, and a
    // write after a removal brings it back.
    for (int i = 0; i < 50; ++i)
        appfiles::write(a, QByteArray::number(i));
    appfiles::write(b, "first");
    appfiles::remove(b);
    QVERIFY(appfiles::flush(10000));
    QCOMPARE(read(a), QByteArray("49"));
    QVERIFY(!QFile::exists(b));

    appfiles::remove(a);
    appfiles::write(a, "back");
    appfiles::write(b, "kept");
    QVERIFY(appfiles::flush(10000));
    QCOMPARE(read(a), QByteArray("back"));
    QCOMPARE(read(b), QByteArray("kept"));
    // Nothing waits, so a flush returns at once.
    QVERIFY(appfiles::flush(0));
}

void TstAppFiles::writeCreatesItsFolder()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString file = dir.filePath(QStringLiteral("profile/recovery/x.json"));
    appfiles::write(file, "{}");
    QVERIFY(appfiles::flush(10000));
    QCOMPARE(read(file), QByteArray("{}"));
}

void TstAppFiles::aWaitedWriteReportsItsResult()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString file = dir.filePath(QStringLiteral("config.toml"));
    QString error;
    QCOMPARE(appfiles::writeAndWait(file, "a = 1", 10000, &error), appfiles::Wait::Written);
    QCOMPARE(read(file), QByteArray("a = 1"));
    QVERIFY(error.isEmpty());

    // A folder where the file belongs cannot be replaced, and the reason comes back,
    // even when later writes have finished by the time the caller looks.
    const QString blocked = dir.filePath(QStringLiteral("blocked.toml"));
    QVERIFY(QDir().mkdir(blocked));
    appfiles::write(dir.filePath(QStringLiteral("before.json")), "{}");
    QCOMPARE(appfiles::writeAndWait(blocked, "b = 2", 10000, &error), appfiles::Wait::Failed);
    QVERIFY(!error.isEmpty());
    QCOMPARE(appfiles::writeAndWait(file, "a = 3", 10000), appfiles::Wait::Written);
    QCOMPARE(read(file), QByteArray("a = 3"));
}

void TstAppFiles::publishedFilesReadFromMemory()
{
    const QString first = memoryfiles::publish(
        QStringLiteral("tst-theme"), {{QStringLiteral("arrow.png"), QByteArray("one")},
                                      {QStringLiteral("arrow@2x.png"), QByteArray("two")},
                                      {QStringLiteral("cross.png"), QByteArray()}});
    QVERIFY(first.startsWith(QStringLiteral(":/tst-theme-")));
    QCOMPARE(read(first + QStringLiteral("/arrow.png")), QByteArray("one"));
    QCOMPARE(read(first + QStringLiteral("/arrow@2x.png")), QByteArray("two"));
    QCOMPARE(read(first + QStringLiteral("/cross.png")), QByteArray());
    QCOMPARE(QDir(first).entryList().size(), 3);
    QVERIFY(!QFile::exists(first + QStringLiteral("/missing.png")));

    // New contents get a new root, and the old one goes.
    const QString second = memoryfiles::publish(
        QStringLiteral("tst-theme"), {{QStringLiteral("arrow.png"), QByteArray("three")}});
    QVERIFY(second != first);
    QCOMPARE(read(second + QStringLiteral("/arrow.png")), QByteArray("three"));
    QVERIFY(!QFile::exists(first + QStringLiteral("/arrow.png")));
}

QTEST_GUILESS_MAIN(TstAppFiles)
#include "tst_app_files.moc"
