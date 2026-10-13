#pragma once

#include <QByteArray>
#include <QString>

// The app's own files, such as the settings, Recent and the session, written on a
// background thread. The profile folder can be on a network share, as with a
// roaming or redirected Windows profile or a home folder on NFS, so the UI thread
// never writes them itself. Writes and removals happen in the order asked, on one
// thread. A newer write of a file whose write has not started yet replaces it.
namespace mervin::appfiles {

// Writes `data` to `path` with QSaveFile, creating its folder. A failure is
// logged; the next write of the file tries again.
void write(const QString &path, const QByteArray &data);

// How a write that a caller waits for went.
enum class Wait
{
    Written,
    Failed,  // the write failed; it is not tried again
    Pending, // still waiting, as when the folder stopped answering; it lands later
};

// write(), then waits up to `ms` milliseconds for that write, for a caller that
// must report the result. `error` gets the reason for a failure.
Wait writeAndWait(const QString &path, const QByteArray &data, int ms, QString *error = nullptr);

// Removes `path`, after everything asked for before.
void remove(const QString &path);

// Waits up to `ms` milliseconds for everything asked for so far, for the end of
// the process and for tests. True when nothing is left.
bool flush(int ms);

} // namespace mervin::appfiles
