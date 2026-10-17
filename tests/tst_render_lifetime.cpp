#include "io/Background.h"
#include "render/Document.h"
#include "render/DocumentSearch.h"
#include "render/FormModel.h"
#include "render/RenderEngine.h"
#include "render/TextIndex.h"
#include "Stalls.h"

#include <QElapsedTimer>
#include <QImage>
#include <QFile>
#include <QFileInfo>
#include <QSet>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <mutex>
#include <optional>
#include <thread>

using namespace mervin;
using mervin::testing::SavingJob;
using mervin::testing::Stall;

namespace {

bool closed(const std::shared_ptr<DocumentLifetime> &lifetime)
{
    std::lock_guard lock(lifetime->mutex);
    return !lifetime->document;
}

// Pages rendered without error for `requester` so far.
QSet<int> renderedPages(const QSignalSpy &results, quint64 requester)
{
    QSet<int> pages;
    for (const auto &args : results) {
        const auto result = args[0].value<RenderResult>();
        if (result.requester == requester && result.ok)
            pages.insert(result.pageNo);
    }
    return pages;
}

void submitAllPages(RenderEngine &engine, Document &document, quint64 requester)
{
    for (int page = 0; page < document.pageCount(); ++page) {
        RenderRequest request;
        request.document = &document;
        request.requester = requester;
        request.pageNo = page;
        engine.submit(request);
    }
}

// Writes object bodies with a complete cross-reference table.
QString writePdf(const QTemporaryDir &dir, const QString &name,
                 const QList<QByteArray> &objects)
{
    QByteArray pdf = "%PDF-1.7\n";
    QByteArray xref = "0000000000 65535 f \n";
    for (qsizetype i = 0; i < objects.size(); ++i) {
        xref += QByteArray::number(pdf.size()).rightJustified(10, '0') + " 00000 n \n";
        pdf += QByteArray::number(i + 1) + " 0 obj\n" + objects[i] + "\nendobj\n";
    }
    const QByteArray start = QByteArray::number(pdf.size());
    const QByteArray size = QByteArray::number(objects.size() + 1);
    pdf += "xref\n0 " + size + "\n" + xref + "trailer\n<< /Size " + size
           + " /Root 1 0 R >>\nstartxref\n" + start + "\n%%EOF\n";

    const QString path = dir.filePath(name);
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(pdf) != pdf.size())
        return {};
    return path;
}

// Writes a one-page PDF to `dir` with the web address text "Visit www.inet.se now."
// and one fillable text field, "field1", and returns its path.
QString writeFormAndLinkPdf(const QTemporaryDir &dir)
{
    const QByteArray content = "BT /F1 18 Tf 50 150 Td (Visit www.inet.se now.) Tj ET";
    const QList<QByteArray> objects{
        "<< /Type /Catalog /Pages 2 0 R /AcroForm << /Fields [4 0 R] "
        "/DR << /Font << /Helv 5 0 R >> >> /DA (/Helv 0 Tf 0 g) >> >>",
        "<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
        "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 300 300] /Annots [4 0 R] "
        "/Resources << /Font << /F1 5 0 R /Helv 5 0 R >> >> /Contents 6 0 R >>",
        "<< /Type /Annot /Subtype /Widget /FT /Tx /T (field1) /Rect [20 250 280 270] "
        "/F 4 /DA (/Helv 12 Tf 0 g) /V () /P 3 0 R >>",
        "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>",
        "<< /Length " + QByteArray::number(content.size()) + " >>\nstream\n" + content
            + "\nendstream",
    };
    return writePdf(dir, QStringLiteral("form-and-link.pdf"), objects);
}

