#include "net/UrlDownload.h"

#include "io/Background.h"

#include <QCoreApplication>
#include <QDir>
#include <QEventLoop>
#include <QFileInfo>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QSaveFile>
#include <QTimer>

#include <utility>

namespace mervin::urlopen {
namespace {

constexpr qint64 kBufferSize = 64 * 1024;

/// Carries a completed worker result without retaining any file or network object.
struct Result
{
    UrlDownload::Error error = UrlDownload::Error::None;
    QString message;
};

/// Owns every filesystem and network object on one worker with bounded buffering.
Result download(const QNetworkRequest &request, const QString &destination,
                const std::atomic<bool> &canceled, const UrlDownload::IoHook &hook,
                const std::function<void(qint64, qint64)> &progress)
{
    using Error = UrlDownload::Error;
    using Phase = UrlDownload::IoPhase;
    QSaveFile output(destination);
    output.setDirectWriteFallback(false);
    const auto before = [&](Phase phase) {
        if (hook)
            hook(output, phase);
        return !canceled.load();
    };
    if (!before(Phase::BeforeDirectory))
        return {Error::Canceled, {}};
    if (!QDir().mkpath(QFileInfo(destination).absolutePath()))
        return {Error::Directory, {}};
    if (!before(Phase::BeforeOpen))
        return {Error::Canceled, {}};
    if (!output.open(QIODevice::WriteOnly))
        return {Error::File, output.errorString()};
    if (canceled.load())
        return {Error::Canceled, {}};

    QNetworkAccessManager network;
    QNetworkReply *reply = network.get(request);
    reply->setReadBufferSize(kBufferSize);
    QEventLoop loop;
    QTimer cancellation;
    cancellation.setInterval(25);
    QObject::connect(&cancellation, &QTimer::timeout, &loop, [&] {
        if (canceled.load())
            reply->abort();
    });
    cancellation.start();
    bool writeFailed = false;
    const auto readAvailable = [&] {
        while (!writeFailed && !canceled.load() && reply->bytesAvailable() > 0) {
            const QByteArray chunk = reply->read(kBufferSize);
            if (chunk.isEmpty())
                break;
            if (!before(Phase::BeforeWrite))
                return;
            if (output.write(chunk) != chunk.size())
                writeFailed = true;
        }
    };
    QObject::connect(reply, &QIODevice::readyRead, &loop, [&] {
        readAvailable();
        if (writeFailed || canceled.load())
            reply->abort();
    });
    QObject::connect(reply, &QNetworkReply::downloadProgress, &loop, progress);
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    if (!reply->isFinished())
        loop.exec();
    cancellation.stop();

    // A finished reply may still contain its last bytes without another readyRead.
    if (reply->error() == QNetworkReply::NoError)
        readAvailable();
    if (canceled.load())
        return {Error::Canceled, {}};
    if (writeFailed)
        return {Error::File, output.errorString()};
    if (reply->error() == QNetworkReply::OperationCanceledError)
        return {Error::Canceled, {}};
    if (reply->error() != QNetworkReply::NoError)
        return {Error::Network, reply->errorString()};
    if (!before(Phase::BeforeCommit))
        return {Error::Canceled, {}};
    if (!output.commit())
        return {Error::File, output.errorString()};
    return {};
}

} // namespace

UrlDownload::UrlDownload(QObject *parent) : QObject(parent)
{
    if (QCoreApplication::instance())
        connect(QCoreApplication::instance(), &QCoreApplication::aboutToQuit,
                this, &UrlDownload::cancel);
}

UrlDownload::~UrlDownload()
{
    canceled_->store(true);
}

void UrlDownload::start(const QNetworkRequest &request, const QString &destination)
{
    if (started_ || finished_)
        return;
    started_ = true;
    const QPointer<QObject> guard(this);
    background::run(
        this,
        [this, guard, request, destination, canceled = canceled_, hook = ioHook_] {
            return download(request, destination, *canceled, hook,
                            [this, guard](qint64 received, qint64 total) {
                                background::post(guard, [this, received, total] {
                                    if (!finished_)
                                        emit downloadProgress(received, total);
                                });
                            });
        },
        [this](Result result) {
            if (finished_)
                return;
            finished_ = true;
            emit finished(result.error, result.message);
        });
}

void UrlDownload::cancel()
{
    if (finished_)
        return;
    finished_ = true;
    canceled_->store(true);
    emit finished(Error::Canceled, {});
}

void UrlDownload::setIoHookForTesting(IoHook hook)
{
    ioHook_ = std::move(hook);
}

} // namespace mervin::urlopen
