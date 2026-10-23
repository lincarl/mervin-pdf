#include "update/Updater.h"

#include "config/ConfigPaths.h"
#include "config/Settings.h"
#include "io/Background.h"
#include "mervin_version.h"
#include "update/Installer.h"
#include "update/ReleaseConfig.h"

#include <QApplication>
#include <QDesktopServices>
#include <QDialog>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLabel>
#include <QMainWindow>
#include <QMessageBox>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcess>
#include <QProgressDialog>
#include <QPushButton>
#include <QSettings>
#include <QSysInfo>
#include <QTemporaryDir>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

#include <chrono>
#include <future>
#include <type_traits>

#ifdef Q_OS_WIN
#  define WIN32_LEAN_AND_MEAN
#  define NOMINMAX
#  include <windows.h>
#  include <aclapi.h>
#endif

namespace mervin {

namespace {

Updater *g_instance = nullptr;

// The updater's disk thread: its QSettings state, the updates folder and the
// installation steps, one at a time, in order. Process-wide, so a write asked for
// just before the updater goes still lands.
background::SerialQueue &disk()
{
    static background::SerialQueue queue;
    return queue;
}

std::atomic<int> diskCalls{0}; // see Updater::diskBusy()

// Counts disk work from the request until it, and its completion if any, are done.
struct DiskCall
{
    DiskCall() { ++diskCalls; }
    ~DiskCall() { --diskCalls; }
};

// Queues `job` on the disk thread, without a completion.
void onDiskThread(std::function<void()> job)
{
    disk().add([call = std::make_shared<DiskCall>(), job = std::move(job)] { job(); });
}

const QByteArray kUserAgent = QByteArrayLiteral("MervinPDF/" MERVIN_VERSION_STRING);

// A stalled transfer (no bytes either way) is abandoned after this long.
constexpr int kTransferTimeoutMs = 30'000;

// Update state in QSettings (redirected into the profile by --profile). Copies of
// different package kinds can run for one user, such as an AppImage beside the
// .deb install, and share these settings and the updates folder. So each kind
// keeps its state under update/<kind>/ and its downloads in its own folder, and
// never installs or deletes another kind's.
QString stateKey(update::PackageKind kind, QLatin1StringView name)
{
    return QStringLiteral("update/%1/%2").arg(update::packageKindName(kind), name);
}
constexpr QLatin1StringView kLastCheck("lastCheckUtc");
constexpr QLatin1StringView kPendingVersion("pendingVersion");
constexpr QLatin1StringView kPendingFile("pendingFile");
constexpr QLatin1StringView kPendingSha256("pendingSha256");
// Set when a download fails its check before installing. The check that found
// that download then does not count, so the next start checks again.
constexpr QLatin1StringView kCheckDue("checkDue");

// The state that versions before this shared between all kinds, and that older
// copies still use.
const QString kSharedLastCheckKey = QStringLiteral("update/lastCheckUtc");
const QString kSharedPendingVersionKey = QStringLiteral("update/pendingVersion");
const QString kSharedPendingFileKey = QStringLiteral("update/pendingFile");

// A download in progress is <asset name> plus this suffix until it verifies.
const QString kPartSuffix = QStringLiteral(".part");

// This kind's last check. Until it has one, the date older versions shared, so
// an update to this version keeps the date Settings shows.
QDateTime readLastCheck(update::PackageKind kind)
{
    const QSettings st;
    const QVariant own = st.value(stateKey(kind, kLastCheck));
    return (own.isValid() ? own : st.value(kSharedLastCheckKey)).toDateTime();
}

// Whether `file` is a `kind` package, judged by its name.
bool isPackageOf(update::PackageKind kind, const QString &file)
{
    return update::isPackageFileName(kind, QFileInfo(file).fileName());
}

// A verified download awaiting installation. Windows consumes the pending state
// when handing off to MSI, which can relaunch even after an installation failure.
struct Pending
{
    QVersionNumber version;
    QString file;
    // The asset's SHA-256 from the authenticated release response. Legacy
    // records without a valid digest are discarded and checked again.
    QString sha256;

