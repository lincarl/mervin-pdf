#include "io/SourceStamp.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>

#ifdef Q_OS_WIN
#include <qt_windows.h>
#else
#include <cerrno>
#include <sys/stat.h>
#endif

namespace mervin {

bool SourceStamp::operator==(const SourceStamp &other) const
{
    return exists == other.exists && size == other.size && modified == other.modified
        && changed == other.changed
        && (!identity || !other.identity || identity == other.identity)
        && (!modifiedSubMs || !other.modifiedSubMs || modifiedSubMs == other.modifiedSubMs)
        && (!changedSubMs || !other.changedSubMs || changedSubMs == other.changedSubMs);
}

SourceStamp sourceStamp(const QString &path)
{
    SourceStamp stamp;
    stamp.size = 0;
#ifdef Q_OS_WIN
    QString native = QDir::toNativeSeparators(QDir::cleanPath(QFileInfo(path).absoluteFilePath()));
    // Extended paths retain long names without changing the process-wide policy.
    if (!native.startsWith(QStringLiteral("\\\\?\\"))) {
        if (native.startsWith(QStringLiteral("\\\\")))
            native = QStringLiteral("\\\\?\\UNC\\") + native.mid(2);
        else
            native.prepend(QStringLiteral("\\\\?\\"));
    }
    const HANDLE handle = CreateFileW(reinterpret_cast<LPCWSTR>(native.utf16()),
                                     FILE_READ_ATTRIBUTES,
                                     FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                     nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        const DWORD error = GetLastError();
        stamp.missing = error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND;
        return stamp;
    }
    BY_HANDLE_FILE_INFORMATION info{};
    FILE_BASIC_INFO basic{};
    const bool found = GetFileInformationByHandle(handle, &info);
    const bool hasBasic = GetFileInformationByHandleEx(handle, FileBasicInfo, &basic, sizeof(basic));
    CloseHandle(handle);
    if (!found)
        return stamp;
    constexpr quint64 epochOffset = 116444736000000000ULL;
    const auto epochMillis = [](quint64 ticks) -> qint64 {
        return static_cast<qint64>(ticks - epochOffset) / 10000;
    };
    stamp.exists = true;
    stamp.size = (quint64(info.nFileSizeHigh) << 32) | info.nFileSizeLow;
    const quint64 modified = (quint64(info.ftLastWriteTime.dwHighDateTime) << 32)
        | info.ftLastWriteTime.dwLowDateTime;
    stamp.modified = epochMillis(modified);
    stamp.modifiedSubMs = int(modified % 10000) * 100;
    if (hasBasic) {
        stamp.changed = epochMillis(basic.ChangeTime.QuadPart);
        stamp.changedSubMs = int(basic.ChangeTime.QuadPart % 10000) * 100;
    }
    stamp.identity = FileIdentity{info.dwVolumeSerialNumber,
                                  (quint64(info.nFileIndexHigh) << 32) | info.nFileIndexLow};
#else
    struct stat info{};
    const QByteArray native = QFile::encodeName(path);
    if (::stat(native.constData(), &info) != 0) {
        stamp.missing = errno == ENOENT || errno == ENOTDIR;
        return stamp;
    }
    stamp.exists = true;
    stamp.size = info.st_size;
#ifdef Q_OS_DARWIN
    stamp.modified = qint64(info.st_mtimespec.tv_sec) * 1000 + info.st_mtimespec.tv_nsec / 1000000;
    stamp.changed = qint64(info.st_ctimespec.tv_sec) * 1000 + info.st_ctimespec.tv_nsec / 1000000;
    stamp.modifiedSubMs = info.st_mtimespec.tv_nsec % 1000000;
    stamp.changedSubMs = info.st_ctimespec.tv_nsec % 1000000;
#else
    stamp.modified = qint64(info.st_mtim.tv_sec) * 1000 + info.st_mtim.tv_nsec / 1000000;
    stamp.changed = qint64(info.st_ctim.tv_sec) * 1000 + info.st_ctim.tv_nsec / 1000000;
    stamp.modifiedSubMs = info.st_mtim.tv_nsec % 1000000;
    stamp.changedSubMs = info.st_ctim.tv_nsec % 1000000;
#endif
    stamp.identity = FileIdentity{quint64(info.st_dev), quint64(info.st_ino)};
#endif
    return stamp;
}

std::optional<quint64> sourceAncestorVolume(const QString &path)
{
    QString parent = QFileInfo(path).absolutePath();
    for (;;) {
        const SourceStamp stamp = sourceStamp(parent);
        if (stamp.exists)
            return stamp.identity ? std::optional<quint64>(stamp.identity->volume) : std::nullopt;
        if (!stamp.missing)
            return std::nullopt;
        const QString next = QFileInfo(parent).absolutePath();
        if (next == parent)
            return std::nullopt;
        parent = next;
    }
}

} // namespace mervin
