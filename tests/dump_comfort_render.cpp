// Developer tool: renders pages through the real Comfort pipeline (worker
// pool, display-list walk, per-image classification) and writes PNGs, for
// visual verification of the image treatments.
// Usage: dump_comfort_render <pdf> <pageNo0> <out.png> [dpi]
#include "render/Document.h"
#include "render/RenderEngine.h"
#include "render/RenderTypes.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QImage>

#include <cstdio>

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    if (argc < 4) {
        std::fprintf(stderr, "usage: dump_comfort_render <pdf> <pageNo0> <out.png> [dpi]\n");
        return 2;
    }
    const QString pdf = QString::fromLocal8Bit(argv[1]);
    const int pageNo = QString::fromLocal8Bit(argv[2]).toInt();
    const QString out = QString::fromLocal8Bit(argv[3]);
    const double dpi = argc > 4 ? QString::fromLocal8Bit(argv[4]).toDouble() : 150.0;

    mervin::RenderEngine engine;
    QString err;
    auto doc = engine.openDocument(pdf, QString(), &err);
    if (!doc) {
        std::fprintf(stderr, "open failed: %s\n", qPrintable(err));
        return 1;
    }

    mervin::RenderResult got;
    bool received = false;
    QObject::connect(
        &engine, &mervin::RenderEngine::resultReady, &app,
        [&](const mervin::RenderResult &r) {
            got = r;
            received = true;
        },
        Qt::QueuedConnection);

    mervin::RenderRequest req;
    req.document = doc.get();
    req.requester = 1;
    req.pageNo = pageNo;
    req.scale = dpi / 72.0;
    req.theme = mervin::PageTheme::Comfort;
    req.token = 1;
    engine.submit(req);

    QElapsedTimer t;
    t.start();
    while (!received && t.elapsed() < 30000)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
    engine.shutdown();

    if (!received || !got.ok) {
        std::fprintf(stderr, "render failed: %s\n", qPrintable(got.error));
        return 1;
    }
    if (!got.image.save(out)) {
        std::fprintf(stderr, "save failed: %s\n", qPrintable(out));
        return 1;
    }
    std::printf("%dx%d -> %s\n", got.image.width(), got.image.height(), qPrintable(out));
    return 0;
}
