#include "platform/FileClipboard.h"

#include <QClipboard>
#include <QDir>
#include <QFile>
#include <QGuiApplication>
#include <QMimeData>
#include <QTemporaryDir>
#include <QTest>
#include <QUrl>

#ifdef Q_OS_WIN
#include <windows.h>

#include <shellapi.h>
#endif

using mervin::copyFileToClipboard;

#ifdef Q_OS_WIN
// Clipboard-history/RDP listeners briefly hold the clipboard open right after
// every change, so a single OpenClipboard attempt is a coin toss; retry, and
// close via RAII so a failed assertion cannot leak the open clipboard.
struct ScopedClipboard
{
    bool opened = false;
    ScopedClipboard()
    {
        for (int i = 0; i < 50 && !opened; ++i) {
            opened = OpenClipboard(nullptr) != 0;
            if (!opened)
                QTest::qSleep(10);
        }
    }
    ~ScopedClipboard()
    {
        if (opened)
            CloseClipboard();
    }
};
#endif

class TstFileClipboard : public QObject
{
    Q_OBJECT

private slots:
    void missingFileLeavesClipboardAlone();
    void directoryRejected();
    void copyPlacesFileUrlOnClipboard();
#ifdef Q_OS_WIN
    void win32ClipboardHoldsExplorerStyleCopy();
#endif
    void cleanupTestCase();

private:
    QString makeFile(const QString &name);

    QTemporaryDir dir_;
};

QString TstFileClipboard::makeFile(const QString &name)
{
    const QString path = dir_.filePath(name);
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly))
        return {};
    f.write("%PDF-1.4 test payload");
    return path;
}

void TstFileClipboard::missingFileLeavesClipboardAlone()
{
    QVERIFY(dir_.isValid());
    const QString sentinel = QStringLiteral("mervin-clipboard-sentinel");
    QGuiApplication::clipboard()->setText(sentinel);

    QVERIFY(!copyFileToClipboard(dir_.filePath(QStringLiteral("does-not-exist.pdf"))));
    QVERIFY(!copyFileToClipboard(QString()));
    QCOMPARE(QGuiApplication::clipboard()->text(), sentinel);
}

void TstFileClipboard::directoryRejected()
{
    QVERIFY(!copyFileToClipboard(dir_.path()));
}

void TstFileClipboard::copyPlacesFileUrlOnClipboard()
{
    const QString path = makeFile(QStringLiteral("copied.pdf"));
    QVERIFY(!path.isEmpty());

    QVERIFY(copyFileToClipboard(path));

    const QMimeData *mime = QGuiApplication::clipboard()->mimeData();
    QVERIFY(mime != nullptr);
    QVERIFY(mime->hasUrls());
    QCOMPARE(mime->urls().size(), 1);
    QCOMPARE(mime->urls().first(), QUrl::fromLocalFile(path));
}

#ifdef Q_OS_WIN
// The Qt-level check above only proves round-tripping through Qt. Explorer and
// mail clients read the raw Win32 clipboard, so verify CF_HDROP really carries
// the file and the transfer is marked as a copy, not a cut.
void TstFileClipboard::win32ClipboardHoldsExplorerStyleCopy()
{
    const QString path = makeFile(QStringLiteral("explorer.pdf"));
    QVERIFY(!path.isEmpty());
    QVERIFY(copyFileToClipboard(path));

    QVERIFY(IsClipboardFormatAvailable(CF_HDROP));
    const UINT cfDropEffect = RegisterClipboardFormatW(L"Preferred DropEffect");
    QVERIFY(cfDropEffect != 0);
    QVERIFY(IsClipboardFormatAvailable(cfDropEffect));

    ScopedClipboard clip;
    QVERIFY(clip.opened);

    HANDLE hDrop = GetClipboardData(CF_HDROP);
    QVERIFY(hDrop != nullptr);
    wchar_t buf[MAX_PATH] = {};
    QCOMPARE(DragQueryFileW(static_cast<HDROP>(hDrop), 0xFFFFFFFF, nullptr, 0), 1u);
    QVERIFY(DragQueryFileW(static_cast<HDROP>(hDrop), 0, buf, MAX_PATH) > 0);
    QCOMPARE(QString::fromWCharArray(buf), QDir::toNativeSeparators(path));

    HANDLE hEffect = GetClipboardData(cfDropEffect);
    QVERIFY(hEffect != nullptr);
    const DWORD *effect = static_cast<const DWORD *>(GlobalLock(hEffect));
    QVERIFY(effect != nullptr);
    const DWORD value = *effect;
    GlobalUnlock(hEffect);

    QVERIFY2((value & 0x1) != 0, "DROPEFFECT_COPY bit missing"); // 0x1 = DROPEFFECT_COPY
}
#endif

void TstFileClipboard::cleanupTestCase()
{
    // The copied temp files vanish with dir_; do not leave the clipboard
    // pointing at them.
    QGuiApplication::clipboard()->clear();
}

QTEST_MAIN(TstFileClipboard)
#include "tst_file_clipboard.moc"
