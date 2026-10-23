#include "ipc/PipeName.h"

#include "config/ConfigPaths.h"
#include "ipc/Handoff.h"
#include "platform/PlatformIntegration.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLockFile>
#include <QSaveFile>
#include <QScopeGuard>
#include <QSysInfo>

#ifdef Q_OS_WIN
#  define WIN32_LEAN_AND_MEAN
#  define NOMINMAX
#  include <windows.h>
#  include <aclapi.h>
#endif

#ifdef Q_OS_UNIX
#  include <csignal>

#  include <sys/stat.h>
#  include <sys/un.h>
#  include <unistd.h>
#endif

namespace mervin::ipc {

namespace {

// How long a lock file may be without content while its maker writes it.
constexpr qint64 kUnwrittenLockMs = 2000;

// A short, stable, filesystem/pipe-safe token for `user`.
QString tokenOf(const QByteArray &user)
{
    const QByteArray digest = QCryptographicHash::hash(user, QCryptographicHash::Sha1).toHex();
    return QString::fromLatin1(digest.left(16));
}

#if defined(Q_OS_WIN) || defined(Q_OS_LINUX)
// The user's name as the environment gives it.
QByteArray userName()
{
    QString name = qEnvironmentVariable("USERNAME");
    if (name.isEmpty())
        name = qEnvironmentVariable("USER");
    if (name.isEmpty())
        name = QStringLiteral("default");
    return name.toUtf8();
}
#endif

// The token identifying the current user.
QString userToken()
{
    // The OS identity, not a username supplied by the launching environment.
    const QByteArray user = currentUser();
    return user.isEmpty() ? QString() : tokenOf(user);
}

// Resolve links in the existing part of a profile, including before its first creation.
QString profileIdentity(const QString &profile)
{
    QFileInfo ancestor(profile);
    QStringList missing;
    for (;;) {
        if (const QString canonical = ancestor.canonicalFilePath(); !canonical.isEmpty())
            return QDir::cleanPath(canonical + QLatin1Char('/') + missing.join(QLatin1Char('/')));
        const QString parent = ancestor.absolutePath();
        if (parent == ancestor.absoluteFilePath())
            return QDir::cleanPath(profile);
        missing.prepend(ancestor.fileName());
        ancestor.setFile(parent);
    }
}

// The single-instance group of the user with `token`.
QString groupName(const QString &token, bool canonicalProfile = true)
{
    if (token.isEmpty())
        return {};
    QString name = QStringLiteral("MervinPDF-") + token;
    // Store and MSI copies can coexist. A Store activation must not hand its
    // document to an MSI process with a different updater and default-app identity.
    if (PlatformIntegration::hasPackageIdentity())
        name += QStringLiteral("-msix");

    // A --profile instance forms its own single-instance group: hash the
    // profile directory into the pipe name so it never hands its files to (or
    // gets blocked by) the installed app, while two launches on the SAME
    // profile still coordinate normally.
    const QString profile = ConfigPaths::overrideDir();
    if (!profile.isEmpty()) {
        QString key = canonicalProfile ? profileIdentity(profile) : QDir::cleanPath(profile);
#ifdef Q_OS_WIN
        key = key.toLower(); // Windows paths are case-insensitive
#endif
        const QByteArray digest =
            QCryptographicHash::hash(key.toUtf8(), QCryptographicHash::Sha1).toHex();
        name += QLatin1Char('-') + QString::fromLatin1(digest.left(16));
    }
    return name;
}

// A short token for this machine. QLockFile names the host of a lock the same way.
QString hostToken()
{
    QByteArray id = QSysInfo::machineUniqueId();
    if (id.isEmpty())
        id = QSysInfo::machineHostName().toUtf8();
    const QByteArray digest = QCryptographicHash::hash(id, QCryptographicHash::Sha1).toHex();
    return QString::fromLatin1(digest.left(8));
}

// This machine's folder in the profile folder, for the lock of the files there. Only
// computes the path.
QString profileInstanceDirPath()
{
    return QDir(ConfigPaths::configDirPath()).filePath(QStringLiteral("instance-") + hostToken());
}

#ifdef Q_OS_WIN
// Validate the opened legacy object, including inherited per-user TEMP ACLs.
// Unlike new instance directories, these may also grant Administrators access.
bool trustedLegacyObject(HANDLE handle, bool directory)
{
    BY_HANDLE_FILE_INFORMATION info{};
    if (!GetFileInformationByHandle(handle, &info)
        || bool(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != directory
        || (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
        return false;
    const QByteArray userBytes = currentUser();
    if (userBytes.isEmpty())
        return false;
    const PSID user = const_cast<char *>(userBytes.constData());
    BYTE system[SECURITY_MAX_SID_SIZE];
    BYTE administrators[SECURITY_MAX_SID_SIZE];
    DWORD systemSize = sizeof(system);
    DWORD administratorsSize = sizeof(administrators);
    if (!CreateWellKnownSid(WinLocalSystemSid, nullptr, system, &systemSize)
        || !CreateWellKnownSid(WinBuiltinAdministratorsSid, nullptr,
                               administrators, &administratorsSize))
        return false;
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    PSID owner = nullptr;
    PACL acl = nullptr;
    if (GetSecurityInfo(handle, SE_FILE_OBJECT,
                        OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
                        &owner, nullptr, &acl, nullptr, &descriptor) != ERROR_SUCCESS)
        return false;
    const auto freeDescriptor = qScopeGuard([&] { LocalFree(descriptor); });
    if (!owner || !EqualSid(owner, user) || !acl)
        return false;
    for (DWORD index = 0; index < acl->AceCount; ++index) {
        void *raw = nullptr;
        if (!GetAce(acl, index, &raw))
            return false;
        const auto *ace = static_cast<const ACCESS_ALLOWED_ACE *>(raw);
        if (ace->Header.AceType != ACCESS_ALLOWED_ACE_TYPE)
            return false;
        PSID sid = const_cast<DWORD *>(&ace->SidStart);
        if (!IsValidSid(sid) || (!EqualSid(sid, user) && !EqualSid(sid, system)
                                && !EqualSid(sid, administrators)))
            return false;
    }
    return true;
}

// Inspect a legacy file without following a reparse point or blocking its removal.
bool trustedLegacyFile(const QString &path, bool allowMissing)
{
    const std::wstring native = QDir::toNativeSeparators(path).toStdWString();
    const HANDLE handle = CreateFileW(native.c_str(), READ_CONTROL | FILE_READ_ATTRIBUTES,
                                      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                      nullptr, OPEN_EXISTING,
                                      FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS,
                                      nullptr);
    if (handle == INVALID_HANDLE_VALUE)
        return allowMissing && GetLastError() == ERROR_FILE_NOT_FOUND;
    const auto close = qScopeGuard([&] { CloseHandle(handle); });
    return trustedLegacyObject(handle, false);
}

// Create an owner-and-SYSTEM folder, or accept an existing private folder.
// Never repair a preexisting directory's permissions or follow a reparse point.
bool privateInstanceDirectory(const QString &path)
{
    const QByteArray userBytes = currentUser();
    if (userBytes.isEmpty())
        return false;
    const PSID user = const_cast<char *>(userBytes.constData());
    BYTE system[SECURITY_MAX_SID_SIZE];
    DWORD systemSize = sizeof(system);
    if (!CreateWellKnownSid(WinLocalSystemSid, nullptr, system, &systemSize))
        return false;
    EXPLICIT_ACCESSW entries[2]{};
    entries[0].grfAccessPermissions = FILE_ALL_ACCESS;
    entries[0].grfAccessMode = SET_ACCESS;
    entries[0].grfInheritance = SUB_CONTAINERS_AND_OBJECTS_INHERIT;
    entries[0].Trustee.TrusteeForm = TRUSTEE_IS_SID;
    entries[0].Trustee.ptstrName = static_cast<LPWSTR>(user);
    entries[1] = entries[0];
    entries[1].Trustee.ptstrName = reinterpret_cast<LPWSTR>(system);
    PACL acl = nullptr;
    if (SetEntriesInAclW(2, entries, nullptr, &acl) != ERROR_SUCCESS)
        return false;
    const auto freeAcl = qScopeGuard([&] { LocalFree(acl); });
    SECURITY_DESCRIPTOR descriptor{};
    if (!InitializeSecurityDescriptor(&descriptor, SECURITY_DESCRIPTOR_REVISION)
        || !SetSecurityDescriptorOwner(&descriptor, user, FALSE)
        || !SetSecurityDescriptorDacl(&descriptor, TRUE, acl, FALSE)
        || !SetSecurityDescriptorControl(&descriptor, SE_DACL_PROTECTED, SE_DACL_PROTECTED))
        return false;
    SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES), &descriptor, FALSE};
    const std::wstring native = QDir::toNativeSeparators(path).toStdWString();
    if (!CreateDirectoryW(native.c_str(), &security) && GetLastError() != ERROR_ALREADY_EXISTS)
        return false;
    const DWORD attributes = GetFileAttributesW(native.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES || !(attributes & FILE_ATTRIBUTE_DIRECTORY)
        || (attributes & FILE_ATTRIBUTE_REPARSE_POINT))
        return false;
    PSECURITY_DESCRIPTOR actual = nullptr;
    PSID owner = nullptr;
    PACL actualAcl = nullptr;
    if (GetNamedSecurityInfoW(native.c_str(), SE_FILE_OBJECT,
                             OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
                             &owner, nullptr, &actualAcl, nullptr, &actual) != ERROR_SUCCESS)
        return false;
    const auto freeActual = qScopeGuard([&] { LocalFree(actual); });
    SECURITY_DESCRIPTOR_CONTROL control = 0;
    DWORD revision = 0;
    if (!owner || !EqualSid(owner, user) || !actualAcl
        || !GetSecurityDescriptorControl(actual, &control, &revision)
        || !(control & SE_DACL_PROTECTED))
        return false;
    for (DWORD index = 0; index < actualAcl->AceCount; ++index) {
        void *raw = nullptr;
        if (!GetAce(actualAcl, index, &raw))
            return false;
        const auto *ace = static_cast<const ACCESS_ALLOWED_ACE *>(raw);
        if (ace->Header.AceType != ACCESS_ALLOWED_ACE_TYPE)
            return false;
        PSID sid = const_cast<DWORD *>(&ace->SidStart);
        if (!IsValidSid(sid) || (!EqualSid(sid, user) && !EqualSid(sid, system)))
            return false;
    }
    return true;
}
#endif

#ifdef Q_OS_UNIX
// Qt reserves a terminator and, on Linux, the abstract-socket prefix byte.
#  ifdef Q_OS_LINUX
constexpr qsizetype kQtSpareBytes = 2;
#  else
constexpr qsizetype kQtSpareBytes = 1;
#  endif

// Whether Qt takes the socket path of `group` in `dir`.
bool socketFits(const QString &dir, const QString &group)
{
    return QFile::encodeName(QDir(dir).filePath(group)).size() + kQtSpareBytes
        <= qsizetype(sizeof(sockaddr_un::sun_path));
}

// Fixed roots keep the lock and socket independent of each launch's environment.
QString g_userRuntimeRoot = QStringLiteral("/run/user");
QString g_socketRoot = QStringLiteral("/tmp");

// Use the OS runtime folder only when this user owns it and its mode is 0700.
QString userRuntimeDir()
{
    const QString dir = QDir(g_userRuntimeRoot).filePath(QString::number(uint(::geteuid())));
    return isPrivateDir(dir, uint(::geteuid())) ? dir : QString();
}

// This user's folder in the socket root, for a socket the profile folder cannot hold.
// Only computes the path.
QString socketDirPath()
{
    return QDir(g_socketRoot).filePath(QStringLiteral("mervin-pdf-%1").arg(uint(::geteuid())));
}

// Makes `dir` if it is missing, and returns it if it is this user's own 0700 folder and
// not a link, else empty. A folder this call made is this user's, whatever the umask made
// of its mode. One that already exists is refused if it is anything else, not repaired.
// The folder that holds `dir` is left as it is.
QString ownPrivateDir(const QString &dir)
{
    const QByteArray native = QFile::encodeName(dir);
    if (::mkdir(native.constData(), S_IRWXU) == 0)
        ::chmod(native.constData(), S_IRWXU);
    return isPrivateDir(dir, uint(::geteuid())) ? dir : QString();
}

// The lock file, endpoint and carry file of `group` in `dir`.
InstancePaths pathsIn(const QString &dir, const QString &group)
{
    const QDir folder(dir);
    return {folder.filePath(group + QStringLiteral(".lock")), folder.filePath(group),
            folder.filePath(group + QStringLiteral(".carried")), QString()};
}

// The socket of `group` in this user's socket folder, which is made only if `make` is true.
// Empty if that folder is refused or Qt does not take the path.
QString socketInSocketFolder(const QString &group, bool make)
{
    const QString socketDir = socketDirPath();
    const bool usable =
        make ? !ownPrivateDir(socketDir).isEmpty() : isPrivateDir(socketDir, uint(::geteuid()));
    return usable && socketFits(socketDir, group) ? QDir(socketDir).filePath(group) : QString();
}

// The paths of `group` in the profile's instance folder `dir`. The socket goes there if Qt
// takes its path, else to this user's socket folder, which is made only if `make` is
// true. Without a usable socket folder `server` is empty. The primary records where it
// listens in the endpoint file.
InstancePaths instanceFolderPaths(const QString &dir, const QString &group, bool make)
{
    InstancePaths paths = pathsIn(dir, group);
    paths.endpointFile = QDir(dir).filePath(group + QStringLiteral(".endpoint"));
    if (!socketFits(dir, group))
        paths.server = socketInSocketFolder(group, make);
    return paths;
}
#endif

} // namespace

QString hostPipeName()
{
    return groupName(userToken());
}

#ifdef Q_OS_WIN
// The directory stays pinned until after the legacy QLockFile releases its handle.
class LegacyCarryGuard
{
public:
    ~LegacyCarryGuard()
    {
        lock.reset();
        if (directory != INVALID_HANDLE_VALUE)
            CloseHandle(directory);
    }

