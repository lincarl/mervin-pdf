#pragma once

#include <QIcon>
#include <QList>
#include <QString>

#include <functional>

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
// separators. Missing files/folders disable their respective actions. An empty path is a no-op.
void showFileContextMenu(QWidget *parent, const QString &path, const QPoint &globalPos,
                         const QList<FileMenuItem> &surfaceItems = {});

} // namespace mervin
