#include "io/Background.h"
#include "render/AnnotModel.h"
#include "render/Document.h"
#include "render/FormModel.h"
#include "render/RenderEngine.h"
#include "render/ViewerQueries.h"
#include "ui/ViewerWidget.h"

#include <QApplication>
#include <QClipboard>
#include <QComboBox>
#include <QElapsedTimer>
#include <QLineEdit>
#include <QListWidget>
#include <QFile>
#include "ui/PdfPropertiesPopup.h"
#include "ui/AnnotPopup.h"
#include <QScopeGuard>
#include <QSemaphore>
#include <QSignalBlocker>
#include <QTemporaryDir>
#include <QTimer>
#include <QtTest>
#include <atomic>

using namespace mervin;

namespace {
// A redistributable two-page drawing with an internal link, KiCad properties and outline.
QByteArray queryFixture()
{
    const QByteArray content = "BT /F1 16 Tf 20 260 Td (Selected text) Tj ET 20 180 m 160 180 l S";
    const QList<QByteArray> objects{
        "<< /Type /Catalog /Pages 2 0 R /Outlines 8 0 R >>",
        "<< /Type /Pages /Kids [3 0 R 4 0 R] /Count 2 >>",
        "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 300 300] /Contents 5 0 R /Resources << /Font << /F1 10 0 R >> >> /Annots [6 0 R 7 0 R] /VP [<< /Type /Viewport /BBox [0 0 300 300] /Measure << /Subtype /RL /R (1:100) /X [<< /U (mm) /C 35.2778 >>] >> >>] >>",
        "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 300 300] /Resources << >> >>",
        "<< /Length " + QByteArray::number(content.size()) + " >>\nstream\n" + content + "\nendstream",
        "<< /Type /Annot /Subtype /Link /Rect [20 200 140 230] /Dest [4 0 R /Fit] >>",
        "<< /Type /Annot /Subtype /Link /Rect [160 200 280 230] /A << /S /JavaScript /JS (ShM([[\"Reference = R7\"], [\"Value = 10k\"]]);) >> >>",
        "<< /Type /Outlines /First 9 0 R /Last 9 0 R /Count 1 >>",
        "<< /Title (Second page) /Parent 8 0 R /Dest [4 0 R /Fit] >>",
        "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>"};
    QByteArray pdf = "%PDF-1.7\n";
    QList<int> offsets;
    for (int i = 0; i < objects.size(); ++i) {
        offsets.append(pdf.size());
        pdf += QByteArray::number(i + 1) + " 0 obj\n" + objects[i] + "\nendobj\n";
    }
    const int xref = pdf.size();
    pdf += "xref\n0 " + QByteArray::number(objects.size() + 1) + "\n0000000000 65535 f \n";
    for (int offset : offsets) pdf += QByteArray::number(offset).rightJustified(10, '0') + " 00000 n \n";
    pdf += "trailer\n<< /Size " + QByteArray::number(objects.size() + 1)
        + " /Root 1 0 R >>\nstartxref\n" + QByteArray::number(xref) + "\n%%EOF\n";
    return pdf;
}

/// Two visible widgets share either equal display labels or equal export values.
QByteArray choiceIdentityFixture(bool list, bool duplicateExports)
{
    const QByteArray options = duplicateExports
        ? "[[(same) (First)] [(same) (Second)]]"
        : "[[(a) (Same)] [(b) (Same)]]";
    const QByteArray value = duplicateExports ? "same" : "a";
    const QList<QByteArray> objects{
        "<< /Type /Catalog /Pages 2 0 R /AcroForm << /Fields [5 0 R] /DR << /Font << /Helv 4 0 R >> >> /DA (/Helv 12 Tf 0 g) >> >>",
        "<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
        "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 400 350] /Annots [6 0 R 7 0 R] /Resources << >> >>",
        "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>",
        "<< /FT /Ch /Ff " + QByteArray::number(list ? 0 : 131072) + " /T (shared-choice) /V (" + value + ") /I [0] /Opt " + options + " /Kids [6 0 R 7 0 R] >>",
        "<< /Type /Annot /Subtype /Widget /Parent 5 0 R /P 3 0 R /Rect [20 230 380 " + QByteArray(list ? "300" : "255") + "] /F 4 >>",
        "<< /Type /Annot /Subtype /Widget /Parent 5 0 R /P 3 0 R /Rect [20 100 380 " + QByteArray(list ? "170" : "125") + "] /F 4 >>"};
    QByteArray pdf = "%PDF-1.7\n";
    QList<int> offsets;
    for (int i = 0; i < objects.size(); ++i) {
        offsets.append(pdf.size());
        pdf += QByteArray::number(i + 1) + " 0 obj\n" + objects[i] + "\nendobj\n";
    }
    const int xref = pdf.size();
    pdf += "xref\n0 " + QByteArray::number(objects.size() + 1) + "\n0000000000 65535 f \n";
    for (int offset : offsets) pdf += QByteArray::number(offset).rightJustified(10, '0') + " 00000 n \n";
    pdf += "trailer\n<< /Size " + QByteArray::number(objects.size() + 1)
        + " /Root 1 0 R >>\nstartxref\n" + QByteArray::number(xref) + "\n%%EOF\n";
    return pdf;
}

// Locate the page through the public hit-test, independent of viewport margins.
QPoint viewportPoint(ViewerWidget &viewer, QPointF pagePoint)
{
    QPoint origin(-1, -1);
    for (int y = 0; y < viewer.viewport()->height() && origin.x() < 0; ++y)
        for (int x = 0; x < viewer.viewport()->width(); ++x)
            if (viewer.pageUnder({x, y}) == 0) { origin = {x, y}; break; }
    return origin + QPoint(qRound(pagePoint.x() * viewer.scale()), qRound(pagePoint.y() * viewer.scale()));
}
}