    // Still worth installing. Newer than this copy and not deleted since.
    bool usable() const
    {
        return update::isNewerStableVersion(version, QStringLiteral(MERVIN_VERSION_STRING))
            && update::isSha256(sha256) && QFileInfo(file).isFile();
    }
};

// This kind's pending update. That is its own record, or else the one an older
// version recorded in the shared keys if that file is this kind's package.
Pending loadPending(update::PackageKind kind)
{
    const QSettings st;
    const Pending pending =
        st.contains(stateKey(kind, kPendingFile))
            ? Pending{update::parseVersion(st.value(stateKey(kind, kPendingVersion)).toString()),
                      st.value(stateKey(kind, kPendingFile)).toString(),
                      st.value(stateKey(kind, kPendingSha256)).toString()}
            : Pending{update::parseVersion(st.value(kSharedPendingVersionKey).toString()),
                      st.value(kSharedPendingFileKey).toString(), {}};
    return isPackageOf(kind, pending.file) ? pending : Pending{};
}

void savePending(update::PackageKind kind, const QString &version, const QString &file,
                 const QString &sha256)
{
    QSettings st;
    st.setValue(stateKey(kind, kPendingVersion), version);
    st.setValue(stateKey(kind, kPendingFile), file);
    st.setValue(stateKey(kind, kPendingSha256), sha256);
}

// Forgets this kind's pending update, including one an older version recorded in
// the shared keys. Another kind's record stays.
void clearPending(update::PackageKind kind, QSettings &st)
{
    st.remove(stateKey(kind, kPendingVersion));
    st.remove(stateKey(kind, kPendingFile));
    st.remove(stateKey(kind, kPendingSha256));
    if (isPackageOf(kind, st.value(kSharedPendingFileKey).toString())) {
        st.remove(kSharedPendingVersionKey);
        st.remove(kSharedPendingFileKey);
    }
}

// Records the pending update again, on the disk thread, after a Windows handoff
// consumed it and then did not happen.
void putBackPending(update::PackageKind kind, const QString &version, const QString &file,
                    const QString &sha256)
{
    onDiskThread([=] { savePending(kind, version, file, sha256); });
}

// The folder in the updates folder that holds this kind's downloads.
QString downloadsDir(update::PackageKind kind)
{
    return QDir(ConfigPaths::updatesDir()).filePath(update::packageKindName(kind));
}

// The SHA-256 of `path` in lower-case hex, or empty when it cannot be read.
QString fileSha256(const QString &path)
{
    QFile file(path);
    QCryptographicHash hash(QCryptographicHash::Sha256);
    if (!file.open(QIODevice::ReadOnly) || !hash.addData(&file))
        return {};
    return QString::fromLatin1(hash.result().toHex());
}

// Keep Windows parent ACLs from granting other users access to staging, even
// when an isolated profile lives in a shared folder. SYSTEM can read MSI inputs.
bool privateStagingDirectory(const QString &path)
{
#ifdef Q_OS_WIN
    const std::wstring native = QDir::toNativeSeparators(path).toStdWString();
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    PSID owner = nullptr;
    if (GetNamedSecurityInfoW(native.c_str(), SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION,
                             &owner, nullptr, nullptr, nullptr, &descriptor) != ERROR_SUCCESS)
        return false;
    BYTE systemSid[SECURITY_MAX_SID_SIZE];
    DWORD systemSize = sizeof(systemSid);
    EXPLICIT_ACCESSW entries[2]{};
    entries[0].grfAccessPermissions = FILE_ALL_ACCESS;
    entries[0].grfAccessMode = SET_ACCESS;
    entries[0].grfInheritance = SUB_CONTAINERS_AND_OBJECTS_INHERIT;
    entries[0].Trustee.TrusteeForm = TRUSTEE_IS_SID;
    entries[0].Trustee.ptstrName = static_cast<LPWSTR>(owner);
    entries[1] = entries[0];
    entries[1].Trustee.ptstrName = reinterpret_cast<LPWSTR>(systemSid);
    PACL acl = nullptr;
    const bool ok = CreateWellKnownSid(WinLocalSystemSid, nullptr, systemSid, &systemSize)
        && SetEntriesInAclW(2, entries, nullptr, &acl) == ERROR_SUCCESS
        && SetNamedSecurityInfoW(const_cast<LPWSTR>(native.c_str()), SE_FILE_OBJECT,
                                 DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
                                 nullptr, nullptr, acl, nullptr) == ERROR_SUCCESS;
    if (acl)
        LocalFree(acl);
    LocalFree(descriptor);
    return ok;
#else
    return QFile::setPermissions(path, QFileDevice::ReadOwner | QFileDevice::WriteOwner
                                      | QFileDevice::ExeOwner);
#endif
}

// Create a new file inheriting only the private directory's access rules. CopyFile
// on Windows can copy the source ACL, so stream bytes instead of QFile::copy.
bool copyToStaging(const QString &sourcePath, const QString &destinationPath)
{
    QFile source(sourcePath);
    QFile destination(destinationPath);
    if (!source.open(QIODevice::ReadOnly)
        || !destination.open(QIODevice::WriteOnly | QIODevice::NewOnly,
                             QFileDevice::ReadOwner | QFileDevice::WriteOwner))
        return false;
    while (!source.atEnd()) {
        const QByteArray bytes = source.read(1024 * 1024);
        if (bytes.isEmpty() || destination.write(bytes) != bytes.size())
            return false;
    }
    return source.error() == QFileDevice::NoError && destination.flush();
}

// The download in progress (<asset>.part), open from the start of the transfer to
// its end. Only disk jobs use it (and tests, while the disk thread is idle), so it
// is never written or closed on the screen thread, even when the operation that
// opened it has ended.
QFile *g_part = nullptr;

void closePart()
{
    delete std::exchange(g_part, nullptr);
}

// Opens `path` as the download in progress, making its folder. Returns why it
// failed, or empty.
QString openPart(const QString &path)
{
    closePart();
    QDir().mkpath(QFileInfo(path).absolutePath());
    auto file = std::make_unique<QFile>(path);
    if (!file->open(QIODevice::WriteOnly | QIODevice::Truncate))
        return file->errorString();
    g_part = file.release();
    return {};
}

// Deletes this kind's downloads except `keep`, such as interrupted .part files
// and superseded or already installed downloads. That is everything in its
// folder, and its packages that older versions saved in the updates folder
// itself. Other kinds' downloads stay. An installer that is still running stays
// locked on Windows and goes on a later start.
void removeDownloadsExcept(update::PackageKind kind, const QString &keep)
{
    closePart(); // an open file cannot be deleted on Windows
    const QString kept = keep.isEmpty() ? QString() : QFileInfo(keep).absoluteFilePath();
    const auto remove = [&kept](const QFileInfo &fi) {
        if (fi.absoluteFilePath() != kept)
            QFile::remove(fi.absoluteFilePath());
    };
    const QDir::Filters files = QDir::Files | QDir::Hidden;
    for (const QFileInfo &fi : QDir(downloadsDir(kind)).entryInfoList(files))
        remove(fi);
    for (const QFileInfo &fi : QDir(ConfigPaths::updatesDir()).entryInfoList(files)) {
        QString name = fi.fileName();
        if (name.endsWith(kPartSuffix))
            name.chop(kPartSuffix.size());
        if (update::isPackageFileName(kind, name))
            remove(fi);
    }
}

void discardDownloads(update::PackageKind kind)
{
    QSettings st;
    clearPending(kind, st);
    removeDownloadsExcept(kind, {});
}

// Keeps this kind's pending update while it is newer than this copy and still
// there, and deletes the rest of this kind's downloads, such as the package this
// copy was updated with. True when an update is kept.
bool tidyDownloads(update::PackageKind kind)
{
    const Pending pending = loadPending(kind);
    if (!pending.usable()) {
        if (!pending.file.isEmpty() && !update::isSha256(pending.sha256)
            && update::isNewerStableVersion(pending.version, QStringLiteral(MERVIN_VERSION_STRING)))
            QSettings().setValue(stateKey(kind, kCheckDue), true);
        discardDownloads(kind); // installed, deleted by hand, or never there
        return false;
    }
    removeDownloadsExcept(kind, pending.file);
    return true;
}

// The active window, or failing that any visible main window, so a prompt that
// appears while Mervin is in the background still belongs to it.
QWidget *dialogParent()
{
    // A screenshot tool or another app can take focus while Settings remains
    // modal. Keep the update dialogs above it even without an active window.
    if (QWidget *w = QApplication::activeModalWidget())
        return w;
    if (QWidget *w = QApplication::activeWindow())
        return w;
    for (QWidget *w : QApplication::topLevelWidgets())
        if (w->isVisible() && qobject_cast<QMainWindow *>(w))
            return w;
    return nullptr;
}

// "Installing..." while pkexec and the package manager run: blocks the windows,
// whose files are being replaced, and cannot be dismissed with Esc.
class BusyNote : public QDialog
{
public:
    BusyNote(const QString &text, QWidget *parent)
        : QDialog(parent, Qt::Dialog | Qt::CustomizeWindowHint | Qt::WindowTitleHint)
    {
        //: Window title (noun): the software update.
        setWindowTitle(Updater::tr("Update"));
        setWindowModality(Qt::ApplicationModal);
        setMinimumWidth(420);
        auto *layout = new QVBoxLayout(this);
        layout->addWidget(new QLabel(text, this));
    }
    void reject() override {} // Esc; closed by the updater when the install ends
};

void warn(const QString &text)
{
    //: Window title (noun): the software update.
    QMessageBox::warning(dialogParent(), Updater::tr("Update"), text);
}

// Tells a manual check that it got no answer it could use.
void warnCheckFailed(const QString &reason)
{
    //: %1 says why, such as a network error message.
    warn(Updater::tr("Couldn't check for updates right now.\n\n%1").arg(reason));
}

void warnInstallerNotStarted(const QString &file)
{
    //: %1 is the path of the downloaded installer.
    warn(Updater::tr("Couldn't start the installer. It is saved at:\n\n%1\n\n"
                     "Run it to finish updating.")
             .arg(QDir::toNativeSeparators(file)));
}

// Closes every window the way the user would, so unsaved work gets its prompt.
// Declined when one stayed open (the user cancelled a save prompt).
Updater::CloseResult closeAllWindows()
{
    QApplication::closeAllWindows();
    for (QWidget *w : QApplication::topLevelWidgets())
        if (w->isWindow() && w->isVisible())
            return Updater::CloseResult::Declined;
    return Updater::CloseResult::Closed;
}

} // namespace

Updater::Updater(QObject *parent)
    : QObject(parent)
    , kind_(update::installedPackageKind())
    , exePath_(QCoreApplication::applicationFilePath())
    , nam_(new QNetworkAccessManager(this))
{
    g_instance = this;
    onDisk(
        [kind = kind_] { return readLastCheck(kind); },
        [this](const QDateTime &utc) {
            if (lastCheckKnown_)
                return; // a check has set it meanwhile
            lastCheck_ = utc;
            lastCheckKnown_ = true;
            emit lastCheckChanged(utc);
        },
        /*forOperation=*/false);
}

template <class Work, class Done>
void Updater::onDisk(Work work, Done done, bool forOperation)
{
    disk().add([guard = QPointer<QObject>(this), operation = operation_, forOperation,
                call = std::make_shared<DiskCall>(), work = std::move(work),
                done = std::move(done)]() mutable {
        const auto deliver = [guard, operation, forOperation, call](auto fn) {
            background::post(guard, [guard, operation, forOperation, call, fn]() mutable {
                if (!forOperation || static_cast<Updater *>(guard.data())->operation_ == operation)
                    fn();
            });
        };
        if constexpr (std::is_void_v<std::invoke_result_t<Work &>>) {
            work();
            deliver([done] { done(); });
        } else {
            deliver([done, result = work()] { done(result); });
        }
    });
}

Updater::~Updater()
{
    clearStaging();
    if (g_instance == this)
        g_instance = nullptr;
}

Updater *Updater::instance()
{
    return g_instance;
}

bool Updater::diskBusy()
{
    return diskCalls.load() > 0;
}

bool Updater::waitForDisk(int ms)
{
    auto reached = std::make_shared<std::promise<void>>();
    std::future<void> done = reached->get_future();
    disk().add([reached] { reached->set_value(); });
    return done.wait_for(std::chrono::milliseconds(ms)) == std::future_status::ready;
}

bool Updater::openPartForTesting(const QString &path)
{
    return openPart(path).isEmpty();
}

QDateTime Updater::lastCheck()
{
    return g_instance ? g_instance->lastCheck_ : QDateTime();
}

// This kind's next automatic check is 30 days away. A definite answer or a
// Windows installer handoff resets the interval, and clears kCheckDue. A network
// error or an answer that cannot be read leaves the check due so the next start
// retries.
void Updater::markChecked()
{
    const QDateTime now = QDateTime::currentDateTimeUtc();
    lastCheck_ = now;
    lastCheckKnown_ = true;
    onDiskThread([kind = kind_, now] {
        QSettings st;
        st.setValue(stateKey(kind, kLastCheck), now);
        st.remove(stateKey(kind, kCheckDue));
        st.remove(QStringLiteral("update/startsSinceCheck"));
    });
    emit checked(now);
    emit lastCheckChanged(now);
}

void Updater::onStartup()
{
    // The update state belongs to installed copies. A dev build run against the
    // real settings must neither offer nor delete an installed copy's download.
    if (!canSelfUpdate() || busy_)
        return;
    if (!Settings::load().autoUpdate) {
        // Nothing installs on its own, but a download that is no longer newer,
        // such as the package of the update that just finished, goes.
        onDiskThread([kind = kind_] { tidyDownloads(kind); });
        return;
    }

    busy_ = true;
    struct Start
    {
        bool resume; // a pending update of this kind is kept
        QDateTime lastCheck;
        bool checkDue; // see kCheckDue
    };
    onDisk(
        [kind = kind_] {
            const bool resume = tidyDownloads(kind);
            const bool checkDue = QSettings().value(stateKey(kind, kCheckDue)).toBool();
            return Start{resume, readLastCheck(kind), checkDue};
        },
        [this](const Start &start) {
            busy_ = false;
            if (!lastCheckKnown_) {
                lastCheck_ = start.lastCheck;
                lastCheckKnown_ = true;
                emit lastCheckChanged(lastCheck_);
            }
            // Check for Updates pressed meanwhile made this operation manual.
            if (start.resume) {
                installWhenIdle();
                return;
            }
            if (manual_ || start.checkDue
                || update::automaticCheckDue(lastCheck_, QDateTime::currentDateTimeUtc()))
                check(manual_);
        });
}

void Updater::checkNow()
{
    if (kind_ == update::PackageKind::Msix) {
        QDesktopServices::openUrl(QUrl(QStringLiteral("ms-windows-store://downloadsandupdates")));
        return;
    }
    if (busy_) {
        // Take over background work, including an installation waiting behind
        // Settings, without starting a second check or installer.
        if (!manual_) {
            manual_ = true;
            if (reply_ && partFailed_) // downloading
                showProgress();
        }
        return;
    }
    check(/*manual=*/true);
}

void Updater::onAutoUpdateChanged(bool on)
{
    if (kind_ == update::PackageKind::Msix)
        return;
    if (on || manual_ || installing_)
        return;
    installQueued_ = false;
    if (reply_)
        reply_->abort(); // its finished handler removes the .part file
    else
        endOperation();
    onDiskThread([kind = kind_] { discardDownloads(kind); });
}

void Updater::check(bool manual)
{
    if (kind_ == update::PackageKind::Msix)
        return;
    busy_ = true;
    manual_ = manual;
    QNetworkRequest req{QUrl(QStringLiteral(MERVIN_RELEASE_API_URL))};
    req.setRawHeader("Accept", "application/vnd.github+json");
    req.setRawHeader("X-GitHub-Api-Version", "2022-11-28");
    req.setRawHeader("User-Agent", kUserAgent); // GitHub rejects requests without one
    req.setTransferTimeout(kTransferTimeoutMs);
    QNetworkReply *reply = nam_->get(req);
    reply_ = reply;
    connect(reply, &QNetworkReply::finished, this, [this, reply] { onReleaseReply(reply); });
}

void Updater::onReleaseReply(QNetworkReply *reply)
{
    reply->deleteLater();
    reply_ = nullptr;

    if (reply->error() != QNetworkReply::NoError) {
        if (manual_ && reply->error() != QNetworkReply::OperationCanceledError)
            warnCheckFailed(reply->errorString());
        endOperation();
        return;
    }

    const update::ReleaseResponse response = update::parseRelease(reply->readAll());
    if (!response.understood) {
        // Not a release at all, such as a network filter's page sent with HTTP 200.
        // Like a network error, it leaves the automatic check due.
        if (manual_) {
            //: Shown below "Couldn't check for updates right now." when the update
            //: server's answer was not release information, such as a web page
            //: from a network filter.
            warnCheckFailed(tr("The update server's answer couldn't be read."));
        }
        endOperation();
        return;
    }
    const std::optional<update::Release> &release = response.release;
    if (!release || !update::isNewerStableVersion(release->version,
                                                 QStringLiteral(MERVIN_VERSION_STRING))) {
        markChecked();
        if (manual_) {
            QMessageBox::information(dialogParent(), tr("Check for Updates"),
                                     //: %1 is the version number of this copy.
                                     tr("You're up to date.\n\nMervin PDF %1 is the latest version.")
                                         .arg(QStringLiteral(MERVIN_VERSION_STRING)));
        }
        endOperation();
        return;
    }

    releasePageUrl_ = release->pageUrl;
    const std::optional<update::ReleaseAsset> asset =
        update::assetFor(release->assets, kind_, QSysInfo::productVersion());
    if (!asset) {
        // This copy can't install anything itself: point at the release page.
        markChecked();
        offerManualDownload(release->version.toString(), release->pageUrl);
        return;
    }

    onDisk(
        [kind = kind_] {
            const Pending pending = loadPending(kind);
            return std::pair(pending.usable(), pending.version);
        },
        [this, asset = *asset, version = release->version](
            const std::pair<bool, QVersionNumber> &pending) {
            if (pending.first && pending.second >= version) {
                // Resume the verified download without fetching the same package again.
                markChecked();
                installWhenIdle();
                return;
            }
            download(asset, version.toString());
        });
}

void Updater::download(const update::ReleaseAsset &asset, const QString &version)
{
    asset_ = asset;
    downloadVersion_ = version;
    hash_.reset();
    // If the operation ends while the file opens, the file stays open until the
    // discard that follows closes it on the disk thread.
    onDisk(
        [kind = kind_, name = asset.name] {
            return openPart(QDir(downloadsDir(kind)).filePath(name + kPartSuffix));
        },
        [this](const QString &error) {
            if (!error.isEmpty()) {
                if (manual_) {
                    //: %1 is the file error message.
                    warn(tr("Couldn't save the update.\n\n%1").arg(error));
                }
                endOperation();
                return;
            }
            partFailed_ = std::make_shared<std::atomic<bool>>(false);
            startDownload();
        });
}

void Updater::startDownload()
{
    const update::ReleaseAsset &asset = asset_;
    QNetworkRequest req{QUrl(asset.url)};
    req.setRawHeader("User-Agent", kUserAgent);
    req.setTransferTimeout(kTransferTimeoutMs);
    QNetworkReply *reply = nam_->get(req);
    reply_ = reply;
    // Stream to disk and hash on the way: the images run to ~90 MB.
    connect(reply, &QNetworkReply::readyRead, this, [this, reply] {
        const QByteArray chunk = reply->readAll();
        hash_.addData(chunk);
        if (partFailed_->load()) {
            reply->abort(); // disk full or similar; onDownloadFinished reports it
            return;
        }
        onDiskThread([failed = partFailed_, chunk] {
            if (!failed->load() && (!g_part || g_part->write(chunk) != chunk.size()))
                failed->store(true);
        });
    });
    connect(reply, &QNetworkReply::finished, this, &Updater::onDownloadFinished);
    if (manual_)
        showProgress();
}

void Updater::showProgress()
{
    //: %1 is the version number being downloaded.
    progress_ = new QProgressDialog(tr("Downloading Mervin PDF %1…").arg(downloadVersion_),
                                    tr("Cancel"), 0, 100, dialogParent());
    //: Window title (noun): the software update.
    progress_->setWindowTitle(tr("Update"));
    progress_->setWindowModality(Qt::WindowModal);
    progress_->setMinimumWidth(420);
    progress_->setMinimumDuration(0);
    progress_->setAutoClose(false);
    progress_->setAutoReset(false);
    progress_->setValue(0);
    QProgressDialog *dialog = progress_;
    connect(reply_, &QNetworkReply::downloadProgress, dialog, [dialog](qint64 received, qint64 total) {
        if (total > 0)
            dialog->setValue(static_cast<int>(received * 100 / total));
    });
    connect(dialog, &QProgressDialog::canceled, reply_, &QNetworkReply::abort);
}

void Updater::onDownloadFinished()
{
    QNetworkReply *reply = reply_;
    reply_ = nullptr;
    reply->deleteLater();
    if (progress_) {
        // close() emits canceled(), which is wired to abort the reply. This is
        // normal completion; only an explicit cancellation should abort it.
        progress_->hide();
        progress_->deleteLater();
        progress_ = nullptr;
    }

    const QByteArray rest = reply->readAll(); // normally empty: readyRead took it all
    hash_.addData(rest);
    const bool received = reply->error() == QNetworkReply::NoError;
    const QString networkError = reply->errorString();
    const bool canceled = reply->error() == QNetworkReply::OperationCanceledError;
    const bool verified = update::isSha256(asset_.sha256)
        && QString::fromLatin1(hash_.result().toHex()) == asset_.sha256.toLower();

    // The rest of the file, then the verdict, on the disk thread.
    enum class Outcome { Saved, WriteFailed, NetworkFailed, BadHash, RenameFailed };
    struct Finish
    {
        Outcome outcome;
        QString writeError;
    };
    const std::shared_ptr<std::atomic<bool>> failed = std::exchange(partFailed_, nullptr);
    onDisk(
        [failed, rest, received, verified, kind = kind_, version = downloadVersion_,
         sha256 = asset_.sha256] {
            const std::unique_ptr<QFile> part(std::exchange(g_part, nullptr));
            if (!part)
                return Finish{Outcome::WriteFailed, {}};
            if (!failed->load() && part->write(rest) != rest.size())
                failed->store(true);
            part->flush();
            // Any failed write sticks.
            const bool written = !failed->load() && part->error() == QFileDevice::NoError;
            const QString writeError = part->errorString();
            const QString partPath = part->fileName();
            part->close();
            if (!written || !received) {
                QFile::remove(partPath);
                return Finish{written ? Outcome::NetworkFailed : Outcome::WriteFailed, writeError};
            }
            if (!verified) {
                QFile::remove(partPath);
                return Finish{Outcome::BadHash, {}};
            }
            const QString file = partPath.chopped(kPartSuffix.size());
            QFile::remove(file);
            if (!QFile::rename(partPath, file)) {
                QFile::remove(partPath);
                return Finish{Outcome::RenameFailed, {}};
            }
            // The new download supersedes any older pending one of this kind.
            const Pending previous = loadPending(kind);
            if (!previous.file.isEmpty()
                && QFileInfo(previous.file).absoluteFilePath() != QFileInfo(file).absoluteFilePath())
                QFile::remove(previous.file);
            QSettings st;
            clearPending(kind, st);
            savePending(kind, version, file, sha256);
            return Finish{Outcome::Saved, {}};
        },
        [this, networkError, canceled](const Finish &finish) {
            switch (finish.outcome) {
            case Outcome::WriteFailed:
                if (manual_)
                    warn(tr("Couldn't save the update.\n\n%1").arg(finish.writeError));
                endOperation();
                return;
            case Outcome::NetworkFailed:
                if (manual_ && !canceled) {
                    //: %1 is the network error message.
                    warn(tr("The update download failed.\n\n%1").arg(networkError));
                }
                endOperation();
                return;
            case Outcome::BadHash:
                markChecked(); // a bad asset would fail again on the very next start
                if (manual_)
                    warn(tr("The downloaded update failed its integrity check and was discarded."));
                endOperation();
                return;
            case Outcome::RenameFailed:
                if (manual_)
                    warn(tr("Couldn't save the update."));
                endOperation();
                return;
            case Outcome::Saved:
                markChecked();
                installWhenIdle();
                return;
            }
        });
}

void Updater::installWhenIdle()
{
    if (installQueued_)
        return;
    busy_ = true;
    installQueued_ = true;
    QTimer::singleShot(0, this, &Updater::pollInstall);
}

void Updater::pollInstall()
{
    if (!installQueued_)
        return;
    if (!manual_ && !Settings::load().autoUpdate) {
        onAutoUpdateChanged(false);
        return;
    }
    if (shouldWaitForDialogs()) {
        QTimer::singleShot(1000, this, &Updater::pollInstall);
        return;
    }
    installQueued_ = false;
    onDisk(
        [kind = kind_]() -> std::optional<Pending> {
            const Pending pending = loadPending(kind);
            // Checking the cached file can block on the profile's filesystem.
            return pending.usable() ? std::optional(pending) : std::nullopt;
        },
        [this](const std::optional<Pending> &pending) {
            if (!pending) {
                // Metadata written by old copies must never bypass verification.
                // Tidy discards it and forces a fresh authenticated release check.
                onDisk([kind = kind_] { tidyDownloads(kind); }, [this] { endOperation(); });
                return;
            }
            if (shouldWaitForDialogs()) {
                installWhenIdle();
                return;
            }
            install(pending->file, pending->version.toString(), pending->sha256);
        });
}

// Automatic updates wait for other dialogs. A manual check must be able to
// finish over its still-open Settings dialog, including after focus changes.
bool Updater::shouldWaitForDialogs() const
{
    return (!manual_ && QApplication::activeModalWidget()) || QApplication::activePopupWidget();
}

void Updater::install(const QString &file, const QString &version, const QString &sha256)
{
    // The last guard. Whatever the record says, never install another kind's
    // package, such as a .deb copied over this AppImage.
    if (!isPackageOf(kind_, file)) {
        endOperation();
        return;
    }
    struct Staged
    {
        std::shared_ptr<QTemporaryDir> directory;
        QString file;
        bool badHash = false;
    };
    onDisk(
        [kind = kind_, file, sha256] {
            // QTemporaryDir creates an owner-only directory. Copy first, then
            // hash that independent copy, never a path later reopened for copying.
            // Same-user code that can modify this private directory or our process
            // is outside this boundary.
            Staged staged;
            staged.directory = std::shared_ptr<QTemporaryDir>(
                new QTemporaryDir(QDir(downloadsDir(kind)).filePath(QStringLiteral(".install-XXXXXX"))),
                [](QTemporaryDir *directory) {
                    onDiskThread([directory] { delete directory; });
                });
            if (!staged.directory->isValid() || !privateStagingDirectory(staged.directory->path()))
                return staged;
            staged.file = staged.directory->filePath(QFileInfo(file).fileName());
            if (!copyToStaging(file, staged.file)) {
                staged.file.clear();
                return staged;
            }
            if (!update::isSha256(sha256) || fileSha256(staged.file) != sha256.toLower()) {
                QSettings().setValue(stateKey(kind, kCheckDue), true);
                discardDownloads(kind);
                staged.badHash = true;
                staged.file.clear();
            }
            return staged;
        },
        [this, file, version, sha256](const Staged &staged) {
            if (staged.file.isEmpty()) {
                if (manual_) {
                    if (staged.badHash)
                        warn(tr("The downloaded update failed its integrity check and was discarded."));
                    else
                        warn(tr("Couldn't save the update."));
                }
                endOperation();
                return;
            }
            if (shouldWaitForDialogs()) {
                installing_ = false;
                installWhenIdle();
                return;
            }
            staging_ = staged.directory;
            stagedFile_ = staged.file;
            installing_ = true;
            installStaged(file, version, sha256);
        });
}

void Updater::installStaged(const QString &file, const QString &version, const QString &sha256)
{
    switch (kind_) {
    case update::PackageKind::Msi:
        // The helper relaunches even if MSI fails or is cancelled, so this attempt
        // is consumed before the handoff, and only once that is written. The old
        // app must not enter an install/relaunch loop. The helper keeps staging
        // until MSI exits, then removes it. Startup removes the cached download.
        onDisk(
            [kind = kind_] {
                QSettings st;
                clearPending(kind, st);
                st.sync();
                return st.status() == QSettings::NoError;
            },
            [this, file, version, sha256](bool consumed) {
                if (!consumed) {
                    putBackPending(kind_, version, file, sha256);
                    warnInstallerNotStarted(file);
                    endOperation();
                    return;
                }
                if (shouldWaitForDialogs()) { // one opened meanwhile
                    putBackPending(kind_, version, file, sha256);
                    installing_ = false;
                    clearStaging();
                    installWhenIdle();
                    return;
                }
                handOffToInstaller(file, version, sha256);
            });
        return;
    case update::PackageKind::AppImage:
        // A copy of about 90 MB, beside the running image.
        onDisk(
            [staged = stagedFile_, staging = staging_, sha256] {
                QString error;
                return update::replaceAppImage(staged, sha256, &error) ? QString() : error;
            },
            [this, file](const QString &error) {
                if (!error.isEmpty()) {
                    //: %1 is the reason, %2 the path of the downloaded AppImage file.
                    warn(tr("Couldn't install the update. %1\n\nThe new AppImage is saved at:\n\n%2")
                             .arg(error, file));
                    endOperation();
                    return;
                }
                restart(qEnvironmentVariable("APPIMAGE"));
            });
        return;
    case update::PackageKind::Deb:
    case update::PackageKind::Rpm:
        installWithPackageManager(file, version);
        return;
    case update::PackageKind::None:
    case update::PackageKind::Msix:
        endOperation();
        return;
    }
}

// The installer replaces files this process holds open, so the windows close
// first (each may still save, or cancel the update), then the installer starts
// detached and this process quits, all in this one call. A dialog's event loop
// that ran in between would delete the closed windows under the dialog. The
// pending update was consumed before; it is put back if the handoff fails.
void Updater::handOffToInstaller(const QString &file, const QString &version,
                                 const QString &sha256)
{
    if (const CloseResult closed = closeWindows(); closed != CloseResult::Closed) {
        putBackPending(kind_, version, file, sha256);
        reportCloseRefused(closed);
        endOperation();
        return;
    }
    if (!update::startWindowsInstaller(stagedFile_)) {
        putBackPending(kind_, version, file, sha256);
        warnInstallerNotStarted(file);
        endOperation();
        QApplication::quit();
        return;
    }
    // The helper removes its private directory after msiexec exits. The app may
    // now quit without deleting bytes the detached installer has yet to open.
    staging_->setAutoRemove(false);
    clearStaging();
    markChecked(); // also defers retries of downloads kept over 30 days
    QApplication::quit();
}

void Updater::installWithPackageManager(const QString &file, const QString &version)
{
    // Finding pkexec and the package manager looks through PATH.
    onDisk([kind = kind_, staged = stagedFile_] { return update::packageInstallCommand(kind, staged); },
           [this, file, version](const QStringList &command) {
               if (shouldWaitForDialogs()) { // one opened meanwhile
                   installing_ = false;
                   clearStaging();
                   installWhenIdle();
                   return;
               }
               runPackageManager(command, file, version);
           });
}

void Updater::runPackageManager(const QStringList &command, const QString &file,
                                const QString &version)
{
    if (command.isEmpty()) {
        // A desktop URL launch provides no completion handle. Keep the pending
        // download for retry and let manual checks offer the release page instead.
        Q_UNUSED(file);
        // Startup can resume a cached package without fetching release metadata.
        const QString pageUrl = releasePageUrl_.isEmpty()
            ? QStringLiteral("https://github.com/lincarl/mervin-pdf/releases/latest")
            : releasePageUrl_;
        offerManualDownload(version, pageUrl);
        return;
    }

    // pkexec asks for the password in its own window.
    //: %1 is the version number being installed.
    auto *note = new BusyNote(tr("Installing Mervin PDF %1…").arg(version), dialogParent());
    note->show();

    auto *proc = new QProcess(this);
    proc->setProcessChannelMode(QProcess::MergedChannels);
    const auto done = [this, proc, note, staging = staging_](bool ok, const QString &failure) {
        proc->deleteLater();
        note->hide(); // not close(): that goes through the disabled reject()
        note->deleteLater();
        clearStaging();
        if (ok) {
            restart(exePath_);
        } else {
            if (!failure.isEmpty())
                //: %1 is the package manager's error output.
                warn(tr("The update could not be installed.\n\n%1").arg(failure));
            endOperation();
        }
    };
    // Completion can show a modal warning. Wait until QProcess has unwound its
    // signal emission before that event loop can deliver its deferred deletion.
    connect(proc, &QProcess::finished, this, [proc, done](int code, QProcess::ExitStatus status) {
        if (status == QProcess::NormalExit && code == 0)
            return done(true, {});
        // 126: the password prompt was dismissed. The user knows; say nothing.
        if (status == QProcess::NormalExit && code == 126)
            return done(false, {});
        const QString output = QString::fromLocal8Bit(proc->readAll()).trimmed();
        done(false, output.right(600));
    }, Qt::QueuedConnection);
    connect(proc, &QProcess::errorOccurred, this, [proc, done](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart)
            done(false, proc->errorString());
    }, Qt::QueuedConnection);
    proc->start(command.first(), command.mid(1));
}

