#include "ocr/OcrProvisioner.h"

#include "net/UrlOpen.h"
#include "ocr/TessdataFile.h"
#include "ocr/TessdataManager.h"

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
#include <atomic>

namespace mervin::ocr {
namespace {

constexpr qint64 kBufferSize = 64 * 1024;
constexpr qint64 kMaximumModelBytes = 128 * 1024 * 1024;

} // namespace

// The file a download is written to until it validates. Its file is used only on
// the OCR folder's thread (TessdataManager::runInFolder), as the profile can be on a
// share; the UI thread reads `failed` to stop a download that cannot be saved.
struct Staging
{
    QTemporaryFile file;
    std::atomic<bool> failed{false}; // it could not be created or written
};

struct OcrProvisioner::Download
{
    QString model;
    QString target;
    std::shared_ptr<Staging> staging = std::make_shared<Staging>();
    qint64 received = 0; // bytes handed to the staging file
    QPointer<QNetworkReply> reply;
    QTimer timeout;
    bool writeFailed = false;
    // The staging file goes on the folder's thread, after the writes asked for.
    ~Download() { TessdataManager::runInFolder([staging = std::move(staging)] {}); }
};

namespace {

// Validates the complete staging file, then copies it over `target` with QSaveFile,
// which keeps the existing model intact if writing or committing fails.
bool installStaged(Staging &staging, const QString &target)
{
    if (staging.failed.load() || !staging.file.flush()
        || !TessdataFile::validate(staging.file.fileName())
        || !staging.file.seek(0))
        return false;
    QSaveFile output(target);
    output.setDirectWriteFallback(false);
    if (!output.open(QIODevice::WriteOnly))
        return false;
    while (!staging.file.atEnd()) {
        const QByteArray chunk = staging.file.read(kBufferSize);
        if (chunk.isEmpty() || output.write(chunk) != chunk.size())
            return false;
    }
    return output.commit();
}

} // namespace

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
    if (!directory.isEmpty()) {
        for (const QString &model : models) {
            if (!safeModel.match(model).hasMatch() || seen.contains(model))
                continue;
            seen.insert(model);
            auto download = std::make_unique<Download>();
            download->model = model;
            download->target = QDir(directory).filePath(model + QStringLiteral(".traineddata"));
            downloads_.push_back(std::move(download));
        }
    }
    if (downloads_.empty()) {
        finished_ = true;
        emit finished();
        return;
    }

    // The folder and the staging files are made on the OCR folder's thread. Only
    // the downloads that have one start, so an unwritable folder costs no download.
    std::vector<std::shared_ptr<Staging>> stagings;
    for (const auto &download : downloads_)
        stagings.push_back(download->staging);
    TessdataManager::runInFolder(
        [stagings, directory] {
            const bool folder = QDir().mkpath(directory);
            for (const auto &staging : stagings) {
                staging->file.setFileTemplate(
                    QDir(directory).filePath(QStringLiteral(".ocr-download-XXXXXX")));
                staging->failed.store(!folder || !staging->file.open());
            }
        },
        this, [this] { request(); });
}

void OcrProvisioner::request()
{
    if (finished_)
        return; // canceled meanwhile
    std::erase_if(downloads_,
                  [](const auto &download) { return download->staging->failed.load(); });
    for (const auto &entry : downloads_) {
        Download *download = entry.get();
        const QUrl url(QStringLiteral(
            "https://raw.githubusercontent.com/tesseract-ocr/tessdata_best/main/%1.traineddata")
                           .arg(download->model));
        QNetworkRequest get = urlopen::makeRequest(url);
        get.setRawHeader("Accept", QByteArrayLiteral("application/octet-stream"));
        get.setTransferTimeout(inactivityTimeoutMs_);
        get.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                             QNetworkRequest::ManualRedirectPolicy);
        download->reply = network_->get(get);
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
    if (download.writeFailed || download.staging->failed.load())
        return false;
    while (download.reply->bytesAvailable() > 0) {
        const QByteArray chunk = download.reply->read(kBufferSize);
        if (chunk.isEmpty() || download.received + chunk.size() > kMaximumModelBytes) {
            download.writeFailed = true;
            return false;
        }
        download.received += chunk.size();
        TessdataManager::runInFolder([staging = download.staging, chunk] {
            if (!staging->failed.load() && staging->file.write(chunk) != chunk.size())
                staging->failed.store(true);
        });
    }
    return true;
}

void OcrProvisioner::complete(Download *download)
{
    download->timeout.stop();
    const bool received = download->reply->error() == QNetworkReply::NoError
        && download->reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() == 200
        && readAvailable(*download);
    const QString model = download->model;
    const auto installed = std::make_shared<bool>(false);
    ++installing_;
    TessdataManager::runInFolder(
        [staging = download->staging, target = download->target, received, installed,
         canceled = canceled_] {
            *installed = received && !canceled->load() && installStaged(*staging, target);
        },
        this, [this, model, installed] { finishInstall(model, *installed); });
    download->reply->disconnect(this);
    download->reply->deleteLater();
    std::erase_if(downloads_, [download](const auto &entry) { return entry.get() == download; });
}

void OcrProvisioner::finishInstall(const QString &model, bool success)
{
    --installing_;
    if (finished_)
        return; // canceled meanwhile
    const bool allFinished = downloads_.empty() && installing_ == 0;
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
    canceled_->store(true);
    abortDownloads();
    emit finished();
}

} // namespace mervin::ocr
