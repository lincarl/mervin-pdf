#include "update/UpdateChecker.h"
#include "update/ReleaseConfig.h"

#include "config/Settings.h"
#include "mervin_version.h"

#include <QAbstractButton>
#include <QApplication>
#include <QByteArray>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMessageBox>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcess>
#include <QProgressDialog>
#include <QPushButton>
#include <QSettings>
#include <QUrl>
#include <QVersionNumber>
#include <QWidget>

namespace mervin {

namespace {

// Public GitHub Releases API for the source repository.
const QString kReleaseApiUrl =
    QStringLiteral(MERVIN_RELEASE_API_URL);

// Don't run the background check more than once per day.
constexpr qint64 kThrottleMs = qint64(24) * 60 * 60 * 1000;

const QByteArray kUserAgent = QByteArrayLiteral("MervinPDF/" MERVIN_VERSION_STRING);

// Strip a leading 'v'/'V' so "v1.9.0" and "1.9.0" compare equal. QVersionNumber
// compares only the numeric segments (any "-beta" suffix is ignored), so keep
// pre-releases out of the "latest" channel rather than relying on ordering here.
QVersionNumber parseVersion(QString s)
{
    s = s.trimmed();
    if (s.startsWith(QLatin1Char('v')) || s.startsWith(QLatin1Char('V')))
        s.remove(0, 1);
    return QVersionNumber::fromString(s);
}

// Dialogs parent to whichever window is active when they are shown (resolved
// late so this works whether triggered from startup or the About dialog).
QWidget *dialogParent()
{
    return QApplication::activeWindow();
}

} // namespace

UpdateChecker::UpdateChecker(QObject *parent)
    : QObject(parent), nam_(new QNetworkAccessManager(this))
{
}

void UpdateChecker::finish()
{
    deleteLater();
}

void UpdateChecker::checkOnStartup()
{
    QSettings st;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const qint64 last = st.value(QStringLiteral("update/lastCheckMs"), qint64(0)).toLongLong();
    if (last != 0 && now - last < kThrottleMs) {
        finish();
        return;
    }
    st.setValue(QStringLiteral("update/lastCheckMs"), now);
    requestLatest(/*manual=*/false);
}

void UpdateChecker::checkManually()
{
    QSettings st;
    st.setValue(QStringLiteral("update/lastCheckMs"), QDateTime::currentMSecsSinceEpoch());
    requestLatest(/*manual=*/true);
}

void UpdateChecker::requestLatest(bool manual)
{
    QNetworkRequest req{QUrl(kReleaseApiUrl)};
    req.setRawHeader("Accept", "application/vnd.github+json");
    req.setRawHeader("X-GitHub-Api-Version", "2022-11-28");
    req.setRawHeader("User-Agent", kUserAgent); // GitHub rejects requests without one
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                     QNetworkRequest::NoLessSafeRedirectPolicy);

    QNetworkReply *reply = nam_->get(req);
    connect(reply, &QNetworkReply::finished, this,
            [this, reply, manual] { handleLatest(reply, manual); });
}

void UpdateChecker::handleLatest(QNetworkReply *reply, bool manual)
{
    reply->deleteLater();

    if (reply->error() != QNetworkReply::NoError) {
        if (manual) {
            QMessageBox::warning(
                dialogParent(), tr("Check for Updates"),
                tr("Couldn't check for updates right now.\n\n%1").arg(reply->errorString()));
        }
        finish();
        return;
    }

    const QJsonObject root = QJsonDocument::fromJson(reply->readAll()).object();
    const QString tag = root.value(QStringLiteral("tag_name")).toString();
    const QString notes = root.value(QStringLiteral("body")).toString();
    const QString pageUrl = root.value(QStringLiteral("html_url")).toString();

    const QVersionNumber latest = parseVersion(tag);
    const QVersionNumber current = parseVersion(QStringLiteral(MERVIN_VERSION_STRING));

    if (tag.isEmpty() || latest.isNull() || latest <= current) {
        if (manual) {
            QMessageBox::information(
                dialogParent(), tr("Check for Updates"),
                tr("You're up to date.\n\nMervin PDF %1 is the latest version.")
                    .arg(QStringLiteral(MERVIN_VERSION_STRING)));
        }
        finish();
        return;
    }

    // Background check only: don't nag about a version the user chose to skip.
    if (!manual) {
        QSettings st;
        if (st.value(QStringLiteral("update/skippedVersion")).toString() == tag) {
            finish();
            return;
        }
    }

    // Locate the Windows NSIS installer asset (per-user, silently installable).
    QString assetUrl;
    QString sha256;
    const QJsonArray assets = root.value(QStringLiteral("assets")).toArray();
    for (const QJsonValue &v : assets) {
        const QJsonObject a = v.toObject();
        const QString name = a.value(QStringLiteral("name")).toString();
        if (name.startsWith(QStringLiteral("MervinPDF-Setup-"), Qt::CaseInsensitive)
            && name.endsWith(QStringLiteral(".exe"), Qt::CaseInsensitive)) {
            assetUrl = a.value(QStringLiteral("browser_download_url")).toString();
            // GitHub exposes a per-asset digest "sha256:<hex>" (since 2025-06).
            const QString digest = a.value(QStringLiteral("digest")).toString();
            if (digest.startsWith(QStringLiteral("sha256:")))
                sha256 = digest.mid(7);
            break;
        }
    }

    offerUpdate(tag, notes, assetUrl, sha256, pageUrl, manual);
}