// Exercises real Qt event delivery while a page parser or document lock is stalled.
class TstViewerQueries : public QObject
{
    Q_OBJECT
private slots:
    void cleanup()
    {
        ViewerQueries::testing::observe({});
        QVERIFY(background::settle(5000));
    }

    void pageReadsDeliverEventsAndComplete()
    {
        RenderEngine engine;
        auto doc = engine.openDocument(QStringLiteral(MERVIN_FORM_PDF));
        QVERIFY(doc);
        ViewerQueries queries(doc.get(), engine.baseContext());
        QSemaphore entered, release;
        std::atomic<int> reads{0};
        ViewerQueries::testing::observe([&](bool edit, int page) {
            if (!edit && page == 0) { ++reads; entered.release(); release.acquire(); }
        });
        const auto unblock = qScopeGuard([&] { release.release(100); ViewerQueries::testing::observe({}); });
        std::optional<ViewerPage> result;
        queries.requestPage(0, [&](ViewerPage value) { result = std::move(value); });
        queries.requestPage(0, [](ViewerPage) { QFAIL("duplicate read was not coalesced"); });
        QTRY_VERIFY(entered.available() == 1);
        bool delivered = false;
        QTimer::singleShot(0, [&] { delivered = true; });
        QTRY_VERIFY(delivered);
        QVERIFY(!result);
        QCOMPARE(reads.load(), 1);
        release.release();
        QTRY_VERIFY(result.has_value());
        QCOMPARE(result->fields.size(), size_t(1));
        QCOMPARE(result->fields.front().value, QStringLiteral("value-0"));
        QCOMPARE(result->annotations.size(), size_t(1));
        QVERIFY(result->text.text.contains(QStringLiteral("STANDARD TEST")));
        QVERIFY(result->text.linksReady);
    }

    void canceledLockWaitCannotReachUnloadedViewer()
    {
        RenderEngine engine;
        auto doc = engine.openDocument(QStringLiteral(MERVIN_FORM_PDF));
        QVERIFY(doc);
        auto viewer = std::make_unique<ViewerWidget>(&engine);
        QSemaphore locked, release;
        background::start([document = doc.get(), &locked, &release] {
            std::lock_guard lock(document->accessMutex());
            locked.release();
            release.acquire();
        });
        const auto unblock = qScopeGuard([&] { release.release(100); });
        QTRY_VERIFY(locked.available() == 1);
        QElapsedTimer elapsed;
        elapsed.start();
        viewer->setDocument(doc.get());
        viewer->resize(800, 600);
        viewer->show();
        QCoreApplication::processEvents();
        viewer->setDocument(nullptr);
        viewer.reset();
        Document::closeInBackground(std::move(doc));
        QVERIFY2(elapsed.elapsed() < 500, "unload waited for a document access lock");
        bool delivered = false;
        QTimer::singleShot(0, [&] { delivered = true; });
        QTRY_VERIFY(delivered);
        release.release();
        QTRY_COMPARE(Document::closing(), 0);
    }

