#include "platform/FileClipboard.h"

#include <QClipboard>
#include <QFileInfo>
#include <QGuiApplication>
#include <QMimeData>
#include <QUrl>

namespace mervin {

bool copyFileToClipboard(const QString &path)
{
    const QFileInfo fi(path);
    if (!fi.isFile())
        return false;

    const QUrl url = QUrl::fromLocalFile(fi.absoluteFilePath());
    auto *mime = new QMimeData;
    mime->setUrls({url});

#if defined(Q_OS_WIN)
    // Qt maps the URL list to CF_HDROP, but without CFSTR_PREFERREDDROPEFFECT
    // some paste targets treat the transfer as a cut. Explorer's own Copy puts
    // DROPEFFECT_COPY | DROPEFFECT_LINK (5) there, as a little-endian DWORD.
    QByteArray dropEffect(4, '\0');
    dropEffect[0] = 0x5;
    mime->setData(
        QStringLiteral("application/x-qt-windows-mime;value=\"Preferred DropEffect\""),
        dropEffect);
#else
    // GNOME file managers paste files from this format ("copy\n<uri>");
    // everything else falls back to the text/uri-list from setUrls().
    mime->setData(QStringLiteral("x-special/gnome-copied-files"),
                  QByteArray("copy\n") + url.toEncoded());
#endif

    QGuiApplication::clipboard()->setMimeData(mime);
    return true;
}

} // namespace mervin