void UpdateChecker::offerUpdate(const QString &tag, const QString &notes, const QString &assetUrl,
                                const QString &expectedSha256, const QString &releasePageUrl,
                                bool manual)
{
    Q_UNUSED(manual);

    QMessageBox box(dialogParent());
    box.setWindowTitle(tr("Update Available"));
    box.setIcon(QMessageBox::Information);
    box.setTextFormat(Qt::RichText);
    box.setText(tr("<b>Mervin PDF %1 is available.</b><br>You have %2.")
                    .arg(parseVersion(tag).toString(), QStringLiteral(MERVIN_VERSION_STRING)));

    QString detail = notes.trimmed();
    if (detail.size() > 1200)
        detail = detail.left(1200) + QStringLiteral("\n…");
    if (!detail.isEmpty())
        box.setInformativeText(detail);

#ifdef Q_OS_WIN
    const bool canAutoInstall = !assetUrl.isEmpty();
#else
    const bool canAutoInstall = false; // other platforms update via their package manager
#endif

    QPushButton *primary =
        canAutoInstall ? box.addButton(tr("Download && Install"), QMessageBox::AcceptRole)
                       : box.addButton(tr("Open Download Page"), QMessageBox::AcceptRole);
    QPushButton *skipBtn = box.addButton(tr("Skip This Version"), QMessageBox::DestructiveRole);
    box.addButton(tr("Later"), QMessageBox::RejectRole);
    box.setDefaultButton(primary);

    box.exec();
    QAbstractButton *clicked = box.clickedButton();

    if (clicked == primary) {
        if (canAutoInstall) {
            downloadAndRun(assetUrl, expectedSha256, parseVersion(tag).toString());
            return; // downloadAndRun owns the remaining lifecycle
        }
        if (!releasePageUrl.isEmpty())
            QDesktopServices::openUrl(QUrl(releasePageUrl));
    } else if (clicked == skipBtn) {
        QSettings().setValue(QStringLiteral("update/skippedVersion"), tag);
    }
    finish();
}

void UpdateChecker::downloadAndRun(const QString &assetUrl, const QString &expectedSha256,
                                   const QString &version)
{
    auto *progress = new QProgressDialog(tr("Downloading Mervin PDF %1…").arg(version),
                                         tr("Cancel"), 0, 100, dialogParent());
    progress->setWindowTitle(tr("Updating"));
    progress->setWindowModality(Qt::WindowModal);
    progress->setMinimumDuration(0);
    progress->setAutoClose(false);
    progress->setAutoReset(false);
    progress->setValue(0);

    QNetworkRequest req{QUrl(assetUrl)};
    req.setRawHeader("User-Agent", kUserAgent);
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                     QNetworkRequest::NoLessSafeRedirectPolicy);
    QNetworkReply *reply = nam_->get(req);

    connect(reply, &QNetworkReply::downloadProgress, progress,
            [progress](qint64 received, qint64 total) {
                if (total > 0)
                    progress->setValue(static_cast<int>(received * 100 / total));
            });
    connect(progress, &QProgressDialog::canceled, reply, &QNetworkReply::abort);

    connect(reply, &QNetworkReply::finished, this, [this, reply, progress, expectedSha256,
                                                    version] {
        reply->deleteLater();
        progress->close();
        progress->deleteLater();

        if (reply->error() != QNetworkReply::NoError) {
            if (reply->error() != QNetworkReply::OperationCanceledError) { // not a user cancel
                QMessageBox::warning(dialogParent(), tr("Update"),
                                     tr("The update download failed.\n\n%1").arg(reply->errorString()));
            }
            finish();
            return;
        }

        const QByteArray data = reply->readAll();

        if (!expectedSha256.isEmpty()) {
            const QString got = QString::fromLatin1(
                QCryptographicHash::hash(data, QCryptographicHash::Sha256).toHex());
            if (got.compare(expectedSha256, Qt::CaseInsensitive) != 0) {
                QMessageBox::warning(
                    dialogParent(), tr("Update"),
                    tr("The downloaded update failed its integrity check and was discarded."));
                finish();
                return;
            }
        }

        const QString path =
            QDir(QDir::tempPath()).filePath(QStringLiteral("MervinPDF-Setup-%1.exe").arg(version));
        QFile f(path);
        if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)
            || f.write(data) != data.size()) {
            QMessageBox::warning(dialogParent(), tr("Update"),
                                 tr("Couldn't save the update installer."));
            finish();
            return;
        }
        f.close();

        // Let the user save any open work first. If a window vetoes its close
        // (e.g. an unsaved-changes prompt was cancelled), abort the update rather
        // than force it from under them.
        QApplication::closeAllWindows();
        for (QWidget *w : QApplication::topLevelWidgets()) {
            if (w->isWindow() && w->isVisible()) {
                finish();
                return;
            }
        }

        // Silent per-user install (no UAC). The installer force-closes any
        // lingering instance (MessageBox /SD IDRETRY) and relaunches Mervin when
        // done (IfSilent Exec in mervin.nsi). startDetached so it outlives us.
        if (QProcess::startDetached(path, {QStringLiteral("/S")})) {
            QApplication::quit();
        } else {
            QMessageBox::warning(
                nullptr, tr("Update"),
                tr("Couldn't launch the installer automatically. It has been saved to:\n\n%1\n\n"
                   "Please run it to finish updating.")
                    .arg(QDir::toNativeSeparators(path)));
            QApplication::quit();
        }
    });
}

} // namespace mervin