    void acceptedFormEditFinishesBeforeSave()
    {
        RenderEngine engine;
        auto doc = engine.openDocument(QStringLiteral(MERVIN_FORM_PDF));
        QVERIFY(doc);
        ViewerWidget viewer(&engine);
        viewer.resize(800, 700);
        viewer.setDocument(doc.get());
        viewer.show();
        QTRY_VERIFY(!viewer.findChildren<QLineEdit *>().empty());
        auto *editor = viewer.findChildren<QLineEdit *>().front();
        QTRY_COMPARE(editor->text(), QStringLiteral("value-0"));
        QSemaphore entered, release;
        ViewerQueries::testing::observe([&](bool edit, int) {
            if (edit) { entered.release(); release.acquire(); }
        });
        const auto unblock = qScopeGuard([&] { release.release(100); ViewerQueries::testing::observe({}); });
        editor->setText(QStringLiteral("accepted edit"));
        viewer.commitActiveFormEditor();
        QVERIFY(viewer.hasFormEdits());
        QTRY_VERIFY(entered.available() >= 1);
        QTemporaryDir directory;
        const QString saved = directory.filePath(QStringLiteral("saved.pdf"));
        std::atomic<bool> savedDone{false};
        std::atomic<bool> savedOk{false};
        background::start([life = doc->lifetime(), &savedDone, &savedOk, saved] {
            Document::Hold hold(life);
            if (auto *document = hold.document()) savedOk = document->savePdfTo(saved);
            savedDone = true;
        });
        bool delivered = false;
        QTimer::singleShot(0, [&] { delivered = true; });
        QTRY_VERIFY(delivered);
        QVERIFY(!savedDone.load());
        editor->setText(QStringLiteral("later typing"));
        release.release();
        QTRY_VERIFY(savedDone.load());
        QVERIFY(savedOk.load());
        auto reopened = engine.openDocument(saved);
        QVERIFY(reopened);
        QCOMPARE(FormModel(*reopened).pageFields(0).front().value, QStringLiteral("accepted edit"));
        QTRY_COMPARE(editor->text(), QStringLiteral("later typing"));
        ViewerQueries::testing::observe({});
        viewer.commitActiveFormEditor();
        QTRY_COMPARE(doc->lifetime()->pendingEdits.load(), 0);
        if (!qEnvironmentVariable("MERVIN_QUERY_EVIDENCE").isEmpty())
            QVERIFY(viewer.grab().save(qEnvironmentVariable("MERVIN_QUERY_EVIDENCE") + QStringLiteral("/viewer-form.png")));
        viewer.setDocument(nullptr);
    }

    void queuedEditsSurviveCloseAndRemainOrdered()
    {
        RenderEngine engine;
        auto doc = engine.openDocument(QStringLiteral(MERVIN_FORM_PDF));
        QVERIFY(doc);
        auto queries = std::make_unique<ViewerQueries>(doc.get(), engine.baseContext());
        QSemaphore entered, release;
        ViewerQueries::testing::observe([&](bool edit, int page) {
            if (!edit && page == 0) { entered.release(); release.acquire(); }
        });
        const auto unblock = qScopeGuard([&] { release.release(100); ViewerQueries::testing::observe({}); });
        queries->requestPage(0, [](ViewerPage) { QFAIL("canceled read delivered a result"); });
        QTRY_VERIFY(entered.available() == 1);
        for (int page = 1; page < doc->pageCount(); ++page)
            queries->requestPage(page, [](ViewerPage) { QFAIL("pending read survived cancellation"); });
        queries->cancelReads();
        std::atomic<bool> first{false}, second{false};
        queries->edit([&](Document &document) {
            first = FormModel(document).setTextValue(0, 0, QStringLiteral("first"));
        }, [] { QFAIL("closed query owner received an edit callback"); });
        queries->edit([&](Document &document) {
            FormModel model(document);
            second = model.pageFields(0).front().value == QStringLiteral("first")
                && model.setTextValue(0, 0, QStringLiteral("second"));
        }, [] { QFAIL("closed query owner received an edit callback"); });
        const auto lifetime = doc->lifetime();
        QCOMPARE(lifetime->pendingEdits.load(), 2);
        queries.reset();
        Document::closeInBackground(std::move(doc));
        bool delivered = false;
        QTimer::singleShot(0, [&] { delivered = true; });
        QTRY_VERIFY(delivered);
        QVERIFY(!first.load());
        release.release();
        QTRY_COMPARE(Document::closing(), 0);
        QVERIFY(first.load());
        QVERIFY(second.load());
        QCOMPARE(lifetime->pendingEdits.load(), 0);
    }

