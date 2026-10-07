#include "ocr/OcrProvisioner.h"

#include "net/UrlOpen.h"
#include "ocr/TessdataFile.h"

#include <QCoreApplication>
#include <QDir>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QPointer>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QTemporaryFile>
#include <QTimer>

#include <algorithm>

namespace mervin::ocr {
namespace {

constexpr qint64 kBufferSize = 64 * 1024;
constexpr qint64 kMaximumModelBytes = 128 * 1024 * 1024;

} // namespace

struct OcrProvisioner::Download
{
    QString model;
    QString target;
    QTemporaryFile staging;
    QPointer<QNetworkReply> reply;
    QTimer timeout;
    bool writeFailed = false;
};

OcrProvisioner::OcrProvisioner(QObject *parent, QNetworkAccessManager *network,
                             int inactivityTimeoutMs)
    : QObject(parent), network_(network ? network : new QNetworkAccessManager(this)),
      inactivityTimeoutMs_(qMax(1, inactivityTimeoutMs))
{
    if (QCoreApplication::instance())
        connect(QCoreApplication::instance(), &QCoreApplication::aboutToQuit,
                this, &OcrProvisioner::cancel);
}

OcrProvisioner::~OcrProvisioner()
{
    abortDownloads();
}

void OcrProvisioner::start(const QStringList &models, const QString &directory)
{
    if (started_ || finished_)
        return;
    started_ = true;

    static const QRegularExpression safeModel(QStringLiteral("^[a-z]{3}(?:_[a-z]+)?$"));
    QSet<QString> seen;
    if (!directory.isEmpty() && QDir().mkpath(directory)) {
        for (const QString &model : models) {
            if (!safeModel.match(model).hasMatch() || seen.contains(model))
                continue;
            seen.insert(model);
            auto download = std::make_unique<Download>();
            download->model = model;
            download->target = QDir(directory).filePath(model + QStringLiteral(".traineddata"));
            download->staging.setFileTemplate(
                QDir(directory).filePath(QStringLiteral(".ocr-download-XXXXXX")));
            if (!download->staging.open())
                continue;
            downloads_.push_back(std::move(download));
        }
    }

    for (const auto &entry : downloads_) {
        Download *download = entry.get();
        const QUrl url(QStringLiteral(
            "https://raw.githubusercontent.com/tesseract-ocr/tessdata_best/main/%1.traineddata")
                           .arg(download->model));
        QNetworkRequest request = urlopen::makeRequest(url);
        request.setRawHeader("Accept", QByteArrayLiteral("application/octet-stream"));
        request.setTransferTimeout(inactivityTimeoutMs_);
        request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                             QNetworkRequest::ManualRedirectPolicy);
        download->reply = network_->get(request);
        download->reply->setReadBufferSize(kBufferSize);
        download->timeout.setSingleShot(true);
        connect(&download->timeout, &QTimer::timeout, this, [download] {
            if (download->reply)
                download->reply->abort();
        });
        connect(download->reply, &QNetworkReply::readyRead, this, [this, download] {
            download->timeout.start(inactivityTimeoutMs_);
            if (!readAvailable(*download))
                download->reply->abort();
        });
        connect(download->reply, &QNetworkReply::finished, this,
                [this, download] { complete(download); });
        download->timeout.start(inactivityTimeoutMs_);
    }
    if (downloads_.empty()) {
        finished_ = true;
        emit finished();
    }
}

bool OcrProvisioner::readAvailable(Download &download)
{
    if (download.writeFailed)
        return false;
    while (download.reply->bytesAvailable() > 0) {
        const QByteArray chunk = download.reply->read(kBufferSize);
        if (chunk.isEmpty() || download.staging.size() + chunk.size() > kMaximumModelBytes
            || download.staging.write(chunk) != chunk.size()) {
            download.writeFailed = true;
            return false;
        }
    }
    return true;
}

bool OcrProvisioner::install(Download &download)
{
    if (!readAvailable(download) || !download.staging.flush()
        || !TessdataFile::validate(download.staging.fileName()) || !download.staging.seek(0))
        return false;

    // QSaveFile keeps the existing model intact if writing or committing fails.
    // Validate the complete staging file before opening the replacement.
    QSaveFile output(download.target);
    output.setDirectWriteFallback(false);
    if (!output.open(QIODevice::WriteOnly))
        return false;
    while (!download.staging.atEnd()) {
        const QByteArray chunk = download.staging.read(kBufferSize);
        if (chunk.isEmpty() || output.write(chunk) != chunk.size())
            return false;
    }
    return output.commit();
}

void OcrProvisioner::complete(Download *download)
{
    download->timeout.stop();
    const bool success = download->reply->error() == QNetworkReply::NoError
        && download->reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() == 200
        && install(*download);
    const QString model = download->model;
    download->reply->disconnect(this);
    download->reply->deleteLater();
    std::erase_if(downloads_, [download](const auto &entry) { return entry.get() == download; });
    const bool allFinished = downloads_.empty();
    if (allFinished)
        finished_ = true;
    if (success)
        emit modelInstalled(model);
    if (allFinished)
        emit finished();
}

void OcrProvisioner::abortDownloads()
{
    for (const auto &download : downloads_) {
        if (download->reply) {
            download->reply->disconnect(this);
            download->reply->abort();
            download->reply->deleteLater();
        }
    }
    downloads_.clear();
}

void OcrProvisioner::cancel()
{
    if (finished_)
        return;
    finished_ = true;
    abortDownloads();
    emit finished();
}

} // namespace mervin::ocr
