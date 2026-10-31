#pragma once

#include <QNetworkRequest>
#include <QObject>

#include <atomic>
#include <functional>
#include <memory>

class QSaveFile;

namespace mervin::urlopen {

/// Downloads once on a background worker, replacing the destination atomically.
/// All methods and signals belong to the UI thread. Destruction abandons the
/// worker without waiting for filesystem calls that the OS cannot cancel.
class UrlDownload final : public QObject
{
    Q_OBJECT

public:
    enum class Error { None, Canceled, Directory, File, Network };
    Q_ENUM(Error)

    explicit UrlDownload(QObject *parent = nullptr);
    ~UrlDownload() override;

    /// Creates the destination's folder and starts one bounded streaming download.
    void start(const QNetworkRequest &request, const QString &destination);
    /// Reports cancellation immediately. An already running commit may still finish.
    void cancel();

    enum class IoPhase { BeforeDirectory, BeforeOpen, BeforeWrite, BeforeCommit };
    using IoHook = std::function<void(QSaveFile &, IoPhase)>;
    /// Installs a per-download worker hook before start for storage failure tests.
    void setIoHookForTesting(IoHook hook);

signals:
    void downloadProgress(qint64 received, qint64 total);
    void finished(mervin::urlopen::UrlDownload::Error error, const QString &message);

private:
    bool started_ = false;
    bool finished_ = false;
    IoHook ioHook_;
    std::shared_ptr<std::atomic<bool>> canceled_ = std::make_shared<std::atomic<bool>>(false);
};

} // namespace mervin::urlopen
