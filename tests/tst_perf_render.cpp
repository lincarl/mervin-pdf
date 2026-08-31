// Performance guardrail for the render pipeline: times MuPDF rasterization,
// the Comfort tone pass, and QImage::invertPixels (the floor for any pixel
// pass) on an optional local sample drawing at several zoom levels. The printed
// numbers are the deliverable - compare them before/after a change on the
// same machine (run the exe directly or `ctest -R tst_perf -V`; a plain ctest
// run hides passing output). The QVERIFY ceilings are deliberately loose and
// only catch runaway regressions, not machine-to-machine variance.
#include "render/ComfortTransform.h"
#include "render/Document.h"
#include "render/RenderEngine.h"

#include <QDir>
#include <QImage>
#include <QtTest>
#include <algorithm>
#include <cstdio>

class TstPerfRender : public QObject
{
    Q_OBJECT

private slots:
    void renderAndPixelPasses();
    void documentOpen();
};

void TstPerfRender::renderAndPixelPasses()
{
    const QString pdf = QStringLiteral(MERVIN_HOUSE_PDF);
    if (!QFileInfo::exists(pdf))
        QSKIP("examples/house-drawing.pdf not present");

    mervin::RenderEngine engine;
    QString err;
    auto doc = engine.openDocument(pdf, QString(), &err);
    QVERIFY2(doc != nullptr, qPrintable(err));

    for (double s : {1.5, 3.0, 5.0, 7.0}) {
        QElapsedTimer t;
        t.start();
        QImage img = engine.renderPageImage(doc.get(), 0, s, 0);
        const double renderMs = t.nsecsElapsed() / 1e6;
        QVERIFY(!img.isNull());
        if (img.format() != QImage::Format_RGB888)
            img = img.convertToFormat(QImage::Format_RGB888);
        const double px = img.width() * static_cast<double>(img.height());

        double comfortMs = 1e18;
        for (int i = 0; i < 3; ++i) {
            QImage copy = img.copy();
            t.restart();
            mervin::applyComfortTransform(copy);
            comfortMs = std::min(comfortMs, t.nsecsElapsed() / 1e6);
        }

        double invertMs = 1e18;
        for (int i = 0; i < 3; ++i) {
            QImage copy = img.copy();
            t.restart();
            copy.invertPixels();
            invertMs = std::min(invertMs, t.nsecsElapsed() / 1e6);
        }

        std::printf("scale %.1f  %5dx%-5d (%4.1f MPx)  render=%7.1f ms  "
                    "comfort=%7.1f ms  invert=%5.1f ms\n",
                    s, img.width(), img.height(), px / 1e6, renderMs, comfortMs, invertMs);
        std::fflush(stdout);

        QVERIFY2(renderMs < 60000, "page render grossly regressed (>60 s)");
        QVERIFY2(comfortMs < 5000, "Comfort pixel pass grossly regressed (>5 s)");
    }
}

// Time RenderEngine::openDocument on each local sample. This is the per-document
// cost a start pays for every restored tab, and the number a multi-document
// startup is made of - process/window bring-up dominates whole-process timings, so
// tst_perf_startup cannot resolve a change here at all.
//
// Open means: parse the xref, walk the page tree for every page's size, and read
// the catalog. It does NOT include rasterization (above) or text extraction (lazy).
// Warm-cache numbers, best of several runs, so they measure CPU work rather than
// the disk; a cold first open of the same file costs substantially more.
void TstPerfRender::documentOpen()
{
    const QDir examples(QStringLiteral(MERVIN_EXAMPLES_DIR));
    const QStringList names = examples.entryList({QStringLiteral("*.pdf")}, QDir::Files,
                                                 QDir::Name);
    if (names.isEmpty())
        QSKIP("no sample documents present");

    mervin::RenderEngine engine;
    for (const QString &name : names) {
        const QString path = examples.filePath(name);
        double best = 1e18;
        int pages = 0;
        for (int i = 0; i < 5; ++i) {
            QElapsedTimer t;
            t.start();
            QString err;
            auto doc = engine.openDocument(path, QString(), &err);
            const double ms = t.nsecsElapsed() / 1e6;
            QVERIFY2(doc != nullptr, qPrintable(err));
            pages = doc->pageCount();
            best = std::min(best, ms);
        }
        std::printf("open %-20s %3d pages  %7.2f ms  (%5.2f ms/page)\n", qPrintable(name), pages,
                    best, pages > 0 ? best / pages : 0.0);
        std::fflush(stdout);
        QVERIFY2(best < 10000, "document open grossly regressed (>10 s)");
    }
}

QTEST_GUILESS_MAIN(TstPerfRender)
#include "tst_perf_render.moc"
