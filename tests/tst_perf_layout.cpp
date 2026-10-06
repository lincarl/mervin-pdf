#include "render/Document.h"
#include "render/RenderEngine.h"
#include "render/ViewLayout.h"

#include <QElapsedTimer>
#include <QtTest>
#include <algorithm>

using namespace mervin;

class TstPerfLayout : public QObject
{
    Q_OBJECT
private slots:
    void viewportLookup()
    {
        RenderEngine engine;
        auto doc = engine.openDocument(QStringLiteral(MERVIN_LONG_PDF));
        QVERIFY(doc);
        ViewLayout layout;
        layout.setDocument(doc.get());
        for (bool spread : {false, true}) {
            layout.setMode({ViewLayout::Scroll::Continuous, spread});
            std::vector<QRect> viewports;
            std::vector<std::vector<int>> expected;
            for (int i = 0; i < 2000; ++i)
                viewports.emplace_back(0, (qint64(i) * 7919) % layout.totalSize().height(), 1400, 900);
            QElapsedTimer timer;
            timer.start();
            for (const auto &viewport : viewports) {
                std::vector<int> found;
                for (int page = 0; page < layout.pageCount(); ++page)
                    if (layout.pageRect(page).intersects(viewport))
                        found.push_back(page);
                expected.push_back(std::move(found));
            }
            const double scanMs = timer.nsecsElapsed() / 1e6;
            std::vector<double> micros;
            for (int i = 0; i < int(viewports.size()); ++i) {
                timer.restart();
                const auto found = layout.pagesInViewport(viewports[i]);
                micros.push_back(timer.nsecsElapsed() / 1e3);
                QCOMPARE(found, expected[i]);
                int expectedLeader = -1;
                for (int page = 0; page < layout.pageCount();) {
                    int end = layout.rowEnd(page);
                    int bottom = 0;
                    for (int p = page; p < end; ++p)
                        bottom = std::max(bottom, layout.pageRect(p).bottom());
                    expectedLeader = page;
                    if (viewports[i].y() <= bottom + 6)
                        break;
                    page = end;
                }
                QCOMPARE(layout.pageAtY(viewports[i].y()), expectedLeader);
            }
            std::sort(micros.begin(), micros.end());
            qInfo("layout spread=%d pages=%d scan=%.2f ms/2000 lookups lookup p50=%.3f us p95=%.3f us",
                  spread, layout.pageCount(), scanMs, micros[1000], micros[1900]);
        }
    }
};
QTEST_GUILESS_MAIN(TstPerfLayout)
#include "tst_perf_layout.moc"
