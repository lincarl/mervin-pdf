#pragma once

#include "io/FileFacts.h"

#include <QIcon>
#include <QList>
#include <QString>

#include <functional>
#include <optional>

class QPoint;
class QWidget;

namespace mervin {

// A surface-specific entry shown below the shared file actions. `enabled`
// greys it out when its action does not apply; `icon` is the glyph shown in
// front of the label (see icons::glyph), null for none.
struct FileMenuItem
{
    QString label;
    std::function<void()> action;
    bool enabled = true;
    QIcon icon;
};

// Shared file menu at globalPos: copy file, open containing folder, copy file/folder paths,
// then optional surfaceItems. File copy uses the OS file clipboard; paths use native
// separators. `facts`, what a FileProbe check found, disables the actions for a missing
// file or folder; without it they stay enabled. The menu never touches the disk on the
// calling thread. Copy file checks the file on a background thread first. An empty path is
// a no-op.
void showFileContextMenu(QWidget *parent, const QString &path, const QPoint &globalPos,
                         const QList<FileMenuItem> &surfaceItems = {},
                         const std::optional<FileFacts> &facts = {});

} // namespace mervin