    void choiceEditorsPreserveOptionIdentity_data()
    {
        QTest::addColumn<bool>("list");
        QTest::addColumn<bool>("duplicateExports");
        QTest::newRow("combo-duplicate-labels") << false << false;
        QTest::newRow("list-duplicate-labels") << true << false;
        QTest::newRow("combo-duplicate-exports") << false << true;
        QTest::newRow("list-duplicate-exports") << true << true;
    }

    /// Option identities survive queued edits, sibling refresh, saving and editor recreation.
    void choiceEditorsPreserveOptionIdentity()
    {
        QFETCH(bool, list);
        QFETCH(bool, duplicateExports);
        QTemporaryDir files;
        QVERIFY(files.isValid());
        const QString source = files.filePath(QStringLiteral("choices.pdf"));
        QFile file(source);
        QVERIFY(file.open(QIODevice::WriteOnly));
        const QByteArray bytes = choiceIdentityFixture(list, duplicateExports);
        QCOMPARE(file.write(bytes), bytes.size());
        file.close();
        RenderEngine engine;
        auto doc = engine.openDocument(source);
        QVERIFY(doc);
        ViewerWidget viewer(&engine);
        viewer.resize(850, 700);
        viewer.setDocument(doc.get());
        viewer.show();
        auto editors = [list](ViewerWidget &owner) {
            QList<QWidget *> result;
            if (list) for (auto *widget : owner.findChildren<QListWidget *>()) result.append(widget);
            else for (auto *widget : owner.findChildren<QComboBox *>()) result.append(widget);
            return result;
        };
        auto index = [list](QWidget *widget) {
            return list ? qobject_cast<QListWidget *>(widget)->currentRow()
                        : qobject_cast<QComboBox *>(widget)->currentIndex();
        };
        QTRY_COMPARE(editors(viewer).size(), 2);
        const auto widgets = editors(viewer);
        QCOMPARE(index(widgets[0]), 0);
        QCOMPARE(index(widgets[1]), 0);
        QSemaphore entered, release;
        std::atomic<bool> once{true};
        ViewerQueries::testing::observe([&](bool edit, int) {
            if (edit && once.exchange(false)) { entered.release(); release.acquire(); }
        });
        const auto unblock = qScopeGuard([&] { release.release(100); ViewerQueries::testing::observe({}); });
        QSignalSpy changed(&viewer, &ViewerWidget::formEditsChanged);
        widgets[0]->setFocus();
        QTest::keyClick(widgets[0], Qt::Key_Down);
        QVERIFY(viewer.hasFormEdits());
        QTRY_COMPARE(entered.available(), 1);
        // Returning to the original equal-label row must enqueue after the first edit.
        QTest::keyClick(widgets[0], Qt::Key_Up);
        viewer.commitActiveFormEditor();
        QCOMPARE(doc->lifetime()->pendingEdits.load(), 2);
        QCOMPARE(changed.count(), 2);
        release.release();
        QTRY_COMPARE(changed.count(), 3);
        QCOMPARE(index(widgets[0]), 0);
        QCOMPARE(index(widgets[1]), 0);
        QCOMPARE(viewer.formModel()->pageFields(0)[0].choiceIndex, 0);
        // Flushing an uncommitted row change must compare identity as well as text.
        {
            const QSignalBlocker block(widgets[0]);
            if (list) qobject_cast<QListWidget *>(widgets[0])->setCurrentRow(1);
            else qobject_cast<QComboBox *>(widgets[0])->setCurrentIndex(1);
        }
        viewer.commitActiveFormEditor();
        QTRY_COMPARE(changed.count(), 5);
        QCOMPARE(index(widgets[0]), 1);
        QCOMPARE(index(widgets[1]), 1);
        QCOMPARE(viewer.formModel()->pageFields(0)[0].choiceIndex, 1);
        QCOMPARE(viewer.formModel()->pageFields(0)[1].choiceIndex, 1);
        const QString saved = files.filePath(QStringLiteral("saved.pdf"));
        std::atomic<bool> savedDone{false}, savedOk{false};
        background::start([life = doc->lifetime(), &savedDone, &savedOk, saved] {
            Document::Hold hold(life);
            if (auto *document = hold.document()) savedOk = document->savePdfTo(saved);
            savedDone = true;
        });
        QTRY_VERIFY(savedDone.load());
        QVERIFY(savedOk.load());
        auto reopened = engine.openDocument(saved);
        QVERIFY(reopened);
        QCOMPARE(FormModel(*reopened).pageFields(0)[0].choiceIndex, 1);
        viewer.setDocument(nullptr);
        ViewerWidget reopenedViewer(&engine);
        reopenedViewer.resize(850, 700);
        reopenedViewer.setDocument(reopened.get());
        reopenedViewer.show();
        QTRY_COMPARE(editors(reopenedViewer).size(), 2);
        QCOMPARE(index(editors(reopenedViewer)[0]), 1);
        QCOMPARE(index(editors(reopenedViewer)[1]), 1);
        reopenedViewer.setDocument(nullptr);
    }

