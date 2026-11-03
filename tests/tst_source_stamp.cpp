#include "io/SourceStamp.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QTest>

#ifdef Q_OS_WIN
#include <qt_windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

using mervin::SourceStamp;
using mervin::sourceAncestorVolume;
using mervin::sourceStamp;

/// Checks native identity and distinguishes absent files from unavailable metadata.
class TstSourceStamp : public QObject
{
    Q_OBJECT

private slots:
    /// A replacement can keep the old size and modification time but has a new identity.
    void replacementHasAnotherIdentity()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path = directory.filePath(QStringLiteral("source.pdf"));
        const QString replacement = directory.filePath(QStringLiteral("replacement.pdf"));
        for (const QString &name : {path, replacement}) {
            QFile file(name);
            QVERIFY(file.open(QIODevice::WriteOnly));
            QCOMPARE(file.write("test"), 4);
        }
        const SourceStamp before = sourceStamp(path);
        QVERIFY(before.exists);
        QVERIFY(!before.missing);
        QVERIFY(before.identity.has_value());
        QCOMPARE(before.size, 4);
        QCOMPARE(before.modified, QFileInfo(path).lastModified().toMSecsSinceEpoch());
        QCOMPARE(before, sourceStamp(path));

        QFile file(replacement);
        QVERIFY(file.open(QIODevice::ReadWrite));
        QVERIFY(file.setFileTime(QDateTime::fromMSecsSinceEpoch(before.modified),
                                 QFileDevice::FileModificationTime));
        file.close();
        QVERIFY(QFile::remove(path));
        QVERIFY(QFile::rename(replacement, path));
        const SourceStamp after = sourceStamp(path);
        QVERIFY(after.exists);
        QVERIFY(after.identity.has_value());
        QCOMPARE(after.size, before.size);
        QCOMPARE(after.modified, before.modified);
        QCOMPARE(after.identity->volume, before.identity->volume);
        QVERIFY(after.identity->file != before.identity->file);
        QVERIFY(after != before);
    }

    /// Missing folders keep the volume of their nearest surviving ancestor.
    void missingFolderKeepsItsVolume()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString folder = directory.filePath(QStringLiteral("removed"));
        const QString path = QDir(folder).filePath(QStringLiteral("source.pdf"));
        QVERIFY(QDir().mkpath(folder));
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.close();
        const SourceStamp before = sourceStamp(path);
        QVERIFY(before.identity.has_value());
        QVERIFY(QFile::remove(path));
        QVERIFY(QDir().rmdir(folder));
        const SourceStamp after = sourceStamp(path);
        QVERIFY(!after.exists);
        QVERIFY(after.missing);
        QVERIFY(!after.identity.has_value());
        QCOMPARE(sourceAncestorVolume(path), std::optional<quint64>(before.identity->volume));
    }

    /// An inaccessible lookup must never authorize removal or fall back above its error.
    void unavailableIsNotMissing()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
#ifdef Q_OS_WIN
        const QString path = directory.filePath(QStringLiteral("invalid|name.pdf"));
        const SourceStamp stamp = sourceStamp(path);
        QVERIFY(!stamp.exists);
        QVERIFY(!stamp.missing);
#else
        const QString loop = directory.filePath(QStringLiteral("loop"));
        const QByteArray native = QFile::encodeName(loop);
        QCOMPARE(::symlink(native.constData(), native.constData()), 0);
        const QString path = QDir(loop).filePath(QStringLiteral("source.pdf"));
        const SourceStamp stamp = sourceStamp(path);
        QVERIFY(!stamp.exists);
        QVERIFY(!stamp.missing);
        QVERIFY(!sourceAncestorVolume(path).has_value());
#endif
    }

    /// Native timestamps distinguish writes that fall within one millisecond.
    void timestampsKeepSubMillisecondPrecision()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path = directory.filePath(QStringLiteral("source.pdf"));
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        QCOMPARE(file.write("test"), 4);
        file.close();
        const auto setModified = [&path](int nanoseconds) {
#ifdef Q_OS_WIN
            const QString native = QDir::toNativeSeparators(path);
            const HANDLE handle = CreateFileW(reinterpret_cast<LPCWSTR>(native.utf16()),
                                               FILE_WRITE_ATTRIBUTES,
                                               FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                               nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (handle == INVALID_HANDLE_VALUE)
                return false;
            const quint64 ticks = (1700000000ULL + 11644473600ULL) * 10000000 + nanoseconds / 100;
            const FILETIME modified{DWORD(ticks), DWORD(ticks >> 32)};
            const bool changed = SetFileTime(handle, nullptr, nullptr, &modified);
            CloseHandle(handle);
            return changed;
#else
            const QByteArray native = QFile::encodeName(path);
            const struct timespec times[2]{{1700000000, 100}, {1700000000, nanoseconds}};
            return ::utimensat(AT_FDCWD, native.constData(), times, 0) == 0;
#endif
        };
        QVERIFY(setModified(100));
        const SourceStamp before = sourceStamp(path);
        QVERIFY(setModified(200));
        const SourceStamp after = sourceStamp(path);
        QVERIFY2(before.modifiedSubMs != after.modifiedSubMs,
                 "Use a temporary filesystem that retains submillisecond timestamps.");
        QCOMPARE(before.identity, after.identity);
        QCOMPARE(before.size, after.size);
        QCOMPARE(before.modified, after.modified);
        QCOMPARE(before.modifiedSubMs, std::optional<int>(100));
        QCOMPARE(after.modifiedSubMs, std::optional<int>(200));
        QVERIFY(before != after);
    }

    /// Old manifests lack native identity, while new identities detect matching metadata.
    void recoveryMetadataRemainsComparable()
    {
        SourceStamp legacy;
        legacy.exists = true;
        legacy.size = 42;
        legacy.modified = 1000;
        legacy.changed = 1000;
        SourceStamp current = legacy;
        current.identity = mervin::FileIdentity{1, 2};
        current.modifiedSubMs = 100;
        current.changedSubMs = 100;
        QCOMPARE(legacy, current);
        SourceStamp replacement = current;
        replacement.identity = mervin::FileIdentity{1, 3};
        QVERIFY(current != replacement);
        replacement.identity = mervin::FileIdentity{2, 2};
        QVERIFY(current != replacement);
        replacement = current;
        replacement.modifiedSubMs = 200;
        QVERIFY(current != replacement);
        replacement = current;
        replacement.changedSubMs = 200;
        QVERIFY(current != replacement);
        current.modified = 1001;
        QVERIFY(legacy != current);
    }
};

QTEST_GUILESS_MAIN(TstSourceStamp)
#include "tst_source_stamp.moc"
