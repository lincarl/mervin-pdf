#pragma once

#include <QString>
#include <QtGlobal>

namespace mervin {

// One entry in the recent-files history: the absolute path as last opened plus
// the time it was last opened. Plain value type shared by the host store and
// the IPC layer; existence-on-disk is checked by the UI when it renders, not
// stored here.
struct RecentEntry
{
    QString path;          // absolute path as last opened
    qint64 lastOpened = 0; // epoch milliseconds (UTC); larger == more recent
    int pageCount = 0;     // total pages; 0 = unknown (older entries / not yet cached)
    bool favorite = false; // user-starred; persisted as "f" in recent.json
    // The mount point of the file system the file was on when last opened while it
    // was there, such as "/", "/home" or "/home/ann/nas" (RecentStore::mountOf).
    // Linux only, "" when not known. Persisted as "m".
    QString mount;
};

} // namespace mervin
