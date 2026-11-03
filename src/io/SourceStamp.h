#pragma once

#include <QString>
#include <QtGlobal>

#include <optional>

namespace mervin {

/// Identifies a filesystem object separately from the name currently reaching it.
struct FileIdentity
{
    quint64 volume = 0;
    quint64 file = 0;
    bool operator==(const FileIdentity &) const = default;
};

/// Captures source metadata without treating a failed lookup as a deleted file.
struct SourceStamp
{
    bool exists = false;
    qint64 size = -1;
    qint64 modified = 0;
    qint64 changed = 0;
    bool missing = false;
    std::optional<FileIdentity> identity;
    // Nanoseconds after the whole millisecond, absent in older recovery records.
    std::optional<int> modifiedSubMs;
    std::optional<int> changedSubMs;

    /// Compares identity when available, while accepting older recovery metadata.
    bool operator==(const SourceStamp &other) const;
};

/// Reads native metadata on the calling thread, which must be a worker thread.
SourceStamp sourceStamp(const QString &path);

/// Finds the nearest existing parent's volume, stopping on any error but absence.
/// Runs filesystem calls on the calling thread, which must be a worker thread.
std::optional<quint64> sourceAncestorVolume(const QString &path);

} // namespace mervin
