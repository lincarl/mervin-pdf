#include "config/ConfigPaths.h"
#include "render/AnnotModel.h"
#include "render/MeasureContent.h"
#include "render/RenderEngine.h"
#include "security/MeasureExport.h"
#include "ui/MainWindow.h"
#include "ui/TabPage.h"
#include "ui/ViewerWidget.h"

#include <QFile>
#include <QMessageBox>
#include <QPushButton>
#include <QTemporaryDir>
#include <QTimer>
#include <QtTest>

using namespace mervin;

class TstDocumentWorkflow : public QObject
{
    Q_OBJECT
private slots:
    void init() { ConfigPaths::setOverrideDir(profile_.path()); }
    void cleanup() { ConfigPaths::setOverrideDir({}); }
    void closingDirtyTabCanBeCanceled()
    {
        RenderEngine engine;
        MainWindow window(&engine, nullptr);
        QVERIFY(window.openFile(QStringLiteral(MERVIN_FIXTURE_PDF)));
        auto *tab = window.findChild<TabPage *>();
        QVERIFY(tab);
        auto *model = tab->viewer()->annotModel();
        QVERIFY(model->addTextNote(0, {50, 50}, Qt::yellow, {}, "pending") >= 0);
        QTimer::singleShot(0, [] {
            auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
            QVERIFY(box);
            box->button(QMessageBox::Cancel)->click();
        });
        QVERIFY(QMetaObject::invokeMethod(&window, "closeTab", Q_ARG(int, 0)));
        QCOMPARE(window.tabCount(), 1);
        QVERIFY(tab->viewer()->hasUnsavedEdits());
        QTimer::singleShot(0, [] {
            auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
            QVERIFY(box);
            box->button(QMessageBox::Discard)->click();
        });
        QVERIFY(QMetaObject::invokeMethod(&window, "closeTab", Q_ARG(int, 0)));
        QCOMPARE(window.tabCount(), 0);
    }

    void saveDeletionOfLastMeasurement()
    {
        QTemporaryDir files;
        const QString path = files.filePath("measured.pdf");
        MeasureDoc data;
        data.measurements.push_back({0, MeasureKind::Distance, {{10, 10}, {80, 10}}});
        QCOMPARE(MeasureExport::embedMervin(QStringLiteral(MERVIN_FIXTURE_PDF), path, data, {}),
                 MeasureExport::Status::Ok);
        RenderEngine engine;
        MainWindow window(&engine, nullptr);
        QVERIFY(window.openFile(path));
        auto *tab = window.findChild<TabPage *>();
        QVERIFY(tab);
        auto *viewer = tab->viewer();
        QVERIFY(!viewer->hasMeasurementEdits());
        viewer->clearMeasurements();
        QVERIFY(viewer->hasMeasurementEdits());
        QVERIFY(QMetaObject::invokeMethod(&window, "saveMeasurements"));
        QVERIFY(!viewer->hasUnsavedEdits());
        auto blob = MeasureExport::readMervinBlob(path);
        QVERIFY(blob);
        MeasureDoc saved;
        QVERIFY(parseMeasurements(*blob, &saved));
        QVERIFY(saved.measurements.empty());
        QVERIFY(saved.pageScales.empty());
        QVERIFY(QMetaObject::invokeMethod(&window, "closeTab", Q_ARG(int, 0)));
    }

    void failedSaveRetainsEditableSnapshot()
    {
        QTemporaryDir files;
        const QString path = files.filePath("read-only.pdf");
        QVERIFY(QFile::copy(QStringLiteral(MERVIN_FIXTURE_PDF), path));
        RenderEngine engine;
        MainWindow window(&engine, nullptr);
        QVERIFY(window.openFile(path));
        auto *tab = window.findChild<TabPage *>();
        QVERIFY(tab);
        QVERIFY(tab->viewer()->annotModel()->addTextNote(0, {40, 40}, Qt::yellow, {}, "retained") >= 0);
        const auto permissions = QFile::permissions(path);
        QVERIFY(QFile::setPermissions(path, QFileDevice::ReadOwner | QFileDevice::ReadUser
                                            | QFileDevice::ReadGroup | QFileDevice::ReadOther));
        QTimer::singleShot(0, [] {
            auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
            QVERIFY(box);
            box->accept();
        });
        QVERIFY(QMetaObject::invokeMethod(&window, "saveMeasurements"));
        QVERIFY(tab->hasRecoverySnapshot());
        QCOMPARE(tab->path(), path);
        QVERIFY(tab->viewer()->document());
        QCOMPARE(tab->viewer()->annotModel()->pageAnnots(0).size(), 1u);
        QVERIFY(QFile::setPermissions(path, permissions));
        QVERIFY(QMetaObject::invokeMethod(&window, "saveMeasurements"));
        QVERIFY(!tab->hasRecoverySnapshot());
        auto saved = engine.openDocument(path);
        QVERIFY(saved);
        QCOMPARE(AnnotModel(*saved).pageAnnots(0)[0].contents, QStringLiteral("retained"));
    }

    void documentSearchReplacedAndClosed()
    {
        RenderEngine engine;
        auto doc = engine.openDocument(QStringLiteral(MERVIN_FIXTURE_PDF));
        QVERIFY(doc);
        ViewerWidget viewer(&engine);
        viewer.setDocument(doc.get());
        viewer.startFind("STANDARD", false, false);
        viewer.startFind("absent", false, false);
        QTest::qWait(100);
        QCOMPARE(viewer.matchCount(), 0);
        viewer.startFind("STANDARD", false, false);
        QTRY_COMPARE(viewer.matchCount(), doc->pageCount());
        viewer.startFind("The", false, false);
        viewer.setDocument(nullptr);
        doc.reset();
        QTest::qWait(100);
        QCOMPARE(viewer.matchCount(), 0);
    }

private:
    QTemporaryDir profile_;
};

QTEST_MAIN(TstDocumentWorkflow)
#include "tst_document_workflow.moc"