// Late page contents, a form field and the outline are more than 512 KiB from both
// file ends. Opening cannot keep them all in MuPDF's file-stream read cache.
QString writeLateObjectsPdf(const QTemporaryDir &dir)
{
    const auto stream = [](const QByteArray &contents) {
        return "<< /Length " + QByteArray::number(contents.size()) + " >>\nstream\n"
               + contents + "\nendstream";
    };
    const QByteArray padding(512 * 1024, 'x');
    const QList<QByteArray> objects{
        "<< /Type /Catalog /Pages 2 0 R /Outlines 10 0 R "
        "/AcroForm << /Fields [9 0 R] /DR << /Font << /Helv 5 0 R >> >> "
        "/DA (/Helv 12 Tf 0 g) >> >>",
        "<< /Type /Pages /Kids [3 0 R 4 0 R] /Count 2 >>",
        "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 300 300] "
        "/Resources << /Font << /F1 5 0 R >> >> /Contents 6 0 R >>",
        "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 300 300] /Annots [9 0 R] "
        "/Resources << /Font << /F1 5 0 R /Helv 5 0 R >> >> /Contents 8 0 R >>",
        "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>",
        stream("BT /F1 18 Tf 40 150 Td (First page) Tj ET"),
        stream(padding),
        stream("1 0 0 rg 40 40 100 80 re f "
               "0 0 0 rg BT /F1 18 Tf 40 150 Td (Later page text) Tj ET"),
        "<< /Type /Annot /Subtype /Widget /FT /Tx /T (laterField) "
        "/Rect [20 250 280 270] /F 4 /DA (/Helv 12 Tf 0 g) "
        "/V (Original value) /P 4 0 R >>",
        "<< /Type /Outlines /First 11 0 R /Last 11 0 R /Count 1 >>",
        "<< /Title (Later section) /Parent 10 0 R /Dest [4 0 R /Fit] >>",
        stream(padding),
    };
    return writePdf(dir, QStringLiteral("late-objects.pdf"), objects);
}

// Tells QTest to expect the warning that a refused screen-thread call logs.
void expectRefusal(const char *method)
{
    const QByteArray warning = QByteArray("Mervin: ") + method
                               + " refused on the screen thread while a job holds the document.";
    QTest::ignoreMessage(QtWarningMsg, warning.constData());
}

QString took(const char *call, const QElapsedTimer &timer)
{
    return QStringLiteral("%1 took %2 ms").arg(QLatin1String(call)).arg(timer.elapsed());
}

} // namespace

class TstRenderLifetime : public QObject
{
    Q_OBJECT
private slots:
    void loadedDocumentKeepsItsSourceBytes_data()
    {
        QTest::addColumn<QString>("query");
        QTest::addColumn<bool>("truncate");
        for (const QString &query : {QStringLiteral("render"), QStringLiteral("forms"),
                                     QStringLiteral("outline"), QStringLiteral("selection")}) {
            QTest::newRow(qPrintable(query + QStringLiteral("-overwritten"))) << query << false;
            QTest::newRow(qPrintable(query + QStringLiteral("-truncated"))) << query << true;
        }
    }

    // The source may stop answering after open. Every later query must use the bytes
    // already loaded, including an unvisited page and lazy form/outline/text objects.
    void loadedDocumentKeepsItsSourceBytes()
    {
        QFETCH(QString, query);
        QFETCH(bool, truncate);
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = writeLateObjectsPdf(dir);
        QVERIFY(!path.isEmpty());
        const qint64 bytes = QFileInfo(path).size();
        QVERIFY(bytes > 1024 * 1024);
        RenderEngine engine;
        auto doc = engine.openDocument(path);
        QVERIFY(doc);
        QCOMPARE(doc->pageCount(), 2);

        // Change the same file while its Document lives, so an open file handle or
        // a buffered fragment cannot hide continued reads from the source.
        QFile source(path);
        QVERIFY(source.open(QIODevice::WriteOnly | QIODevice::Truncate));
        if (!truncate) {
            const QByteArray replacement(bytes, '#');
            QCOMPARE(source.write(replacement), bytes);
        }
        source.close();
        QCOMPARE(QFileInfo(path).size(), truncate ? qint64(0) : bytes);

        if (query == QStringLiteral("render")) {
            QSignalSpy results(&engine, &RenderEngine::resultReady);
            RenderRequest request;
            request.document = doc.get();
            request.pageNo = 1;
            request.requester = 1;
            engine.submit(request);
            QTRY_COMPARE_WITH_TIMEOUT(results.size(), 1, 5000);
            const RenderResult result = results.first().first().value<RenderResult>();
            QVERIFY2(result.ok, qPrintable(result.error));
            QVERIFY(!result.image.isNull());
            QCOMPARE(result.image.pixelColor(80, 220), QColor(Qt::red));
        } else if (query == QStringLiteral("forms")) {
            FormModel form(*doc);
            const auto &fields = form.pageFields(1);
            QCOMPARE(fields.size(), size_t(1));
            QCOMPARE(fields.front().name, QStringLiteral("laterField"));
            QCOMPARE(fields.front().value, QStringLiteral("Original value"));
            QVERIFY(form.setTextValue(1, 0, QStringLiteral("Edited value")));
            QCOMPARE(form.pageFields(1).front().value, QStringLiteral("Edited value"));
        } else if (query == QStringLiteral("outline")) {
            const auto outline = doc->outline();
            QCOMPARE(outline.size(), size_t(1));
            QCOMPARE(outline.front().title, QStringLiteral("Later section"));
            QCOMPARE(outline.front().page, 1);
        } else {
            TextIndex index(engine.baseContext(), doc.get());
            const QString expected = QStringLiteral("Later page text");
            const QString text = index.pageText(1);
            const int start = text.indexOf(expected);
            QVERIFY2(start >= 0, qPrintable(text));
            QCOMPARE(index.textRange(1, start, expected.size()), expected);
            QVERIFY(!index.rangeRects(1, start, expected.size()).empty());
        }
    }

