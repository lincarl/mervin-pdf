#pragma once

#include <QIcon>
#include <QList>
#include <QString>

#include <functional>

class QPoint;
class QWidget;

namespace mervin {

// A surface-specific entry shown above or below the shared file actions.
// `enabled` greys it out when its action does not apply; `icon` is the glyph
// shown in front of the label (see icons::glyph), null for none.
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
//   Copy file · Open folder
//   ───────────────────────
//   Copy file path · Copy folder path
//   ───────────────────────
//   [optional trailing items]
//
// The file actions in the middle keep the Recent-list and document-tab menus
// identical. Each surface supplies its own items around them (the recent list
// leads with "Open in new window"; the tab bar trails with "Duplicate to new
// window", "Move to new window", "Close all tabs"); pass an empty list to omit
// either group.
//
// "Open folder" opens the containing folder in the OS file browser (disabled
// when that folder no longer exists); the copy-path actions place the native-
// separator folder / file path on the clipboard. "Copy file" places the file
// itself on the clipboard Explorer-style so it can be pasted into a file
// manager or attached in a mail client (disabled when the file no longer
// exists). No-op when path is empty.
void showFileContextMenu(QWidget *parent, const QString &path, const QPoint &globalPos,
                         const QList<FileMenuItem> &leadingItems = {},
                         const QList<FileMenuItem> &trailingItems = {});

} // namespace mervin