void Updater::restart(const QString &exe)
{
    // The new version is already in place, so an automatic update can wait here
    // for a dialog opened meanwhile, as during the AppImage copy. If a window
    // stays open, the next launch simply runs the new version.
    if (shouldWaitForDialogs()) {
        QTimer::singleShot(1000, this, [this, exe] { restart(exe); });
        return;
    }
    if (const CloseResult closed = closeWindows(); closed != CloseResult::Closed) {
        reportCloseRefused(closed);
        endOperation();
        return;
    }
    update::relaunchAfterExit(exe);
    QApplication::quit();
}

void Updater::clearStaging()
{
    staging_.reset();
    stagedFile_.clear();
}

void Updater::offerManualDownload(const QString &version, const QString &pageUrl)
{
    const bool manual = manual_;
    endOperation();
    if (!manual)
        return;
    QMessageBox box(dialogParent());
    box.setWindowTitle(tr("Update Available"));
    box.setIcon(QMessageBox::Information);
    //: %1 is the version number of the new release.
    box.setText(tr("Mervin PDF %1 is available.").arg(version));
    //: %1 is the version number of this copy.
    box.setInformativeText(tr("You have %1.").arg(QStringLiteral(MERVIN_VERSION_STRING)));
    QPushButton *open = box.addButton(tr("Open Download Page"), QMessageBox::AcceptRole);
    box.addButton(QMessageBox::Close);
    box.setDefaultButton(open);
    box.exec();
    if (box.clickedButton() == open && QUrl(pageUrl).scheme() == QLatin1String("https"))
        QDesktopServices::openUrl(QUrl(pageUrl));
}

void Updater::endOperation()
{
    clearStaging();
    ++operation_; // what its disk work answers no longer applies
    busy_ = false;
    manual_ = false;
    installQueued_ = false;
    installing_ = false;
}

Updater::CloseResult Updater::closeWindows()
{
    return closeWindows_ ? closeWindows_() : closeAllWindows();
}

void Updater::reportCloseRefused(CloseResult result) const
{
    if (result == CloseResult::Busy && manual_)
        warn(closeRefusedText());
}

QString Updater::closeRefusedText()
{
    //: Shown over the Settings window after Check for Updates downloaded an
    //: update, when Mervin could not close to install it or to restart into it,
    //: because an operation such as saving a file was still running or another
    //: dialog was open. A save chosen in the save prompt only starts once Settings
    //: closes, so the user closes Settings first. The download is kept. Settings
    //: is that window's title, and Check for Updates is its button in Settings >
    //: Updates; use their translations.
    return tr("Couldn't close Mervin to complete the update.\n\n"
              "Close Settings and other dialogs and wait for running operations to "
              "finish. Then choose Check for Updates again.");
}
} // namespace mervin
