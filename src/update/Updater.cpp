#include "update/Updater.h"

#include "config/ConfigPaths.h"
#include "config/Settings.h"
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

namespace mervin {

namespace {

Updater *g_instance = nullptr;

const QByteArray kUserAgent = QByteArrayLiteral("MervinPDF/" MERVIN_VERSION_STRING);

// A stalled transfer (no bytes either way) is abandoned after this long.
constexpr int kTransferTimeoutMs = 30'000;

// Update state in QSettings (redirected into the profile by --profile).
const QString kLastCheckKey = QStringLiteral("update/lastCheckUtc");
const QString kPendingVersionKey = QStringLiteral("update/pendingVersion");
const QString kPendingFileKey = QStringLiteral("update/pendingFile");

// A download in progress is <asset name> plus this suffix until it verifies.
const QString kPartSuffix = QStringLiteral(".part");

// A verified download waiting for Install Now.
struct Pending
{
    QVersionNumber version;
    QString file;

    // Still worth offering: newer than this copy and not deleted since.
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

// Deletes everything in the updates folder except `keep` (interrupted .part
// files, superseded or already installed downloads). An installer that is still
// running stays locked on Windows and goes on a later start.
void removeDownloadsExcept(const QString &keep)
{
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

QDateTime Updater::lastCheck()
{
    return QSettings().value(kLastCheckKey).toDateTime();
}

// The next automatic check is 30 days away. Called on definite
// answers only; after a network error the check stays due and the next start
// retries.
void Updater::markChecked()
{
    const QDateTime now = QDateTime::currentDateTimeUtc();
    QSettings st;
    st.setValue(kLastCheckKey, now);
    st.remove(QStringLiteral("update/startsSinceCheck"));
    emit checked(now);
}

void Updater::onStartup()
{
    // The update state belongs to installed copies. A dev build run against the
    // real settings must neither offer nor delete an installed copy's download.
    if (kind_ == update::PackageKind::None || busy_)
        return;

    const Pending pending = loadPending();
    if (pending.usable()) {
        removeDownloadsExcept(pending.file);
        askWhenIdle();
    } else {
        discardDownloads(); // installed, deleted by hand, or never there
    }

    if (!Settings::load().autoUpdate)
        return;
    if (update::automaticCheckDue(lastCheck(), QDateTime::currentDateTimeUtc()))
        check(/*manual=*/false);
}

void Updater::checkNow()
{
    if (busy_) {
        // A background check or download is already running: let it report to
        // the user from here on instead of starting a second one.
        if (!manual_) {
            manual_ = true;
            if (part_)
                showProgress();
        }
        return;
    }
    check(/*manual=*/true);
}

void Updater::onAutoUpdateChanged(bool on)
{
    if (on || manual_)
        return; // a check the user started from Settings runs to its end
    if (reply_)
        reply_->abort(); // its finished handler removes the .part file
    discardDownloads();
}

void Updater::check(bool manual)
{
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

    const Pending pending = loadPending();
    if (pending.usable() && pending.version >= release->version) {
        // Already downloaded. An automatic check runs after this start's prompt,
        // so only an explicit check asks again.
        markChecked();
        if (manual_)
            askWhenIdle(/*manual=*/true);
        endOperation();
        return;
    }
    download(*asset, release->version.toString());
}

void Updater::download(const update::ReleaseAsset &asset, const QString &version)
{
    asset_ = asset;
    downloadVersion_ = version;
    hash_.reset();
    part_ = new QFile(QDir(ConfigPaths::updatesDir()).filePath(asset.name + kPartSuffix),
                      this);
    if (!part_->open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (manual_)
            //: %1 is the file error message.
            warn(tr("Couldn't save the update.\n\n%1").arg(part_->errorString()));
        delete part_;
        part_ = nullptr;
        endOperation();
        return;
    }

    QNetworkRequest req{QUrl(asset.url)};
    req.setRawHeader("User-Agent", kUserAgent);
    req.setTransferTimeout(kTransferTimeoutMs);
    QNetworkReply *reply = nam_->get(req);
    reply_ = reply;
    // Stream to disk and hash on the way: the images run to ~90 MB.
    connect(reply, &QNetworkReply::readyRead, this, [this, reply] {
        const QByteArray chunk = reply->readAll();
        hash_.addData(chunk);
        if (part_->write(chunk) != chunk.size())
            reply->abort(); // disk full or similar; onDownloadFinished reports it
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
    part_->write(rest);
    part_->flush();
    const bool written = part_->error() == QFileDevice::NoError; // any failed write sticks
    const QString writeError = part_->errorString();
    const QString partPath = part_->fileName();
    part_->close();
    delete part_;
    part_ = nullptr;

    if (!written || reply->error() != QNetworkReply::NoError) {
        QFile::remove(partPath);
        if (manual_ && !written)
            warn(tr("Couldn't save the update.\n\n%1").arg(writeError));
        else if (manual_ && reply->error() != QNetworkReply::OperationCanceledError)
            //: %1 is the network error message.
            warn(tr("The update download failed.\n\n%1").arg(reply->errorString()));
        endOperation();
        return;
    }

    if (!asset_.sha256.isEmpty()
        && QString::fromLatin1(hash_.result().toHex()) != asset_.sha256) {
        QFile::remove(partPath);
        markChecked(); // a bad asset would fail again on the very next start
        if (manual_)
            warn(tr("The downloaded update failed its integrity check and was discarded."));
        endOperation();
        return;
    }

    const QString file = partPath.chopped(kPartSuffix.size());
    QFile::remove(file);
    if (!QFile::rename(partPath, file)) {
        QFile::remove(partPath);
        if (manual_)
            warn(tr("Couldn't save the update."));
        endOperation();
        return;
    }

    // The new download supersedes any older pending one.
    const Pending previous = loadPending();
    if (!previous.file.isEmpty()
        && QFileInfo(previous.file).absoluteFilePath() != QFileInfo(file).absoluteFilePath())
        QFile::remove(previous.file);
    QSettings st;
    st.setValue(kPendingVersionKey, downloadVersion_);
    st.setValue(kPendingFileKey, file);
    markChecked();
    const bool manual = manual_;
    endOperation();
    askWhenIdle(manual);
}

void Updater::askWhenIdle(bool manual)
{
    // Promote an already queued automatic prompt when the user checks from
    // Settings, so it reports back without queuing a second question.
    askManual_ = askManual_ || manual;
    if (askQueued_)
        return;
    askQueued_ = true;
    QTimer::singleShot(0, this, &Updater::pollAsk);
}

void Updater::pollAsk()
{
    // Automatic updates wait for other dialogs. A manual check must be able to
    // finish over its still-open Settings dialog, including after focus changes.
    if ((!askManual_ && QApplication::activeModalWidget())
        || QApplication::activePopupWidget()) {
        QTimer::singleShot(1000, this, &Updater::pollAsk);
        return;
    }
    askQueued_ = false;
    askManual_ = false;
    askToInstall();
}

void Updater::askToInstall()
{
    const Pending pending = loadPending();
    if (!pending.usable())
        return;

    QMessageBox box(dialogParent());
    box.setWindowTitle(tr("Update Ready"));
    box.setIcon(QMessageBox::Information);
    box.setTextFormat(Qt::RichText);
    //: %1 is the version number of the downloaded update.
    box.setText(tr("Mervin PDF %1 is ready to install.").arg(pending.version.toString()));
    //: %1 is the version number of this copy. Never is the button of that name; keep
    //: the <b></b> tags around it.
    box.setInformativeText(tr("You have %1. Click <b>Never</b> to turn off automatic updates.")
                               .arg(QStringLiteral(MERVIN_VERSION_STRING)));
    QPushButton *installButton = box.addButton(tr("Install Now"), QMessageBox::AcceptRole);
    //: Button: ask about the update again on the next start.
    QPushButton *laterButton = box.addButton(tr("Later"), QMessageBox::RejectRole);
    //: Button: discard the update and turn off automatic updates.
    QPushButton *neverButton = box.addButton(tr("Never"), QMessageBox::DestructiveRole);
    box.setDefaultButton(installButton);
    box.setEscapeButton(laterButton); // closing the box is a Later too
    box.exec();

    // Re-read: a newer download may have replaced the file while the box was up.
    const Pending now = loadPending();
    if (box.clickedButton() == installButton && now.usable()) {
        install(now.file, now.version.toString());
    } else if (box.clickedButton() == neverButton) {
        discardDownloads();
        emit autoUpdateDisabled();
    }
    // Later: the download stays and the prompt returns on the next start.
}

void Updater::install(const QString &file, const QString &version)
{
    switch (kind_) {
    case update::PackageKind::Msi:
        // The installer replaces files this process holds open, so the windows
        // close first (each may still save, or cancel the update), then the
        // installer starts detached and this process quits.
        if (!closeWindows())
            return;
        if (!update::startWindowsInstaller(file)) {
            //: %1 is the path of the downloaded installer.
            warn(tr("Couldn't start the installer. It is saved at:\n\n%1\n\n"
                    "Run it to finish updating.")
                     .arg(QDir::toNativeSeparators(file)));
        }
        QApplication::quit();
        return;
    case update::PackageKind::AppImage: {
        QString error;
        if (!update::replaceAppImage(file, &error)) {
            //: %1 is the reason, %2 the path of the downloaded AppImage file.
            warn(tr("Couldn't install the update. %1\n\nThe new AppImage is saved at:\n\n%2")
                     .arg(error, file));
            return;
        }
        restart(qEnvironmentVariable("APPIMAGE"));
        return;
    }
    case update::PackageKind::Deb:
    case update::PackageKind::Rpm:
        installWithPackageManager(file, version);
        return;
    case update::PackageKind::None:
        return;
    }
}

void Updater::installWithPackageManager(const QString &file, const QString &version)
{
    const QStringList command = update::packageInstallCommand(kind_, file);
    if (command.isEmpty()) {
        // No pkexec: hand the package to the desktop's software installer. The
        // next start after it is installed clears the download.
        QDesktopServices::openUrl(QUrl::fromLocalFile(file));
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
        if (ok)
            restart(exePath_);
        else if (!failure.isEmpty())
            //: %1 is the package manager's error output.
            warn(tr("The update could not be installed.\n\n%1").arg(failure));
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
    // The new version is already in place. If the user keeps a window open by
    // cancelling a save prompt, the next launch simply runs the new version.
    if (!closeWindows())
        return;
    update::relaunchAfterExit(exe);
    QApplication::quit();
}

void Updater::endOperation()
{
    busy_ = false;
    manual_ = false;
}

bool Updater::closeWindows()
{
    return closeWindows_ ? closeWindows_() : closeAllWindows();
}
} // namespace mervin
