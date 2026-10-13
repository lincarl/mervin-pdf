#pragma once

#include <QObject>
#include <QStringList>

#include <memory>
#include <vector>

class QNetworkAccessManager;

namespace mervin::ocr {

// A silent, single-use downloader for first-start OCR setup. The caller must
// persist consumption of the first-start choice before calling start(), so an
// interrupted or failed attempt is never scheduled again by a later process. The
// files are written on the OCR folder's thread (TessdataManager::runInFolder).
class OcrProvisioner final : public QObject
{
    Q_OBJECT

public:
    explicit OcrProvisioner(QObject *parent = nullptr, QNetworkAccessManager *network = nullptr,
                            int inactivityTimeoutMs = 30000);
    ~OcrProvisioner() override;

    // Existing files are replaced only after the best model validates. Each
    // model is requested at most once, even if start() is called repeatedly.
    void start(const QStringList &models, const QString &directory);
    void cancel();

signals:
    void modelInstalled(const QString &model);
    void finished();

private:
    struct Download;
    void request(); // starts the downloads whose staging file exists
    // Hands what has arrived to the staging file, on the OCR folder's thread.
    bool readAvailable(Download &download);
    // A download ended: its file is checked and installed on the folder's thread.
    void complete(Download *download);
    void finishInstall(const QString &model, bool success);
    void abortDownloads();

    QNetworkAccessManager *network_;
    int inactivityTimeoutMs_;
    bool started_ = false;
    bool finished_ = false;
    int installing_ = 0; // completed downloads whose file is still being installed
    std::vector<std::unique_ptr<Download>> downloads_;
};

} // namespace mervin::ocr