    void parsingCancellationLeavesNoPartialCache()
    {
        RenderEngine engine;
        auto doc = engine.openDocument(QStringLiteral(MERVIN_FORM_PDF));
        QVERIFY(doc);
        QSemaphore entered, release;
        auto state = std::make_shared<job::State>();
        std::atomic<bool> done{false}, ready{true};
        const auto unblock = qScopeGuard([&] { release.release(100); });
        background::start([&, base = engine.baseContext(), document = doc.get()] {
            const job::Scope scope(*state);
            bool first = true;
            Document::testing::setRasterObserver([&] {
                if (std::exchange(first, false)) { entered.release(); release.acquire(); }
            });
            TextIndex index(base, document);
            ready = index.snapshot(0).ready;
            Document::testing::setRasterObserver({});
            done = true;
        });
        QTRY_VERIFY(entered.available() == 1);
        bool delivered = false;
        QTimer::singleShot(0, [&] { delivered = true; });
        QTRY_VERIFY(delivered);
        state->stop();
        release.release();
        QTRY_VERIFY(done.load());
        QVERIFY(!ready.load());
        TextIndex retry(engine.baseContext(), doc.get());
        QVERIFY(retry.snapshot(0).text.contains(QStringLiteral("STANDARD TEST")));
    }

    void delayedLinkAndPropertiesClicksKeepTheirTargets()
    {
        QTemporaryDir directory;
        const QString path = directory.filePath(QStringLiteral("queries.pdf"));
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(queryFixture());
        file.close();
        RenderEngine engine;
        auto doc = engine.openDocument(path);
        QVERIFY(doc);
        ViewerWidget viewer(&engine);
        viewer.resize(600, 600);
        QSemaphore entered, release;
        std::atomic<bool> once{true};
        ViewerQueries::testing::observe([&](bool edit, int page) {
            if (!edit && page == 0 && once.exchange(false)) { entered.release(); release.acquire(); }
        });
        const auto unblock = qScopeGuard([&] { release.release(100); ViewerQueries::testing::observe({}); });
        viewer.setDocument(doc.get());
        viewer.setZoomMode(ViewerWidget::ZoomMode::FitPage);
        viewer.show();
        QTRY_VERIFY(entered.available() == 1);
        // A complete click arrives before parsing finishes. Its internal destination must survive.
        QTest::mouseClick(viewer.viewport(), Qt::LeftButton, {}, viewportPoint(viewer, {80, 85}));
        QCOMPARE(viewer.currentPage(), 0);
        release.release();
        QTRY_COMPARE(viewer.currentPage(), 1);
        viewer.goToPage(0);
        QTest::mouseClick(viewer.viewport(), Qt::LeftButton, {}, viewportPoint(viewer, {220, 85}));
        QTRY_VERIFY(!viewer.findChildren<PdfPropertiesPopup *>().empty());
        QVERIFY(viewer.findChildren<PdfPropertiesPopup *>().front()->isVisible());
        if (!qEnvironmentVariable("MERVIN_QUERY_EVIDENCE").isEmpty()) {
            const QString evidence = qEnvironmentVariable("MERVIN_QUERY_EVIDENCE");
            QVERIFY(viewer.grab().save(evidence + QStringLiteral("/viewer-properties.png")));
            QFile::remove(evidence + QStringLiteral("/query-interactions.pdf"));
            QVERIFY(QFile::copy(path, evidence + QStringLiteral("/query-interactions.pdf")));
        }
        QTRY_VERIFY(!viewer.outline().empty());
        QCOMPARE(viewer.outline().front().page, 1);
        QVERIFY(viewer.pageHasEmbeddedScale(0));
        viewer.setDocument(nullptr);
    }

