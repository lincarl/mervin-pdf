#pragma once

#include <QString>

namespace mervin {

// Whether `path` names a file that copyFileToClipboard can offer. It touches the
// disk and can stall on an unreachable drive, so call it off the UI thread.
bool isCopyableFile(const QString &path);

// Places the file at the absolute `path` on the OS clipboard as a file-transfer
// object - the same thing Explorer's Copy does - so pasting into a file manager
// copies the file and pasting into a mail client attaches it. It does not touch
// the disk, so check isCopyableFile first. Returns false (clipboard untouched) for an
// empty path.
bool copyFileToClipboard(const QString &path);

} // namespace mervin
