#pragma once

#include <QString>
#include <QtGlobal>

namespace mervin {

// A file for FileProbe to check, with the mount recorded when it was opened
// (RecentEntry::mount), which RecentStore::isVolumeAttached takes into account.
struct FileCheck
{
    QString path;
    QString mount;
};

// What a check on a background thread found for one file (see FileProbe).
struct FileFacts
{
    bool exists = false;
    qint64 size = 0;   // bytes, when the file exists
    QString mount;     // RecentStore::mountOf, when the file exists
    // When the file does not exist: RecentStore::isVolumeAttached with the mount
    // recorded for the file, and, on an attached volume, whether its folder is
    // still there.
    bool volumeAttached = false;
    bool folderExists = false;

    // RecentStore::isRemovedFromDisk: gone from a volume that is attached.
    bool removed() const { return !exists && volumeAttached; }
    // The rule session restore uses to drop a document, gone from a folder that is
    // still there on an attached volume. A missing folder can be a drive mounted
    // where the volume check does not look, such as a share in the home folder.
    bool goneFromFolder() const { return removed() && folderExists; }
};

} // namespace mervin