    void closeWhileJobsAreQueued()
    {
        RenderEngine engine;
        for (int i = 0; i < 100; ++i) {
            auto doc = engine.openDocument(QStringLiteral(MERVIN_FIXTURE_PDF));
            QVERIFY(doc);
            auto lifetime = doc->lifetime();
            for (int page = 0; page < doc->pageCount(); ++page) {
                RenderRequest request;
                request.document = doc.get();
                request.requester = i + 1;
                request.pageNo = page;
                request.scale = 2;
                engine.submit(request);
            }
            doc.reset();
            std::lock_guard lock(lifetime->mutex);
            QVERIFY(!lifetime->document);
        }
        // The engine continues rendering after individual documents close.
        auto survivor = engine.openDocument(QStringLiteral(MERVIN_FIXTURE_PDF));
        QSignalSpy results(&engine, &RenderEngine::resultReady);
        RenderRequest request;
        request.document = survivor.get();
        request.requester = 1000;
        engine.submit(request);
        QTRY_VERIFY_WITH_TIMEOUT([&] {
            for (const auto &args : results)
                if (args[0].value<RenderResult>().requester == 1000)
                    return args[0].value<RenderResult>().ok;
            return false;
        }(), 5000);
    }

    void canceledJobsDoNotRasterize()
    {
        RenderEngine engine;
        auto doc = engine.openDocument(QStringLiteral(MERVIN_FIXTURE_PDF));
        QVERIFY(doc);
        QSignalSpy results(&engine, &RenderEngine::resultReady);
        {
            std::lock_guard gate(doc->lifetime()->mutex);
            RenderRequest request;
            request.document = doc.get();
            request.requester = 1;
            engine.submit(request);
            engine.cancelRequests(1);
        }
        engine.shutdown();
        QCoreApplication::processEvents();
        QCOMPARE(results.size(), 0);
    }

    void closingDoesNotWaitForAStuckRender()
    {
        auto engine = std::make_unique<RenderEngine>();
        auto doc = engine->openDocument(QStringLiteral(MERVIN_FIXTURE_PDF));
        QVERIFY(doc);
        const auto lifetime = doc->lifetime();
        Stall stall(lifetime->mutex);
        RenderRequest request;
        request.document = doc.get();
        request.requester = 1;
        engine->submit(request);
        QTest::qWait(200); // a worker takes the request and waits for the document

        QElapsedTimer timer;
        timer.start();
        engine.reset();
        Document::closeInBackground(std::move(doc));
        QVERIFY2(timer.elapsed() < 1000, qPrintable(QStringLiteral("closing took %1 ms")
                                                        .arg(timer.elapsed())));
        stall.release();
        QTRY_VERIFY_WITH_TIMEOUT(closed(lifetime), 10000);
    }

