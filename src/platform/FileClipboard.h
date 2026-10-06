#pragma once

#include <QString>

namespace mervin {

// Places the file itself on the OS clipboard as a file-transfer object - the
// same thing Explorer's Copy does - so pasting into a file manager copies the
// file and pasting into a mail client attaches it. Returns false (clipboard
// untouched) when path does not name an existing file.
bool copyFileToClipboard(const QString &path);

} // namespace mervin
