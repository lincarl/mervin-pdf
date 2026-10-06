#include "render/Document.h"
#include "render/RenderEngine.h"
#include "render/TextIndex.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSysInfo>
#include <QTimer>
#include <algorithm>
#include <cstdio>
#include <stdexcept>

using namespace mervin;

static QJsonObject timings(std::vector<double> values)
{
    const double first = values.front();
    std::sort(values.begin(), values.end());
    return {{"first_ms", first}, {"p50_ms", values[values.size() / 2]},
            {"p95_ms", values[std::min(values.size() - 1, values.size() * 95 / 100)]}};
}

static QString imageHash(const QImage &image)
{
    QCryptographicHash hash(QCryptographicHash::Sha256);
    for (int y = 0; y < image.height(); ++y)
        hash.addData(QByteArrayView(reinterpret_cast<const char *>(image.constScanLine(y)),
                                   image.width() * image.depth() / 8));
    return QString::fromLatin1(hash.result().toHex());
}

static QJsonObject benchmark(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        throw std::runtime_error(file.errorString().toStdString());
    QCryptographicHash hash(QCryptographicHash::Sha256);
    hash.addData(&file);
    QJsonObject result{{"file", QFileInfo(path).fileName()},
                       {"sha256", QString::fromLatin1(hash.result().toHex())}};
    RenderEngine engine;
    std::vector<double> openTimes;
    std::unique_ptr<Document> doc;
    QElapsedTimer timer;
    for (int run = 0; run < 5; ++run) {
        doc.reset();
        timer.start();
        doc = engine.openDocument(path);
        openTimes.push_back(timer.nsecsElapsed() / 1e6);
        if (!doc || doc->pageCount() == 0)
            throw std::runtime_error("Could not open document");
    }
    result["pages"] = doc->pageCount();
    result["open"] = timings(openTimes);
    QJsonArray renders;
    QList<int> pages{0, doc->pageCount() / 4, doc->pageCount() / 2,
                     doc->pageCount() * 3 / 4, doc->pageCount() - 1};
    pages.erase(std::unique(pages.begin(), pages.end()), pages.end());
    quint64 token = 0;
    for (int page : pages)
        for (bool clipped : {false, true})
            for (auto theme : {PageTheme::Light, PageTheme::Comfort}) {
                std::vector<double> renderTimes;
                QString expectedHash;
                for (int run = 0; run < 5; ++run) {
                    RenderRequest request;
                    request.document = doc.get();
                    request.requester = 1;
                    request.token = ++token;
                    request.pageNo = page;
                    request.theme = theme;
                    request.scale = clipped ? 5.0 : 1.5;
                    if (clipped)
                        request.clip = QRect(150, 200, 1200, 900);
                    RenderResult rendered;
                    QEventLoop loop;
                    QTimer timeout;
                    timeout.setSingleShot(true);
                    QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
                    auto connection = QObject::connect(&engine, &RenderEngine::resultReady, &loop,
                        [&](const RenderResult &r) {
                            if (r.token == request.token) {
                                rendered = r;
                                loop.quit();
                            }
                        }, Qt::QueuedConnection);
                    timer.start();
                    engine.submit(request);
                    timeout.start(60000);
                    loop.exec();
                    renderTimes.push_back(timer.nsecsElapsed() / 1e6);
                    QObject::disconnect(connection);
                    if (!rendered.ok)
                        throw std::runtime_error("Render failed or timed out");
                    const QString currentHash = imageHash(rendered.image);
                    if (run == 0)
                        expectedHash = currentHash;
                    else if (currentHash != expectedHash)
                        throw std::runtime_error("Repeated render changed pixels");
                }
                auto entry = timings(renderTimes);
                entry["page"] = page;
                entry["clip"] = clipped;
                entry["theme"] = theme == PageTheme::Light ? "light" : "comfort";
                entry["pixels_sha256"] = expectedHash;
                renders.append(entry);
            }
    result["renders"] = renders;
    TextIndex index(engine.baseContext(), doc.get());
    std::vector<double> searchTimes;
    QJsonArray matches;
    for (int run = 0; run < 5; ++run) {
        timer.start();
        const auto found = index.search("the", false, true);
        searchTimes.push_back(timer.nsecsElapsed() / 1e6);
        QJsonArray current;
        for (const auto &match : found)
            current.append(QJsonArray{match.page, match.start, match.length});
        if (run == 0)
            matches = current;
        else if (matches != current)
            throw std::runtime_error("Repeated search changed results");
    }
    result["search"] = timings(searchTimes);
    result["matches"] = matches;
    return result;
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    if (argc < 2) {
        std::fprintf(stderr, "Usage: mervin_benchmark document.pdf [document.pdf ...]\n");
        return 1;
    }
    try {
        QJsonArray files;
        for (const auto &path : app.arguments().mid(1))
            files.append(benchmark(path));
        const auto json = QJsonDocument(QJsonObject{{"platform", QSysInfo::prettyProductName()},
            {"architecture", QSysInfo::currentCpuArchitecture()}, {"qt", qVersion()},
            {"runs", 5}, {"documents", files}}).toJson();
        std::fwrite(json.constData(), 1, json.size(), stdout);
        return 0;
    } catch (const std::exception &error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