    // Quitting waits for a close whose follow-up removes a file, such as a tab's
    // recovery copy, and not for one without.
    void closesWithFollowUpsCanBeAwaited()
    {
        RenderEngine engine;
        auto doc = engine.openDocument(QStringLiteral(MERVIN_FIXTURE_PDF));
        QVERIFY(doc);
        const auto release = std::make_shared<std::atomic<bool>>(false);
        const auto ran = std::make_shared<std::atomic<bool>>(false);
        Document::closeInBackground(std::move(doc), [release, ran] {
            while (!release->load())
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            ran->store(true);
        });
        QVERIFY(!Document::waitForCloses(100));
        release->store(true);
        QVERIFY(Document::waitForCloses(10000));
        QVERIFY(ran->load());

        auto other = engine.openDocument(QStringLiteral(MERVIN_FIXTURE_PDF));
        QVERIFY(other);
        const auto lifetime = other->lifetime();
        Stall stall(lifetime->mutex);
        Document::closeInBackground(std::move(other));
        QVERIFY(Document::waitForCloses(100));
        stall.release();
        QTRY_VERIFY_WITH_TIMEOUT(closed(lifetime), 10000);
    }

    // A close of no document with nothing to run after it starts no thread.
    void closingNoDocumentStartsNothing()
    {
        QVERIFY(background::settle(10000)); // the earlier closes have ended
        Document::closeInBackground(nullptr);
        QCOMPARE(Document::closing(), 0);
        QCOMPARE(background::running(), 0);
    }

    void closingDoesNotWaitForAStuckSearch()
    {
        RenderEngine engine;
        auto doc = engine.openDocument(QStringLiteral(MERVIN_FIXTURE_PDF));
        QVERIFY(doc);
        const auto lifetime = doc->lifetime();
        auto search = std::make_unique<DocumentSearch>(&engine);
        Stall stall(lifetime->mutex);
        search->start(doc.get(), QStringLiteral("Test"), false, false, [](std::vector<TextMatch>) {
            QFAIL("a canceled search reported matches");
        });
        QTest::qWait(200); // the search starts on the first page and waits for the document

        QElapsedTimer timer;
        timer.start();
        search.reset();
        Document::closeInBackground(std::move(doc));
        QVERIFY2(timer.elapsed() < 1000, qPrintable(QStringLiteral("closing took %1 ms")
                                                        .arg(timer.elapsed())));
        stall.release();
        QTRY_VERIFY_WITH_TIMEOUT(closed(lifetime), 10000);
        QCoreApplication::processEvents();
    }

    // A tab whose job holds its document cancels its viewer's and thumbnails'
    // requests. The workers that already wait for that document then let go of
    // them, so another tab keeps rendering. Without the stepped wait every worker
    // waits out the stall, about 3 s.
    void otherDocumentsRenderWhileOneIsHeld()
    {
        RenderEngine engine;
        auto held = engine.openDocument(QStringLiteral(MERVIN_FIXTURE_PDF));
        auto other = engine.openDocument(QStringLiteral(MERVIN_FIXTURE_PDF));
        QVERIFY(held && other);
        QSignalSpy results(&engine, &RenderEngine::resultReady);
        Stall stall(held->lifetime()->mutex);
        submitAllPages(engine, *held, 1);
        submitAllPages(engine, *held, 2);
        QTest::qWait(200); // every worker takes a request for `held` and waits for it

        engine.cancelRequests(1);
        engine.cancelRequests(2);
        RenderRequest request;
        request.document = other.get();
        request.requester = 3;
        engine.submit(request);
        QTRY_VERIFY_WITH_TIMEOUT(renderedPages(results, 3).contains(0), 1000);
    }

    // The opposite case. Requests that were not canceled wait for the document and
    // render once the job lets go of it.
    void uncanceledJobsRenderAfterTheHold()
    {
        RenderEngine engine;
        auto held = engine.openDocument(QStringLiteral(MERVIN_FIXTURE_PDF));
        QVERIFY(held);
        QSignalSpy results(&engine, &RenderEngine::resultReady);
        {
            Stall stall(held->lifetime()->mutex);
            submitAllPages(engine, *held, 1);
            QTest::qWait(200); // the workers wait for the document in steps
            QCOMPARE(results.size(), 0);
        }
        QTRY_COMPARE_WITH_TIMEOUT(renderedPages(results, 1).size(), held->pageCount(), 10000);
    }

