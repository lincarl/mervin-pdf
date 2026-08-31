#pragma once

#include <QObject>
#include <QString>

QT_BEGIN_NAMESPACE
class QNetworkAccessManager;
class QNetworkReply;
QT_END_NAMESPACE

namespace mervin {

// Checks GitHub Releases for a newer Mervin PDF and, with the user's consent,
// downloads and runs the Windows installer silently (per-user, so no UAC) and
// relaunches. Each call is one-shot: it runs a single check and the object
// deletes itself when the flow ends, so callers simply do
//     (new UpdateChecker(qApp))->checkOnStartup();
//
// This reads the public GitHub Releases API for lincarl/mervin-pdf
// (kReleaseApiUrl in the .cpp), so no credentials are required.
class UpdateChecker : public QObject
{
    Q_OBJECT

public:
    explicit UpdateChecker(QObject *parent = nullptr);

    // Opt-in background check: gated by Settings::checkUpdatesOnStartup (the
    // caller checks that) and a 24h throttle, silent about versions the user
    // chose to skip, and never shows anything on error.
    void checkOnStartup();

    // Manual "Check for Updates": ignores the throttle and the skip-list, and
    // tells the user the result either way ("up to date" / "couldn't check").
    void checkManually();

private:
    void requestLatest(bool manual);
    void handleLatest(QNetworkReply *reply, bool manual);
    void offerUpdate(const QString &tag, const QString &notes, const QString &assetUrl,
                     const QString &expectedSha256, const QString &releasePageUrl, bool manual);
    void downloadAndRun(const QString &assetUrl, const QString &expectedSha256,
                        const QString &version);
    void finish(); // deleteLater() - ends this one-shot check

    QNetworkAccessManager *nam_;
};

} // namespace mervin
