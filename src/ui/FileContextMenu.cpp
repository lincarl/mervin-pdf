#include "ui/FileContextMenu.h"

#include "io/Background.h"
#include "platform/FileClipboard.h"
#include "ui/Icons.h"
#include "ui/Theme.h"

#include <QApplication>
#include <QClipboard>
#include <QCoreApplication>
#include <QDesktopServices>
#include <QDir>
#include <QFileInfo>
#include <QMenu>
#include <QMessageBox>
#include <QUrl>

#include <memory>

#ifdef Q_OS_WIN
#  define WIN32_LEAN_AND_MEAN
#  define NOMINMAX
#  include <windows.h>
#  include <objbase.h>
#  include <shellapi.h>

#  include <string>
#endif

namespace mervin {

namespace {

// Shows `folder` in the file manager.
void showFolder(const QString &folder)
{
#ifdef Q_OS_WIN
    // ShellExecute reaches the folder before it returns, which stalls on a share
    // that stopped answering, so it runs on a background thread.
    const std::wstring native = QDir::toNativeSeparators(folder).toStdWString();
    background::start([native] {
        const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
        ShellExecuteW(nullptr, L"open", native.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        if (SUCCEEDED(com))
            CoUninitialize();
    });
#else
    // xdg-open or the desktop portal opens it in another process.
    QDesktopServices::openUrl(QUrl::fromLocalFile(folder));
#endif
}

} // namespace

// A free function has no tr(), so each string names its context,
// "mervin::FileContextMenu".
void showFileContextMenu(QWidget *parent, const QString &path, const QPoint &globalPos,
                         const QList<FileMenuItem> &surfaceItems,
                         const std::optional<FileFacts> &facts)
{
    if (path.isEmpty())
        return;

    const QFileInfo fi(path);
    const QString folder       = fi.absolutePath();
    const QString folderNative = QDir::toNativeSeparators(folder);
    const QString fileNative   = QDir::toNativeSeparators(fi.absoluteFilePath());

    QMenu menu(parent);

    // House pictographs, tinted to the menu's neutral icon ink. The two path
    // items badge a small Copy glyph onto a page / folder base, so "copy the path
    // text" reads distinctly from the plain two-page Copy mark on "Copy file".
    using icons::Glyph;
    const QColor ink = Theme::iconInk(parent ? parent->palette() : QApplication::palette());
    //: Menu item that puts the file itself on the clipboard, to paste in a file manager.
    const QString copyFileText = QCoreApplication::translate("mervin::FileContextMenu", "Copy file");
    QAction *copyFile = menu.addAction(icons::glyph(Glyph::Copy, ink), copyFileText);
    copyFile->setEnabled(!facts || facts->exists);
    //: Menu item that shows the folder holding the file.
    QAction *openFolder = menu.addAction(
        icons::glyph(Glyph::Open, ink),
        QCoreApplication::translate("mervin::FileContextMenu", "Open folder"));
    openFolder->setEnabled(!folder.isEmpty() && (!facts || facts->exists || facts->folderExists));
    menu.addSeparator();
    QAction *copyFilePath = menu.addAction(
        icons::glyphBadged(Glyph::Document, Glyph::Copy, ink),
        QCoreApplication::translate("mervin::FileContextMenu", "Copy file path"));
    QAction *copyFolderPath = menu.addAction(
        icons::glyphBadged(Glyph::Open, Glyph::Copy, ink),
        QCoreApplication::translate("mervin::FileContextMenu", "Copy folder path"));

    // Surface items; their QActions are kept so the chosen one can be run
    // after exec() returns.
    if (!surfaceItems.isEmpty())
        menu.addSeparator();
    QList<QAction *> surfaceActions;
    for (const FileMenuItem &item : surfaceItems) {
        QAction *a = menu.addAction(item.icon, item.label);
        a->setEnabled(item.enabled);
        surfaceActions.append(a);
    }

    QAction *chosen = menu.exec(globalPos);
    if (chosen == nullptr)
        return;
    if (const qsizetype i = surfaceActions.indexOf(chosen); i >= 0) {
        if (surfaceItems.at(i).action)
            surfaceItems.at(i).action();
        return;
    }
    if (chosen == openFolder) {
        showFolder(folder);
    } else if (chosen == copyFolderPath) {
        QApplication::clipboard()->setText(folderNative);
    } else if (chosen == copyFilePath) {
        QApplication::clipboard()->setText(fileNative);
    } else if (chosen == copyFile) {
        // The enabled state was known when the menu opened; the file can vanish
        // while the menu is up, in which case the clipboard stays untouched - do
        // not let that look like a successful copy.
        const QString file = fi.absoluteFilePath();
        QObject *context = parent ? static_cast<QObject *>(parent) : QCoreApplication::instance();
        // The check can wait on a slow share. Anything copied meanwhile is newer
        // and stays, so the late answer then does nothing.
        const auto overtaken = std::make_shared<bool>(false);
        const QMetaObject::Connection watch =
            QObject::connect(QApplication::clipboard(), &QClipboard::dataChanged, context,
                             [overtaken] { *overtaken = true; });
        background::run(
            context, [file] { return isCopyableFile(file); },
            [parent, file, copyFileText, fileNative, overtaken, watch](bool copyable) {
                QObject::disconnect(watch);
                if (*overtaken)
                    return;
                if (copyable) {
                    copyFileToClipboard(file);
                    return;
                }
                //: %1 is the file's path.
                QMessageBox::warning(parent, copyFileText,
                                     QCoreApplication::translate(
                                         "mervin::FileContextMenu",
                                         "Could not copy \"%1\": the file no longer exists.")
                                         .arg(fileNative));
            });
    }
}

} // namespace mervin