    // The black and white print preview renders without antialiasing on the shared
    // workers. Each worker must return to antialiased rendering for later requests.
    void antialiasingReturnsAfterHardRenders()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QByteArray content = "0 G 1 w 10 10 m 90 60 l S";
        const QString path = writePdf(dir, QStringLiteral("line.pdf"), {
            "<< /Type /Catalog /Pages 2 0 R >>",
            "<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
            "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 100 100] /Contents 4 0 R >>",
            "<< /Length " + QByteArray::number(content.size()) + " >>\nstream\n" + content
                + "\nendstream",
        });
        QVERIFY(!path.isEmpty());
        RenderEngine engine;
        auto doc = engine.openDocument(path);
        QVERIFY(doc);
        QSignalSpy results(&engine, &RenderEngine::resultReady);
        // Distinct requesters, so no request replaces another on the same page.
        const auto render = [&](quint64 first, int count, bool antialias) {
            for (int index = 0; index < count; ++index) {
                RenderRequest request;
                request.document = doc.get();
                request.requester = first + index;
                request.antialias = antialias;
                engine.submit(request);
            }
        };
        const auto images = [&](quint64 first, int count) {
            QList<QImage> found;
            for (const auto &args : results) {
                const auto result = args[0].value<RenderResult>();
                if (result.ok && result.requester >= first && result.requester < first + count)
                    found.append(result.image);
            }
            return found;
        };
        const auto grays = [](const QImage &image) {
            int count = 0;
            for (int y = 0; y < image.height(); ++y)
                for (int x = 0; x < image.width(); ++x)
                    count += qGray(image.pixel(x, y)) % 255 != 0;
            return count;
        };

