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
                         const QList<FileMenuItem> &leadingItems)
{
    if (path.isEmpty())
        return;

    const QFileInfo fi(path);
    const QString folder       = fi.absolutePath();
    const QString folderNative = QDir::toNativeSeparators(folder);
    const QString fileNative   = QDir::toNativeSeparators(fi.absoluteFilePath());

    QMenu menu(parent);

    QList<QAction *> leadingActions;
    for (const FileMenuItem &item : leadingItems) {
        QAction *a = menu.addAction(item.icon, item.label);
        a->setEnabled(item.enabled);
        leadingActions.append(a);
    }
    if (!leadingItems.isEmpty())
        menu.addSeparator();

    // House pictographs, tinted to the menu's neutral icon ink. The two path
    // items badge a small Copy glyph onto a folder / page base, so "copy the path
    // text" reads distinctly from the plain two-page Copy mark on "Copy file".
    using icons::Glyph;
    const QColor ink = Theme::iconInk(parent ? parent->palette() : QApplication::palette());
    QAction *openFolder = menu.addAction(icons::glyph(Glyph::Open, ink),
                                         QObject::tr("Open folder"));
    openFolder->setEnabled(!folder.isEmpty() && QFileInfo::exists(folder));
    QAction *copyFolder = menu.addAction(
        icons::glyphBadged(Glyph::Open, Glyph::Copy, ink),
        QObject::tr("Copy folder path"));
    QAction *copyFile = menu.addAction(
        icons::glyphBadged(Glyph::Document, Glyph::Copy, ink),
        QObject::tr("Copy file path"));
    QAction *copyObject = menu.addAction(icons::glyph(Glyph::Copy, ink),
                                         QObject::tr("Copy file"));
    copyObject->setEnabled(fi.isFile());

    QAction *chosen = menu.exec(globalPos);
    if (chosen == nullptr)
        return;
    for (int i = 0; i < leadingActions.size(); ++i) {
        if (chosen == leadingActions.at(i)) {
            if (leadingItems.at(i).action)
                leadingItems.at(i).action();
            return;
        }
    }
    if (chosen == openFolder) {
        QDesktopServices::openUrl(QUrl::fromLocalFile(folder));
    } else if (chosen == copyFolder) {
        QApplication::clipboard()->setText(folderNative);
    } else if (chosen == copyFile) {
        QApplication::clipboard()->setText(fileNative);
    } else if (chosen == copyObject) {
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
