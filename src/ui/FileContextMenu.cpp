#include "ui/FileContextMenu.h"

#include "platform/FileClipboard.h"
#include "ui/Icons.h"
#include "ui/Theme.h"

#include <QApplication>
#include <QClipboard>
#include <QDesktopServices>
#include <QDir>
#include <QFileInfo>
#include <QMenu>
#include <QMessageBox>
#include <QUrl>

namespace mervin {

void showFileContextMenu(QWidget *parent, const QString &path, const QPoint &globalPos,
                         const QList<FileMenuItem> &leadingItems,
                         const QList<FileMenuItem> &trailingItems)
{
    if (path.isEmpty())
        return;

    const QFileInfo fi(path);
    const QString folder       = fi.absolutePath();
    const QString folderNative = QDir::toNativeSeparators(folder);
    const QString fileNative   = QDir::toNativeSeparators(fi.absoluteFilePath());

    QMenu menu(parent);

    // Surface items, paired with their QAction so the chosen one can be run
    // after exec() returns.
    QList<std::pair<QAction *, FileMenuItem>> surfaceActions;
    const auto addSurfaceItems = [&](const QList<FileMenuItem> &items) {
        for (const FileMenuItem &item : items) {
            QAction *a = menu.addAction(item.icon, item.label);
            a->setEnabled(item.enabled);
            surfaceActions.append({a, item});
        }
    };

    addSurfaceItems(leadingItems);
    if (!leadingItems.isEmpty())
        menu.addSeparator();

    // House pictographs, tinted to the menu's neutral icon ink. The two path
    // items badge a small Copy glyph onto a page / folder base, so "copy the path
    // text" reads distinctly from the plain two-page Copy mark on "Copy file".
    using icons::Glyph;
    const QColor ink = Theme::iconInk(parent ? parent->palette() : QApplication::palette());
    QAction *copyFile = menu.addAction(icons::glyph(Glyph::Copy, ink),
                                       QObject::tr("Copy file"));
    copyFile->setEnabled(fi.isFile());
    QAction *openFolder = menu.addAction(icons::glyph(Glyph::Open, ink),
                                         QObject::tr("Open folder"));
    openFolder->setEnabled(!folder.isEmpty() && QFileInfo::exists(folder));
    menu.addSeparator();
    QAction *copyFilePath = menu.addAction(
        icons::glyphBadged(Glyph::Document, Glyph::Copy, ink),
        QObject::tr("Copy file path"));
    QAction *copyFolderPath = menu.addAction(
        icons::glyphBadged(Glyph::Open, Glyph::Copy, ink),
        QObject::tr("Copy folder path"));

    if (!trailingItems.isEmpty())
        menu.addSeparator();
    addSurfaceItems(trailingItems);

    QAction *chosen = menu.exec(globalPos);
    if (chosen == nullptr)
        return;
    for (const auto &[action, item] : surfaceActions) {
        if (chosen == action) {
            if (item.action)
                item.action();
            return;
        }
    }
    if (chosen == openFolder) {
        QDesktopServices::openUrl(QUrl::fromLocalFile(folder));
    } else if (chosen == copyFolderPath) {
        QApplication::clipboard()->setText(folderNative);
    } else if (chosen == copyFilePath) {
        QApplication::clipboard()->setText(fileNative);
    } else if (chosen == copyFile) {
        // The enabled state was computed when the menu opened; the file can
        // vanish while the menu is up, in which case the helper leaves the
        // clipboard untouched - do not let that look like a successful copy.
        if (!copyFileToClipboard(fi.absoluteFilePath())) {
            QMessageBox::warning(parent, QObject::tr("Copy file"),
                                 QObject::tr("Could not copy \"%1\": the file no longer exists.")
                                     .arg(fileNative));
        }
    }
}

} // namespace mervin
