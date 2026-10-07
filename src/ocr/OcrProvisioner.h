#pragma once

#include <QObject>
#include <QStringList>

#include <memory>
#include <vector>

class QNetworkAccessManager;

namespace mervin::ocr {

// A silent, single-use downloader for first-start OCR setup. The caller must
// persist consumption of the first-start choice before calling start(), so an
// interrupted or failed attempt is never scheduled again by a later process.
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
    bool readAvailable(Download &download);
    bool install(Download &download);
    void complete(Download *download);
    void abortDownloads();

    QNetworkAccessManager *network_;
    int inactivityTimeoutMs_;
    bool started_ = false;
    bool finished_ = false;
    std::vector<std::unique_ptr<Download>> downloads_;
};

} // namespace mervin::ocr