    HANDLE directory = INVALID_HANDLE_VALUE;
    std::unique_ptr<QLockFile> lock;
};

InstancePaths legacyInstancePaths(const QString &name)
{
    const QString group = legacyEndpoint(name);
    const QDir folder(QDir::tempPath());
    return {folder.filePath(group + QStringLiteral(".lock")), group,
            folder.filePath(group + QStringLiteral(".carried")), QString()};
}

std::shared_ptr<LegacyCarryGuard> guardLegacyCarry(const InstancePaths &paths)
{
    const QString parent = QFileInfo(paths.carriedFile).absolutePath();
    if (paths.carriedFile.isEmpty() || paths.lockFile.isEmpty()
        || QFileInfo(paths.lockFile).absolutePath() != parent)
        return {};
    auto guard = std::make_shared<LegacyCarryGuard>();
    const std::wstring native = QDir::toNativeSeparators(parent).toStdWString();
    guard->directory = CreateFileW(native.c_str(), READ_CONTROL | FILE_READ_ATTRIBUTES,
                                   FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                                   FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS,
                                   nullptr);
    if (guard->directory == INVALID_HANDLE_VALUE || !trustedLegacyObject(guard->directory, true)
        || !trustedLegacyFile(paths.carriedFile, false)
        || !trustedLegacyFile(paths.lockFile, true))
        return {};
    guard->lock = std::make_unique<QLockFile>(paths.lockFile);
    guard->lock->setStaleLockTime(0);
    bool locked = guard->lock->tryLock(0);
    if (!locked && guard->lock->error() == QLockFile::LockFailedError
        && lockLeftUnwritten(paths.lockFile) && guard->lock->removeStaleLockFile())
        locked = guard->lock->tryLock(0);
    if (!locked || !trustedLegacyFile(paths.carriedFile, false))
        return {};
    return guard;
}
#endif

InstancePaths instancePaths(const QString &name)
{
    const QString group = name.isEmpty() ? hostPipeName() : name;
    if (group.isEmpty())
        return {};
#ifdef Q_OS_UNIX
    // Chosen from what is on disk, not from the environment, so that every launch of
    // this user and group on this host takes the same lock.
    const QString runtime = userRuntimeDir();
    if (!runtime.isEmpty() && socketFits(runtime, group))
        return pathsIn(runtime, group);
    // Without it, the lock goes with the files it guards. Launches that use the same
    // profile folder take the same lock, whatever their environment.
    ConfigPaths::configDir(); // makes the profile folder if it is missing
    const QString instance = ownPrivateDir(profileInstanceDirPath());
    if (instance.isEmpty())
        return {};
    return instanceFolderPaths(instance, group, true);
#else
    // Keep the lock with the profile it guards, independent of TEMP or TMP.
    // The machine token separates roaming profiles used on different computers.
    ConfigPaths::configDir();
    const QString instance = profileInstanceDirPath();
    if (!privateInstanceDirectory(instance))
        return {};
    const QDir folder(instance);
    return {folder.filePath(group + QStringLiteral(".lock")), group,
            folder.filePath(group + QStringLiteral(".carried")), QString()};
#endif
}

QString endpointOf(const InstancePaths &paths)
{
    if (paths.endpointFile.isEmpty())
        return paths.server;
    QFile file(paths.endpointFile);
    if (!file.open(QIODevice::ReadOnly))
        return paths.server;
    const QString recorded = QString::fromUtf8(file.read(4096)).trimmed();
    return QDir::isAbsolutePath(recorded) ? recorded : paths.server;
}

void recordEndpoint(const InstancePaths &paths)
{
    if (paths.endpointFile.isEmpty())
        return;
    if (paths.server.isEmpty()) {
        QFile::remove(paths.endpointFile);
        return;
    }
    QSaveFile out(paths.endpointFile);
    if (!out.open(QIODevice::WriteOnly))
        return;
    out.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    out.write(paths.server.toUtf8());
    out.commit();
}

#ifdef Q_OS_UNIX
QList<InstancePaths> otherInstancePaths(const QString &name)
{
    const QString group = name.isEmpty() ? hostPipeName() : name;
    const QString own = instancePaths(name).lockFile;
    QList<InstancePaths> others;
    QStringList groups{group};
    // The preceding UID-based version hashed the path as supplied. A launch using
    // that same profile alias must still find its lock before taking ownership.
    if (name.isEmpty()) {
        const QString previous = groupName(userToken(), false);
        if (previous != group)
            groups.append(previous);
    }
    // Each folder is only checked with lstat. Nothing is created in any of them.
    const QString runtime = userRuntimeDir();
    const QString instance = profileInstanceDirPath();
    const bool privateInstance = isPrivateDir(instance, uint(::geteuid()));
    for (const QString &candidate : groups) {
        if (!runtime.isEmpty() && socketFits(runtime, candidate)) {
            const InstancePaths paths = pathsIn(runtime, candidate);
            if (paths.lockFile != own)
                others.append(paths);
        }
        if (privateInstance) {
            const InstancePaths paths = instanceFolderPaths(instance, candidate, false);
            if (paths.lockFile != own)
                others.append(paths);
        }
    }
    return others;
}

bool lockHeldByLiveProcess(const QString &lockFile)
{
    QFile file(lockFile);
    if (!file.open(QIODevice::ReadOnly))
        return false;
    // QLockFile writes the process ID, the program's file name, the host name, the machine
    // ID and the boot ID, one per line.
    const QList<QByteArray> lines = file.read(4096).split('\n');
    file.close();
    bool numeric = false;
    const qint64 pid = lines.value(0).toLongLong(&numeric);
    if (!numeric || pid <= 0)
        return !lockLeftUnwritten(lockFile);
    // Each folder instancePaths() chooses belongs to this machine. A lock made under another
    // machine ID, or in an earlier boot of this machine, has no live holder here.
    const QByteArray machine = QSysInfo::machineUniqueId();
    if (!lines.value(3).isEmpty() && !machine.isEmpty() && lines.value(3) != machine)
        return false;
    const QByteArray boot = QSysInfo::bootUniqueId();
    if (!lines.value(4).isEmpty() && !boot.isEmpty() && lines.value(4) != boot)
        return false;
    // Only this user's processes make locks in this user's private folders, and this user can
    // signal each of them. Any failure, EPERM included, means that the ID now belongs to
    // another user, root or the kernel, and the process that made the lock has ended.
    if (::kill(pid_t(pid), 0) != 0)
        return false;
#  ifdef Q_OS_LINUX
    // A process of this user that now runs another program under that ID does not hold the
    // lock. One whose program cannot be read still does, as a process whose last thread
    // still waits in a file system call after the rest of it ended. A program file that was
    // replaced while it ran, as by an update, counts as the same.
    const QString app = QString::fromUtf8(lines.value(1));
    QString exe = QFileInfo(QStringLiteral("/proc/%1/exe").arg(pid)).symLinkTarget();
    exe.remove(QStringLiteral(" (deleted)"));
    if (!exe.isEmpty() && !app.isEmpty() && QFileInfo(exe).fileName() != app)
        return false;
#  endif
    return true;
}

void setUserRuntimeRootForTests(const QString &root)
{
    g_userRuntimeRoot = root.isEmpty() ? QStringLiteral("/run/user") : root;
}

QString socketFolderServer(const QString &name)
{
    return socketInSocketFolder(name.isEmpty() ? hostPipeName() : name, true);
}

void setSocketRootForTests(const QString &root)
{
    g_socketRoot = root.isEmpty() ? QStringLiteral("/tmp") : root;
}
#endif

bool lockLeftUnwritten(const QString &lockFile)
{
    const QFileInfo file(lockFile);
    qint64 pid = 0;
    QString host;
    QString app;
    if (!file.exists() || (QLockFile(lockFile).getLockInfo(&pid, &host, &app) && pid > 0))
        return false;
    // QLockFile writes the file right after it made it. One still without content after
    // that was left by a process that ended in between.
    const qint64 age = file.lastModified().msecsTo(QDateTime::currentDateTime());
    return age < -kUnwrittenLockMs || age >= kUnwrittenLockMs;
}

#if defined(Q_OS_LINUX) || defined(Q_OS_WIN)
QString legacyEndpoint(const QString &name)
{
    // Older copies hashed the name from the environment, and Qt put a name that is not
    // an absolute path in the temp folder.
    const QString group = name.isEmpty() ? groupName(tokenOf(userName()), false) : name;
#ifdef Q_OS_WIN
    return group;
#else
    return QDir::cleanPath(QDir::tempPath()) + QLatin1Char('/') + group;
#endif
}
#endif

#ifdef Q_OS_UNIX
bool isPrivateDir(const QString &dir, uint uid)
{
    struct stat st{};
    return ::lstat(QFile::encodeName(dir).constData(), &st) == 0 && S_ISDIR(st.st_mode)
        && st.st_uid == uid && (st.st_mode & 07777) == S_IRWXU;
}
#endif

} // namespace mervin::ipc
