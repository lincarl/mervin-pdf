// The app's own files are written on a background thread (io/AppFiles), and style
// sheet images are kept in memory (io/MemoryFiles), so the UI thread never touches
// the disk for them.
#include "io/AppFiles.h"
#include "io/MemoryFiles.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QTest>

#include <mutex>

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
    void queuedJobsKeepTheirOrder();
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

// Jobs held back in the queue, to see which run and in what order.
void TstAppFiles::queuedJobsKeepTheirOrder()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString a = dir.filePath(QStringLiteral("a.json"));
    const QString b = dir.filePath(QStringLiteral("b.json"));
    std::mutex seenMutex;
    QStringList seen; // what each job left on disk, as it finished
    const auto release = qScopeGuard([] {
        appfiles::testing::hold(false);
        appfiles::testing::observe({});
    });
    appfiles::testing::observe([&](const QString &path, bool removed) {
        const QString name = QFileInfo(path).fileName();
        const QString result = removed ? name + QStringLiteral(" removed")
                                       : name + QLatin1Char('=') + QString::fromUtf8(read(path));
        std::lock_guard<std::mutex> lock(seenMutex);
        seen << result;
    });

    // A newer write of a drops the older one and queues after b's removal, which
    // was asked for between them.
    appfiles::testing::hold(true);
    appfiles::write(a, "1");
    appfiles::remove(b);
    appfiles::write(a, "2");
    appfiles::testing::hold(false);
    QVERIFY(appfiles::flush(10000));
    QCOMPARE(seen, QStringList({QStringLiteral("b.json removed"), QStringLiteral("a.json=2")}));

    // A write someone waits for is never dropped, so its result is its own.
    seen.clear();
    appfiles::testing::hold(true);
    QCOMPARE(appfiles::writeAndWait(a, "3", 0), appfiles::Wait::Pending);
    appfiles::write(a, "4");
    appfiles::write(a, "5");
    appfiles::testing::hold(false);
    QVERIFY(appfiles::flush(10000));
    QCOMPARE(seen, QStringList({QStringLiteral("a.json=3"), QStringLiteral("a.json=5")}));
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

    // A folder where the file belongs cannot be replaced, and the reason comes back.
    // A write asked for before it finishing first does not answer for it.
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