    void geometryWaitReplaysSnappedMeasurement()
    {
        QTemporaryDir directory;
        const QString path = directory.filePath(QStringLiteral("geometry.pdf"));
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(queryFixture());
        file.close();
        RenderEngine engine;
        auto doc = engine.openDocument(path);
        QVERIFY(doc);
        ViewerWidget viewer(&engine);
        viewer.resize(600, 600);
        viewer.setDocument(doc.get());
        viewer.setZoomMode(ViewerWidget::ZoomMode::FitPage);
        viewer.show();
        QTRY_VERIFY(viewer.pageHasEmbeddedScale(0));
        QSemaphore entered, release;
        std::atomic<bool> once{true};
        ViewerQueries::testing::observe([&](bool edit, int key) {
            if (!edit && key == -1 && once.exchange(false)) { entered.release(); release.acquire(); }
        });
        const auto unblock = qScopeGuard([&] { release.release(100); ViewerQueries::testing::observe({}); });
        viewer.setMeasureMode(true);
        QTest::mouseClick(viewer.viewport(), Qt::LeftButton, {}, viewportPoint(viewer, {22, 121}));
        QTRY_VERIFY(entered.available() == 1);
        QTest::mouseClick(viewer.viewport(), Qt::LeftButton, {}, viewportPoint(viewer, {159, 121}));
        QVERIFY(viewer.committedMeasurements().empty());
        bool delivered = false;
        QTimer::singleShot(0, [&] { delivered = true; });
        QTRY_VERIFY(delivered);
        release.release();
        QTRY_COMPARE(viewer.committedMeasurements().size(), size_t(1));
        const auto &measurement = viewer.committedMeasurements().front();
        QCOMPARE(measurement.pts.size(), size_t(2));
        QCOMPARE(measurement.pts.front(), QPointF(20, 120));
        QCOMPARE(measurement.pts.back(), QPointF(160, 120));
        viewer.setDocument(nullptr);
    }

    void switchingDocumentsRejectsOldReadAndGesture()
    {
        RenderEngine engine;
        auto first = engine.openDocument(QStringLiteral(MERVIN_FORM_PDF));
        auto second = engine.openDocument(QStringLiteral(MERVIN_LONG_PDF));
        QVERIFY(first && second);
        ViewerWidget viewer(&engine);
        viewer.resize(600, 600);
        QSemaphore entered, release;
        std::atomic<bool> once{true};
        ViewerQueries::testing::observe([&](bool edit, int page) {
            if (!edit && page == 0 && once.exchange(false)) { entered.release(); release.acquire(); }
        });
        const auto unblock = qScopeGuard([&] { release.release(100); ViewerQueries::testing::observe({}); });
        viewer.setDocument(first.get());
        viewer.show();
        QTRY_VERIFY(entered.available() == 1);
        QTest::mouseClick(viewer.viewport(), Qt::LeftButton, {}, {100, 100});
        viewer.setDocument(second.get());
        release.release();
        bool delivered = false;
        QTimer::singleShot(0, [&] { delivered = true; });
        QTRY_VERIFY(delivered);
        QCOMPARE(viewer.document(), second.get());
        QVERIFY(!viewer.hasFormFields());
        QTRY_VERIFY(viewer.findChildren<QLineEdit *>().empty());
        QVERIFY(!viewer.hasSelection());
        viewer.setDocument(nullptr);
    }