        render(1, 1, true);
        QTRY_COMPARE_WITH_TIMEOUT(images(1, 1).size(), 1, 5000);
        const QImage smooth = images(1, 1).front();
        QVERIFY(grays(smooth) > 0);
        // More requests than workers, so every worker draws without antialiasing.
        render(100, 16, false);
        QTRY_COMPARE_WITH_TIMEOUT(images(100, 16).size(), 16, 10000);
        for (const QImage &image : images(100, 16))
            QCOMPARE(grays(image), 0);
        render(200, 16, true);
        QTRY_COMPARE_WITH_TIMEOUT(images(200, 16).size(), 16, 10000);
        for (const QImage &image : images(200, 16))
            QCOMPARE(image, smooth);
    }

    // MuPDF clamps every edge to kMaxDevicePixel pixels from the device origin. A
    // tile of a longer page raster would come back blank or with shapes moved, so the
    // engine reports a failure instead, and a tile just inside the limit renders.
    void tilesBeyondTheEdgeClampFail()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        // A 914 mm x 4 m roll drawing with a blue band across its bottom 400 pt.
        const QByteArray content = "0 0 1 rg 0 0 2590.87 400 re f";
        const QString path = writePdf(dir, QStringLiteral("roll.pdf"), {
            "<< /Type /Catalog /Pages 2 0 R >>",
            "<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
            "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 2590.87 11338.58] /Contents 4 0 R >>",
            "<< /Length " + QByteArray::number(content.size()) + " >>\nstream\n" + content
                + "\nendstream",
        });
        QVERIFY(!path.isEmpty());
        RenderEngine engine;
        auto doc = engine.openDocument(path);
        QVERIFY(doc);
        QSignalSpy results(&engine, &RenderEngine::resultReady);
        const auto render = [&](quint64 requester, double scale) {
            // The bottom-left 500 x 500 pixels of the page's raster at `scale`.
            RenderRequest request;
            request.document = doc.get();
            request.requester = requester;
            request.scale = scale;
            const int height = int(std::ceil(11338.58 * scale));
            request.clip = QRect(0, height - 500, 500, 500);
            engine.submit(request);
        };
        const auto result = [&](quint64 requester) -> std::optional<RenderResult> {
            for (const auto &args : results)
                if (args[0].value<RenderResult>().requester == requester)
                    return args[0].value<RenderResult>();
            return std::nullopt;
        };

        render(1, 100); // 1133858 pixels tall
        render(2, 92);  // 1043150 pixels tall
        QTRY_VERIFY_WITH_TIMEOUT(result(1) && result(2), 10000);
        QVERIFY(!result(1)->ok);
        QVERIFY(result(1)->image.isNull());
        QVERIFY(result(2)->ok);
        const QImage inside = result(2)->image;
        QCOMPARE(inside.size(), QSize(500, 500));
        const QColor band = inside.pixelColor(250, 250);
        QVERIFY(band.blue() > 200 && band.red() < 60 && band.green() < 60);
    }

    // A query that slips through on the screen thread while a job holds the
    // document returns its empty value at once instead of waiting for the job.
    // Without the check each call waits for the job's access lock, about 3 s.
    void screenQueriesRefuseWhileAJobHoldsTheDocument()
    {
        RenderEngine engine;
        auto doc = engine.openDocument(QStringLiteral(MERVIN_FIXTURE_PDF));
        QVERIFY(doc);
        const int before = Document::testing::refusals();
        {
            SavingJob job{doc->lifetime()};
            Stall stall(job);
            QVERIFY(job.answered); // only the screen thread is refused

            QElapsedTimer timer;
            expectRefusal("Document::outline");
            timer.start();
            QVERIFY(doc->outline().empty());
            QVERIFY2(timer.elapsed() < 50, qPrintable(took("outline", timer)));

            expectRefusal("Document::pageMeasurement");
            timer.restart();
            QVERIFY(!doc->pageMeasurement(0).hasEmbedded);
            QVERIFY2(timer.elapsed() < 50, qPrintable(took("pageMeasurement", timer)));

            expectRefusal("Document::linkTargetAt");
            timer.restart();
            QVERIFY(!doc->linkTargetAt(0, QPointF(40, 40)));
            QVERIFY2(timer.elapsed() < 50, qPrintable(took("linkTargetAt", timer)));
            QCOMPARE(Document::testing::refusals() - before, 3);

            // Each of the other guarded calls is refused once too, with the warning that
            // names it. Without its guard a call waits for the job and is not counted.
            const auto refused = [](const char *method, const auto &call) {
                expectRefusal(method);
                const int earlier = Document::testing::refusals();
                const bool empty = call();
                return empty && Document::testing::refusals() - earlier == 1;
            };
            QVERIFY(refused("Document::pageGeometry",
                            [&] { return doc->pageGeometry(0).empty(); }));
            QVERIFY(refused("Document::pagePointToPdfMatrix", [&] {
                return doc->pagePointToPdfMatrix(0)
                       == std::array<double, 6>{1.0, 0.0, 0.0, 1.0, 0.0, 0.0};
            }));
            QVERIFY(refused("Document::linkAt",
                            [&] { return doc->linkAt(0, QPointF(40, 40)).isEmpty(); }));
            QVERIFY(refused("Document::itemPropertiesAt",
                            [&] { return !doc->itemPropertiesAt(0, QPointF(40, 40)); }));
            QVERIFY(refused("Document::hasForm", [&] { return !doc->hasForm(); }));
            QVERIFY(refused("Document::hasMervinMeasurements",
                            [&] { return !doc->hasMervinMeasurements(); }));
            QVERIFY(refused("Document::isPdf", [&] { return !doc->isPdf(); }));
            QVERIFY(refused("Document::withPdfDocument", [&] {
                bool called = false;
                return !doc->withPdfDocument([&](fz_context *, pdf_document *) { called = true; })
                       && !called;
            }));
            QTemporaryDir dir;
            QVERIFY(dir.isValid());
            const QString copy = dir.filePath(QStringLiteral("copy.pdf"));
            QVERIFY(refused("Document::savePdfTo",
                            [&] { return !doc->savePdfTo(copy) && !QFile::exists(copy); }));
            QVERIFY(refused("RenderEngine::renderPageImage",
                            [&] { return engine.renderPageImage(doc.get(), 0, 1.0, 0).isNull(); }));
            QCOMPARE(Document::testing::refusals() - before, 13);
        }
        // Once the job lets go, the same thread is answered again.
        QVERIFY(doc->isPdf());
        QCOMPARE(Document::testing::refusals() - before, 13);
    }

    // A refused page is not cached as empty. The text index reads it once the job
    // lets go of the document.
    void textIndexReadsAPageRefusedDuringAHold()
    {
        RenderEngine engine;
        auto doc = engine.openDocument(QStringLiteral(MERVIN_FIXTURE_PDF));
        QVERIFY(doc);
        TextIndex index(engine.baseContext(), doc.get());
        const int before = Document::testing::refusals();
        {
            SavingJob job{doc->lifetime()};
            Stall stall(job);
            QVERIFY(job.answered);
            expectRefusal("TextIndex::ensure");
            QVERIFY(index.pageText(0).isEmpty());
            QCOMPARE(Document::testing::refusals() - before, 1);
        }
        QVERIFY2(index.pageText(0).contains(QStringLiteral("STANDARD TEST")),
                 qPrintable(index.pageText(0)));
    }

    // The text index does not keep the links of a page it was refused, so the web
    // address under a point is found once the job lets go of the document.
    void textIndexFindsLinksOnAPageRefusedDuringAHold()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        RenderEngine engine;
        auto doc = engine.openDocument(writeFormAndLinkPdf(dir));
        QVERIFY(doc);
        QPointF point;
        {
            TextIndex probe(engine.baseContext(), doc.get());
            const QString text = probe.pageText(0);
            const QString address = QStringLiteral("www.inet.se");
            const int start = text.indexOf(address);
            QVERIFY2(start >= 0, qPrintable(text));
            const std::vector<QRectF> rects = probe.rangeRects(0, start, address.size());
            QVERIFY(!rects.empty());
            point = rects.front().center();
        }
        TextIndex index(engine.baseContext(), doc.get());
        const int before = Document::testing::refusals();
        {
            SavingJob job{doc->lifetime()};
            Stall stall(job);
            QVERIFY(job.answered);
            expectRefusal("TextIndex::ensure");
            QVERIFY(!index.linkAt(0, point));
            QCOMPARE(Document::testing::refusals() - before, 1);
        }
        const std::optional<TextLink> link = index.linkAt(0, point);
        QVERIFY(link);
        QCOMPARE(link->url, QStringLiteral("https://www.inet.se"));
    }

    // FormModel does not cache the empty field list of a page it was refused, so the
    // page's fields appear once the job lets go of the document.
    void formFieldsAppearAfterAHold()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        RenderEngine engine;
        auto doc = engine.openDocument(writeFormAndLinkPdf(dir));
        QVERIFY(doc);
        QVERIFY(doc->hasForm());
        FormModel form(*doc);
        const int before = Document::testing::refusals();
        {
            SavingJob job{doc->lifetime()};
            Stall stall(job);
            QVERIFY(job.answered);
            expectRefusal("Document::withPdfDocument");
            QVERIFY(form.pageFields(0).empty());
            QCOMPARE(Document::testing::refusals() - before, 1);
        }
        const std::vector<FormField> &fields = form.pageFields(0);
        QCOMPARE(fields.size(), size_t(1));
        QCOMPARE(fields.front().name, QStringLiteral("field1"));
    }

    // A document that is not a PDF has no form fields. That answer is cached like
    // any other, so a later call does not ask the document again, even while a job
    // holds it.
    void formFieldsOfANonPdfAreCached()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.filePath(QStringLiteral("picture.png"));
        QImage image(64, 64, QImage::Format_RGB32);
        image.fill(Qt::white);
        QVERIFY(image.save(path));
        RenderEngine engine;
        auto doc = engine.openDocument(path);
        QVERIFY(doc);
        QVERIFY(!doc->isPdf());
        FormModel form(*doc);
        QVERIFY(form.pageFields(0).empty());
        const int before = Document::testing::refusals();
        {
            SavingJob job{doc->lifetime()};
            Stall stall(job);
            QVERIFY(form.pageFields(0).empty());
        }
        QCOMPARE(Document::testing::refusals() - before, 0);
    }

    // A job that is stopped while another thread holds the gate stops waiting for
    // it within a step. The screen thread is refused while the hold waits, and
    // answered again as soon as it gives up.
    void holdGivesUpWhenStopped()
    {
        RenderEngine engine;
        auto doc = engine.openDocument(QStringLiteral(MERVIN_FIXTURE_PDF));
        QVERIFY(doc);
        const auto lifetime = doc->lifetime();
        const int before = Document::testing::refusals();
        {
            Stall stall(lifetime->mutex);
            std::atomic<bool> stop{false};
            std::atomic<int> asked{0};
            std::atomic<bool> got{true};
            std::chrono::steady_clock::time_point gaveUp;
            std::thread job([&] {
                const Document::Hold hold(lifetime, [&] {
                    ++asked;
                    return stop.load();
                });
                gaveUp = std::chrono::steady_clock::now();
                got = hold.document() != nullptr;
            });
            const auto joinJob = qScopeGuard([&] {
                stop = true;
                if (job.joinable())
                    job.join();
            });
            QTRY_VERIFY_WITH_TIMEOUT(asked.load() >= 2, 10000); // it waits in steps

            // A query is refused while the hold still waits for the gate, so it cannot
            // end up waiting for the job once the hold takes the gate.
            expectRefusal("Document::isPdf");
            QVERIFY(!doc->isPdf());
            QCOMPARE(Document::testing::refusals() - before, 1);

            const auto stopped = std::chrono::steady_clock::now();
            stop = true;
            job.join();
            // Timed on the job thread, which leaves out waking this thread from join().
            const auto gaveUpMs =
                std::chrono::duration_cast<std::chrono::milliseconds>(gaveUp - stopped).count();
            QVERIFY2(gaveUpMs < 100,
                     qPrintable(QStringLiteral("giving up took %1 ms").arg(gaveUpMs)));
            QVERIFY(!got.load());
            QVERIFY(doc->isPdf()); // not refused, though the gate is still taken
            QCOMPARE(Document::testing::refusals() - before, 1);
        }

        // A document that has closed is not held either.
        doc.reset();
        bool gotClosed = true;
        std::thread([&] {
            const Document::Hold hold(lifetime);
            gotClosed = hold.document() != nullptr;
        }).join();
        QVERIFY(!gotClosed);
        QCOMPARE(lifetime->holds.load(), 0);
    }

    // The screen thread checks for a hold before it locks the document. A hold that
    // starts between the check and the lock waits for the query to end, so the
    // query cannot end up waiting for the job behind it.
    void holdWaitsForAScreenQueryInFlight()
    {
        RenderEngine engine;
        auto doc = engine.openDocument(QStringLiteral(MERVIN_FIXTURE_PDF));
        QVERIFY(doc);
        const auto lifetime = doc->lifetime();
        std::atomic<int> asked{0};
        std::atomic<bool> got{false};
        std::thread job;
        const auto joinJob = qScopeGuard([&] {
            if (job.joinable())
                job.join();
        });
        {
            const Document::ScreenQuery query(*lifetime, "test");
            QVERIFY(!query.refused());
            job = std::thread([&] {
                const Document::Hold hold(lifetime, [&] {
                    ++asked;
                    return false;
                });
                got = hold.document() != nullptr;
            });
            QTRY_VERIFY_WITH_TIMEOUT(asked.load() >= 3 || got.load(), 10000);
            QVERIFY(!got.load());
        }
        job.join();
        QVERIFY(got.load());
    }

    // A call that a screen-thread query makes inside itself, as savePdfTo calls
    // withPdfDocument, still runs when a hold starts between the two. The hold waits
    // for the outer query, so refusing the inner one would only fail a call that was
    // already let through.
    void nestedQueryRunsWhileAHoldWaits()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        RenderEngine engine;
        auto doc = engine.openDocument(QStringLiteral(MERVIN_FIXTURE_PDF));
        QVERIFY(doc);
        const auto lifetime = doc->lifetime();
        const int before = Document::testing::refusals();
        const QString copy = dir.filePath(QStringLiteral("copy.pdf"));
        std::atomic<bool> got{false};
        std::thread job;
        const auto joinJob = qScopeGuard([&] {
            if (job.joinable())
                job.join();
        });
        {
            const Document::ScreenQuery outer(*lifetime, "test");
            QVERIFY(!outer.refused());
            job = std::thread([&] {
                const Document::Hold hold(lifetime);
                got = hold.document() != nullptr;
            });
            QTRY_VERIFY_WITH_TIMEOUT(lifetime->holds.load() > 0, 10000);
            QVERIFY(doc->savePdfTo(copy));
            QVERIFY(!got.load());
        }
        job.join();
        QVERIFY(got.load());
        QCOMPARE(Document::testing::refusals() - before, 0);
        QVERIFY(QFileInfo(copy).size() > 0);
    }
};

QTEST_GUILESS_MAIN(TstRenderLifetime)
#include "tst_render_lifetime.moc"
