#pragma once

#include <QIcon>
#include <QList>
#include <QString>

#include <functional>

class QPoint;
class QWidget;

namespace mervin {

// A surface-specific entry shown above the shared file actions. `enabled`
// greys it out when its action does not apply; `icon` is the glyph shown in
// front of the label (see icons::glyph), null for none.
struct FileMenuItem
{
    QString label;
    std::function<void()> action;
    bool enabled = true;
    QIcon icon;
};

// Pops up the shared right-click menu for a PDF file at globalPos:
//   [optional leading items]
//   ───────────────────────
//   Open folder · Copy folder path · Copy file path · Copy file
//
// The common lower part keeps the Recent-list and document-tab menus identical.
// Each surface supplies its own leadingItems (the tab bar: "Move to new window",
// "Close all tabs"; the recent list: "Open in new window"); pass an empty list
// to omit them.
//
// "Open folder" opens the containing folder in the OS file browser (disabled
// when that folder no longer exists); the copy-path actions place the native-
// separator folder / file path on the clipboard. "Copy file" places the file
// itself on the clipboard Explorer-style so it can be pasted into a file
// manager or attached in a mail client (disabled when the file no longer
// exists). No-op when path is empty.
void showFileContextMenu(QWidget *parent, const QString &path, const QPoint &globalPos,
                         const QList<FileMenuItem> &leadingItems = {});

} // namespace mervin