    void selectAllCopiesUnloadedPages()
    {
        RenderEngine engine;
        auto doc = engine.openDocument(QStringLiteral(MERVIN_LONG_PDF));
        QVERIFY(doc);
        ViewerWidget viewer(&engine);
        viewer.resize(800, 600);
        viewer.setDocument(doc.get());
        viewer.show();
        QApplication::clipboard()->setText(QStringLiteral("before"));
        viewer.selectAll();
        viewer.copySelection();
        QTRY_VERIFY_WITH_TIMEOUT(QApplication::clipboard()->text() != QStringLiteral("before"), 15000);
        QCOMPARE(QApplication::clipboard()->text().count(QStringLiteral("STANDARD TEST")), doc->pageCount());
        viewer.setDocument(nullptr);
    }

    void keyboardTraversalStartsAtClickedField()
    {
        RenderEngine engine;
        auto doc = engine.openDocument(QStringLiteral(MERVIN_FORM_PDF));
        QVERIFY(doc);
        ViewerWidget viewer(&engine);
        viewer.resize(800, 700);
        viewer.setDocument(doc.get());
        viewer.setZoomMode(ViewerWidget::ZoomMode::FitPage);
        viewer.show();
        viewer.goToPage(2);
        viewer.activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(&viewer));
        const auto field = [&]() -> QLineEdit * {
            for (auto *editor : viewer.findChildren<QLineEdit *>())
                if (editor->isVisible() && editor->text() == QStringLiteral("value-2")) return editor;
            return nullptr;
        };
        QTRY_VERIFY(field());
        QTest::mouseClick(field(), Qt::LeftButton);
        QTest::keyClick(field(), Qt::Key_Tab);
        const auto focusedText = [] {
            const auto *editor = qobject_cast<QLineEdit *>(QApplication::focusWidget());
            return editor ? editor->text() : QString();
        };
        QTRY_COMPARE(focusedText(), QStringLiteral("value-3"));
        QTest::keyClick(QApplication::focusWidget(), Qt::Key_Tab);
        QTRY_COMPARE(focusedText(), QStringLiteral("value-0"));
        QTest::keyClick(QApplication::focusWidget(), Qt::Key_Backtab);
        QTRY_COMPARE(focusedText(), QStringLiteral("value-3"));
        viewer.setDocument(nullptr);
    }

    void sidebarAndKeyboardTraversalFinishAfterStall()
    {
        RenderEngine engine;
        auto doc = engine.openDocument(QStringLiteral(MERVIN_FORM_PDF));
        QVERIFY(doc);
        ViewerWidget viewer(&engine);
        viewer.resize(800, 700);
        viewer.setDocument(doc.get());
        viewer.show();
        viewer.activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(&viewer));
        QTRY_VERIFY(!viewer.findChildren<QLineEdit *>().empty());
        QSemaphore entered, release;
        std::atomic<bool> once{true};
        ViewerQueries::testing::observe([&](bool edit, int key) {
            if (!edit && key == -3 && once.exchange(false)) { entered.release(); release.acquire(); }
        });
        const auto unblock = qScopeGuard([&] { release.release(100); ViewerQueries::testing::observe({}); });
        QVERIFY(viewer.allAnnotations().empty());
        QTRY_VERIFY(entered.available() == 1);
        viewer.stopAnnotationListing();
        QVERIFY(viewer.allAnnotations().empty()); // reopen while the old page read is stalled
        QTest::keyClick(&viewer, Qt::Key_Tab);
        bool delivered = false;
        QTimer::singleShot(0, [&] { delivered = true; });
        QTRY_VERIFY(delivered);
        release.release();
        QTRY_COMPARE(viewer.allAnnotations().size(), size_t(doc->pageCount()));
        QTRY_VERIFY(qobject_cast<QLineEdit *>(QApplication::focusWidget()));
        viewer.setDocument(nullptr);
    }
};

QTEST_MAIN(TstViewerQueries)
#include "tst_viewer_queries.moc"
