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
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

#include <chrono>
#include <future>
#include <type_traits>

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

// Update state in QSettings (redirected into the profile by --profile).
const QString kLastCheckKey = QStringLiteral("update/lastCheckUtc");
const QString kPendingVersionKey = QStringLiteral("update/pendingVersion");
const QString kPendingFileKey = QStringLiteral("update/pendingFile");

// A download in progress is <asset name> plus this suffix until it verifies.
const QString kPartSuffix = QStringLiteral(".part");

QDateTime readLastCheck()
{
    return QSettings().value(kLastCheckKey).toDateTime();
}

// A verified download awaiting installation. Windows consumes the pending state
// when handing off to MSI, which can relaunch even after an installation failure.
struct Pending
{
    QVersionNumber version;
    QString file;

    // Still worth installing. Newer than this copy and not deleted since.
    bool usable() const
    {
        return update::isNewerStableVersion(version, QStringLiteral(MERVIN_VERSION_STRING))
            && QFileInfo(file).isFile();
    }
};

Pending loadPending()
{
    const QSettings st;
    return {update::parseVersion(st.value(kPendingVersionKey).toString()),
            st.value(kPendingFileKey).toString()};
}

void savePending(const QString &version, const QString &file)
{
    QSettings st;
    st.setValue(kPendingVersionKey, version);
    st.setValue(kPendingFileKey, file);
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

// Opens `path` as the download in progress. Returns why it failed, or empty.
QString openPart(const QString &path)
{
    closePart();
    auto file = std::make_unique<QFile>(path);
    if (!file->open(QIODevice::WriteOnly | QIODevice::Truncate))
        return file->errorString();
    g_part = file.release();
    return {};
}

// Deletes everything in the updates folder except `keep` (interrupted .part
// files, superseded or already installed downloads). An installer that is still
// running stays locked on Windows and goes on a later start.
void removeDownloadsExcept(const QString &keep)
{
    closePart(); // an open file cannot be deleted on Windows
    const QString kept = keep.isEmpty() ? QString() : QFileInfo(keep).absoluteFilePath();
    const QDir dir(ConfigPaths::updatesDir());
    for (const QFileInfo &fi : dir.entryInfoList(QDir::Files | QDir::Hidden))
        if (fi.absoluteFilePath() != kept)
            QFile::remove(fi.absoluteFilePath());
}

void discardDownloads()
{
    QSettings st;
    st.remove(kPendingVersionKey);
    st.remove(kPendingFileKey);
    removeDownloadsExcept({});
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

void warnInstallerNotStarted(const QString &file)
{
    //: %1 is the path of the downloaded installer.
    warn(Updater::tr("Couldn't start the installer. It is saved at:\n\n%1\n\n"
                     "Run it to finish updating.")
             .arg(QDir::toNativeSeparators(file)));
}

// Closes every window the way the user would, so unsaved work gets its prompt.
// False when one stayed open (the user cancelled a save prompt).
bool closeAllWindows()
{
    QApplication::closeAllWindows();
    for (QWidget *w : QApplication::topLevelWidgets())
        if (w->isWindow() && w->isVisible())
            return false;
    return true;
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
        readLastCheck,
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

// The next automatic check is 30 days away. A definite answer or a Windows
// installer handoff resets the interval. A network error leaves the check due
// so the next start retries.
void Updater::markChecked()
{
    const QDateTime now = QDateTime::currentDateTimeUtc();
    lastCheck_ = now;
    lastCheckKnown_ = true;
    onDiskThread([now] {
        QSettings st;
        st.setValue(kLastCheckKey, now);
        st.remove(QStringLiteral("update/startsSinceCheck"));
    });
    emit checked(now);
    emit lastCheckChanged(now);
}

void Updater::onStartup()
{
    // The update state belongs to installed copies. A dev build run against the
    // real settings must neither offer nor delete an installed copy's download.
    if (!canSelfUpdate() || busy_ || !Settings::load().autoUpdate)
        return;

    busy_ = true;
    onDisk(
        [] {
            const Pending pending = loadPending();
            const bool resume = pending.usable();
            if (resume)
                removeDownloadsExcept(pending.file);
            else
                discardDownloads(); // installed, deleted by hand, or never there
            return std::pair(resume, readLastCheck());
        },
        [this](const std::pair<bool, QDateTime> &start) {
            busy_ = false;
            if (!lastCheckKnown_) {
                lastCheck_ = start.second;
                lastCheckKnown_ = true;
                emit lastCheckChanged(lastCheck_);
            }
            // Check for Updates pressed meanwhile made this operation manual.
            if (start.first) {
                installWhenIdle();
                return;
            }
            if (manual_ || update::automaticCheckDue(lastCheck_, QDateTime::currentDateTimeUtc()))
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
    onDiskThread(discardDownloads);
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
            //: %1 is the network error message.
            warn(tr("Couldn't check for updates right now.\n\n%1").arg(reply->errorString()));
        endOperation();
        return;
    }

    const std::optional<update::Release> release = update::parseRelease(reply->readAll());
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

    const std::optional<update::ReleaseAsset> asset =
        update::assetFor(release->assets, kind_, QSysInfo::productVersion());
    if (!asset) {
        // This copy can't install anything itself: point at the release page.
        markChecked();
        const bool manual = manual_;
        endOperation();
        if (manual) {
            QMessageBox box(dialogParent());
            box.setWindowTitle(tr("Update Available"));
            box.setIcon(QMessageBox::Information);
            //: %1 is the version number of the new release.
            box.setText(tr("Mervin PDF %1 is available.").arg(release->version.toString()));
            //: %1 is the version number of this copy.
            box.setInformativeText(tr("You have %1.").arg(QStringLiteral(MERVIN_VERSION_STRING)));
            QPushButton *open = box.addButton(tr("Open Download Page"), QMessageBox::AcceptRole);
            box.addButton(QMessageBox::Close);
            box.setDefaultButton(open);
            box.exec();
            if (box.clickedButton() == open && !release->pageUrl.isEmpty())
                QDesktopServices::openUrl(QUrl(release->pageUrl));
        }
        return;
    }

    onDisk(
        [] {
            const Pending pending = loadPending();
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
        [name = asset.name] {
            return openPart(QDir(ConfigPaths::updatesDir()).filePath(name + kPartSuffix));
        },
        [this](const QString &error) {
            if (!error.isEmpty()) {
                if (manual_)
                    //: %1 is the file error message.
                    warn(tr("Couldn't save the update.\n\n%1").arg(error));
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
    const bool verified = asset_.sha256.isEmpty()
        || QString::fromLatin1(hash_.result().toHex()) == asset_.sha256;

    // The rest of the file, then the verdict, on the disk thread.
    enum class Outcome { Saved, WriteFailed, NetworkFailed, BadHash, RenameFailed };
    struct Finish
    {
        Outcome outcome;
        QString writeError;
    };
    const std::shared_ptr<std::atomic<bool>> failed = std::exchange(partFailed_, nullptr);
    onDisk(
        [failed, rest, received, verified, version = downloadVersion_] {
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
            // The new download supersedes any older pending one.
            const Pending previous = loadPending();
            if (!previous.file.isEmpty()
                && QFileInfo(previous.file).absoluteFilePath() != QFileInfo(file).absoluteFilePath())
                QFile::remove(previous.file);
            savePending(version, file);
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
                if (manual_ && !canceled)
                    //: %1 is the network error message.
                    warn(tr("The update download failed.\n\n%1").arg(networkError));
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
        [] {
            const Pending pending = loadPending();
            return pending.usable() ? pending : Pending{};
        },
        [this](const Pending &pending) {
            if (pending.file.isEmpty()) {
                endOperation();
                return;
            }
            if (shouldWaitForDialogs()) { // one opened meanwhile
                installWhenIdle();
                return;
            }
            installing_ = true;
            install(pending.file, pending.version.toString());
        });
}

// Automatic updates wait for other dialogs. A manual check must be able to
// finish over its still-open Settings dialog, including after focus changes.
bool Updater::shouldWaitForDialogs() const
{
    return (!manual_ && QApplication::activeModalWidget()) || QApplication::activePopupWidget();
}

void Updater::install(const QString &file, const QString &version)
{
    switch (kind_) {
    case update::PackageKind::Msi:
        // The helper relaunches even if MSI fails or is cancelled, so this attempt
        // is consumed before the handoff, and only once that is written. The old
        // app must not enter an install/relaunch loop. The file stays until the
        // installer exits and startup cleans it up.
        onDisk(
            [] {
                QSettings st;
                st.remove(kPendingVersionKey);
                st.remove(kPendingFileKey);
                st.sync();
                return st.status() == QSettings::NoError;
            },
            [this, file, version](bool consumed) {
                if (!consumed) {
                    onDiskThread([file, version] { savePending(version, file); });
                    warnInstallerNotStarted(file);
                    endOperation();
                    return;
                }
                if (shouldWaitForDialogs()) { // one opened meanwhile
                    onDiskThread([file, version] { savePending(version, file); });
                    installing_ = false;
                    installWhenIdle();
                    return;
                }
                handOffToInstaller(file, version);
            });
        return;
    case update::PackageKind::AppImage:
        // A copy of about 90 MB, beside the running image.
        onDisk(
            [file] {
                QString error;
                return update::replaceAppImage(file, &error) ? QString() : error;
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
void Updater::handOffToInstaller(const QString &file, const QString &version)
{
    if (!closeWindows()) {
        onDiskThread([file, version] { savePending(version, file); });
        endOperation();
        return;
    }
    if (!update::startWindowsInstaller(file)) {
        onDiskThread([file, version] { savePending(version, file); });
        warnInstallerNotStarted(file);
        endOperation();
        QApplication::quit();
        return;
    }
    markChecked(); // also defers retries of downloads kept over 30 days
    QApplication::quit();
}

void Updater::installWithPackageManager(const QString &file, const QString &version)
{
    // Finding pkexec and the package manager looks through PATH.
    onDisk([kind = kind_, file] { return update::packageInstallCommand(kind, file); },
           [this, file, version](const QStringList &command) {
               if (shouldWaitForDialogs()) { // one opened meanwhile
                   installing_ = false;
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
        // No pkexec: hand the package to the desktop's software installer. The
        // next start after it is installed clears the download.
        QDesktopServices::openUrl(QUrl::fromLocalFile(file));
        endOperation();
        return;
    }

    // pkexec asks for the password in its own window.
    //: %1 is the version number being installed.
    auto *note = new BusyNote(tr("Installing Mervin PDF %1…").arg(version), dialogParent());
    note->show();

    auto *proc = new QProcess(this);
    proc->setProcessChannelMode(QProcess::MergedChannels);
    const auto done = [this, proc, note](bool ok, const QString &failure) {
        proc->deleteLater();
        note->hide(); // not close(): that goes through the disabled reject()
        note->deleteLater();
        if (ok) {
            restart(exePath_);
        } else {
            if (!failure.isEmpty())
                //: %1 is the package manager's error output.
                warn(tr("The update could not be installed.\n\n%1").arg(failure));
            endOperation();
        }
    };
    connect(proc, &QProcess::finished, this, [proc, done](int code, QProcess::ExitStatus status) {
        if (status == QProcess::NormalExit && code == 0)
            return done(true, {});
        // 126: the password prompt was dismissed. The user knows; say nothing.
        if (status == QProcess::NormalExit && code == 126)
            return done(false, {});
        const QString output = QString::fromLocal8Bit(proc->readAll()).trimmed();
        done(false, output.right(600));
    });
    connect(proc, &QProcess::errorOccurred, this, [proc, done](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart)
            done(false, proc->errorString());
    });
    proc->start(command.first(), command.mid(1));
}

void Updater::restart(const QString &exe)
{
    // The new version is already in place, so an automatic update can wait here
    // for a dialog opened meanwhile, as during the AppImage copy. If the user
    // keeps a window open by cancelling a save prompt, the next launch simply
    // runs the new version.
    if (shouldWaitForDialogs()) {
        QTimer::singleShot(1000, this, [this, exe] { restart(exe); });
        return;
    }
    if (!closeWindows()) {
        endOperation();
        return;
    }
    update::relaunchAfterExit(exe);
    QApplication::quit();
}

void Updater::endOperation()
{
    ++operation_; // what its disk work answers no longer applies
    busy_ = false;
    manual_ = false;
    installQueued_ = false;
    installing_ = false;
}

bool Updater::closeWindows()
{
    return closeWindows_ ? closeWindows_() : closeAllWindows();
}
} // namespace mervin
