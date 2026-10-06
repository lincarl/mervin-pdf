#include "config/ConfigPaths.h"
#include "config/Settings.h"
#include "render/AnnotModel.h"
#include "render/Document.h"
#include "render/FormModel.h"
#include "render/MeasureContent.h"
#include "render/RenderEngine.h"
#include "security/QpdfService.h"
#include "security/DocumentOutput.h"
#include "ui/FindCard.h"
#include "ui/MainWindow.h"
#include "ui/TabPage.h"
#include "ui/ThumbnailSidebar.h"
#include "ui/ViewerWidget.h"

#include <QAction>
#include <QFile>
#include <QFileInfo>
#include <QDateTime>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSignalSpy>
#include <QTabBar>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QtTest>

using namespace mervin;

namespace {
// A self-contained AcroForm exercises pending editor text as well as saved PDF objects.
QByteArray formPdf()
{
    const QList<QByteArray> objects{
        "<< /Type /Catalog /Pages 2 0 R /AcroForm << /Fields [4 0 R] /DR << /Font << "
        "/Helv 5 0 R >> >> /DA (/Helv 12 Tf 0 g) >> >>",
        "<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
        "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] /Annots [4 0 R] "
        "/Resources << /Font << /Helv 5 0 R >> >> >>",
        "<< /Type /Annot /Subtype /Widget /FT /Tx /T (name) /Rect [20 740 280 770] "
        "/F 4 /DA (/Helv 12 Tf 0 g) /V () /P 3 0 R >>",
        "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>"};
    QByteArray pdf = "%PDF-1.7\n";
    QList<qsizetype> offsets;
    for (qsizetype i = 0; i < objects.size(); ++i) {
        offsets.append(pdf.size());
        pdf += QByteArray::number(i + 1) + " 0 obj\n" + objects.at(i) + "\nendobj\n";
    }
    const qsizetype start = pdf.size();
    pdf += "xref\n0 6\n0000000000 65535 f \n";
    for (qsizetype offset : offsets)
        pdf += QByteArray::number(offset).rightJustified(10, '0') + " 00000 n \n";
    pdf += "trailer\n<< /Size 6 /Root 1 0 R >>\nstartxref\n" + QByteArray::number(start)
           + "\n%%EOF\n";
    return pdf;
}

QByteArray readFile(const QString &path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

bool writeFile(const QString &path, const QByteArray &bytes)
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
}

void showForLayout(QWidget &widget)
{
    widget.setAttribute(Qt::WA_DontShowOnScreen);
    widget.resize(1000, 800);
    widget.show();
    QCoreApplication::processEvents();
}
} // namespace

class TstDocumentSuspension : public QObject
{
    Q_OBJECT

private slots:
    void init()
    {
        profile_ = std::make_unique<QTemporaryDir>();
        QVERIFY(profile_->isValid());
        ConfigPaths::setOverrideDir(profile_->path());
        Settings settings;
        settings.autoUpdate = false;
        QVERIFY(settings.save());
    }

    void cleanup()
    {
        ConfigPaths::setOverrideDir({});
        profile_.reset();
    }

    void cleanDocumentReleasesItsOwnerAndRestoresTheView()
    {
        RenderEngine engine;
        TabPage tab(&engine);
        showForLayout(tab);
        QVERIFY(tab.open(QStringLiteral(MERVIN_FIXTURE_PDF)));
        ViewerWidget *viewer = tab.viewer();
        viewer->setZoomEaseMs(0);
        // A search lives in the tab's find card; one restored behind a closed card
        // is dropped (TabPage::restoreViewer), so open it as a user would.
        tab.findCard()->open();
        viewer->startFind(QStringLiteral("STANDARD"), true, true);
        QTRY_COMPARE(viewer->matchCount(), 4);
        viewer->setLayoutMode({ViewLayout::Scroll::Single, true});
        viewer->rotateRight();
        viewer->setScale(1.75);
        viewer->goToPage(2);
        viewer->restorePageScrollFraction(2, 0.12, 0.18);
        QCoreApplication::processEvents();
        const auto anchor = viewer->scrollAnchor();
        const auto lifetime = viewer->document()->lifetime();
        const auto mode = viewer->layoutMode();
        const QString path = tab.path();
        QString error;

        QVERIFY2(tab.suspend(&error), qPrintable(error));
        QVERIFY(tab.isSuspended());
        QVERIFY(!tab.isLoaded());
        QVERIFY(!viewer->document());
        QVERIFY(!tab.hasRecoverySnapshot());
        QVERIFY(!tab.hasUnsavedEdits());
        QCOMPARE(tab.path(), path);
        {
            std::lock_guard lock(lifetime->mutex);
            QVERIFY(!lifetime->document);
        }

        QSignalSpy resumed(&tab, &TabPage::resumeFinished);
        tab.resumeAsync();
        QTRY_COMPARE_WITH_TIMEOUT(resumed.size(), 1, 10000);
        QVERIFY2(resumed.first().at(0).toBool(), qPrintable(resumed.first().at(1).toString()));
        QVERIFY(tab.isLoaded());
        QVERIFY(!tab.isLoading());
        QCOMPARE(viewer->layoutMode(), mode);
        QCOMPARE(viewer->rotation(), 90);
        QCOMPARE(viewer->zoomMode(), ViewerWidget::ZoomMode::Custom);
        QCOMPARE(viewer->scale(), 1.75);
        QCOMPARE(viewer->findQuery(), QStringLiteral("STANDARD"));
        QVERIFY(viewer->findCaseSensitive());
        QVERIFY(viewer->findWholeWord());
        QTRY_COMPARE(viewer->matchCount(), 4);
        const auto restored = viewer->scrollAnchor();
        QCOMPARE(restored.page, anchor.page);
        QVERIFY(std::abs(restored.fracX - anchor.fracX) < 0.01);
        QVERIFY(std::abs(restored.fracY - anchor.fracY) < 0.01);
    }

    void editedDocumentRoundTrip_data()
    {
        QTest::addColumn<bool>("encrypted");
        QTest::newRow("plain") << false;
        QTest::newRow("encrypted") << true;
    }

    void editedDocumentRoundTrip()
    {
        QFETCH(bool, encrypted);
        QTemporaryDir files;
        const QString plain = files.filePath(QStringLiteral("plain.pdf"));
        QVERIFY(writeFile(plain, formPdf()));
        const QString password = encrypted ? QStringLiteral("test-suspension-password") : QString();
        const QString path = encrypted ? files.filePath(QStringLiteral("protected.pdf")) : plain;
        if (encrypted) {
            QpdfService service;
            QCOMPARE(service.encrypt(plain, path, {}, password, password,
                                     QpdfService::Algorithm::AES256, {}), QpdfService::Status::Ok);
        }
        const QByteArray original = readFile(path);
        RenderEngine engine;
        QString checkpoint;
        {
            TabPage tab(&engine);
            showForLayout(tab);
            QString error;
            QVERIFY2(tab.open(path, password, &error), qPrintable(error));
            ViewerWidget *viewer = tab.viewer();
            QVERIFY(viewer->formModel());
            viewer->setFormMode(true);
            QTRY_VERIFY(!viewer->viewport()->findChildren<QLineEdit *>(Qt::FindDirectChildrenOnly).isEmpty());
            auto *formEditor = viewer->viewport()->findChild<QLineEdit *>(Qt::FindDirectChildrenOnly);
            QVERIFY(formEditor);
            formEditor->setText(QStringLiteral("Uncommitted field value"));

            const int note = viewer->annotModel()->addTextNote(
                0, {80, 100}, Qt::yellow, QStringLiteral("Tester"), QStringLiteral("Before edit"));
            QVERIFY(note >= 0);
            viewer->loadMeasurements({{0, MeasureKind::Distance, {{30, 200}, {160, 200}}}},
                                     {}, MeasureUnit::Millimeter, 2, 2.0);
            viewer->setPageScaleOverride(0, MeasureModel::fromRatio(25));
            viewer->setCommentToolEnabled(true);
            viewer->setAnnotSubMode(AnnotSubMode::Note);
            viewer->revealAnnotation(0, note);
            auto *comment = viewer->viewport()->findChild<QPlainTextEdit *>();
            QVERIFY(comment);
            comment->setPlainText(QStringLiteral("Uncommitted comment value"));

            QVERIFY(tab.hasUnsavedEdits());
            QVERIFY2(tab.suspend(&error), qPrintable(error));
            QVERIFY(tab.isSuspended());
            QVERIFY(tab.hasRecoverySnapshot());
            QVERIFY(tab.hasUnsavedEdits());
            checkpoint = tab.recoveryPath();
            QVERIFY(QFileInfo::exists(checkpoint));
            QCOMPARE(readFile(path), original);

            if (encrypted) {
                bool needsPassword = false;
                QVERIFY(!engine.openDocument(checkpoint, {}, &error, &needsPassword));
                QVERIFY(needsPassword);
            }
            QVERIFY2(tab.resume(&error), qPrintable(error));
            QCOMPARE(tab.path(), path);
            QCOMPARE(tab.password(), password);
            QVERIFY(tab.hasUnsavedEdits());
            QCOMPARE(viewer->formModel()->pageFields(0).at(0).value,
                     QStringLiteral("Uncommitted field value"));
            const auto notes = viewer->annotModel()->pageAnnots(0);
            QCOMPARE(notes.size(), 1u);
            QCOMPARE(notes.front().contents, QStringLiteral("Uncommitted comment value"));
            QCOMPARE(viewer->committedMeasurements().size(), 1u);
            QCOMPARE(viewer->committedMeasurements().front().pts.at(1), QPointF(160, 200));
            QCOMPARE(viewer->measureOverrides().override(0).label, QStringLiteral("1:25"));
            QCOMPARE(readFile(path), original);

            // A second unload must retain the dirty marker even without further edits.
            QVERIFY2(tab.suspend(&error), qPrintable(error));
            QVERIFY(tab.hasUnsavedEdits());
            QVERIFY2(tab.resume(&error), qPrintable(error));
            QVERIFY(tab.hasUnsavedEdits());
            QCOMPARE(viewer->formModel()->pageFields(0).at(0).value,
                     QStringLiteral("Uncommitted field value"));
            checkpoint = tab.recoveryPath();
        }
        QVERIFY(!QFileInfo::exists(checkpoint));
        QCOMPARE(readFile(path), original);
    }

    void unloadCommitsTheActiveFormEditor()
    {
        const QString path = profile_->filePath(QStringLiteral("form.pdf"));
        QVERIFY(writeFile(path, formPdf()));
        RenderEngine engine;
        TabPage tab(&engine);
        showForLayout(tab);
        QVERIFY(tab.open(path));
        tab.viewer()->setFormMode(true);
        QTRY_VERIFY(!tab.viewer()->viewport()->findChildren<QLineEdit *>(Qt::FindDirectChildrenOnly).isEmpty());
        auto *editor = tab.viewer()->viewport()->findChild<QLineEdit *>(Qt::FindDirectChildrenOnly);
        QVERIFY(editor);
        editor->setText(QStringLiteral("Text still being typed"));
        QVERIFY(tab.viewer()->formModel()->pageFields(0).front().value.isEmpty());
        QVERIFY(tab.suspend());
        QVERIFY(tab.hasRecoverySnapshot());
        QVERIFY(tab.hasUnsavedEdits());
        QVERIFY(tab.resume());
        QCOMPARE(tab.viewer()->formModel()->pageFields(0).front().value,
                 QStringLiteral("Text still being typed"));
    }

    void unfinishedMeasurementRemainsEditableAfterReload()
    {
        RenderEngine engine;
        TabPage tab(&engine);
        showForLayout(tab);
        QVERIFY(tab.open(QStringLiteral(MERVIN_FIXTURE_PDF)));
        ViewerWidget *viewer = tab.viewer();
        viewer->setZoomEaseMs(0);
        viewer->setScale(1.0);
        viewer->setPageScaleOverride(0, MeasureModel::fromRatio(20));
        viewer->setMeasureMode(true);
        viewer->setMeasureKind(MeasureKind::Polyline);
        viewer->setMeasureSnap(false);
        const QPoint first(450, 300);
        QCOMPARE(viewer->pageUnder(first), 0);
        QTest::mouseClick(viewer->viewport(), Qt::LeftButton, Qt::NoModifier, first);
        const auto before = viewer->captureResumeState();
        QCOMPARE(before.inProgress.size(), 1u);
        QVERIFY(tab.suspend());
        QVERIFY(tab.hasRecoverySnapshot());
        QVERIFY(tab.resume());
        QVERIFY(viewer->measureMode());
        QCOMPARE(viewer->captureResumeState().inProgress, before.inProgress);
        QCOMPARE(viewer->captureResumeState().inProgressPage, before.inProgressPage);
        QTest::mouseClick(viewer->viewport(), Qt::LeftButton, Qt::NoModifier, QPoint(550, 350));
        QCOMPARE(viewer->captureResumeState().inProgress.size(), 2u);
    }

    void disabledFormToolStaysDisabledAfterReload()
    {
        const QString path = profile_->filePath(QStringLiteral("form.pdf"));
        QVERIFY(writeFile(path, formPdf()));
        RenderEngine engine;
        TabPage tab(&engine);
        showForLayout(tab);
        tab.viewer()->setAutoFormFill(true);
        QVERIFY(tab.open(path));
        QVERIFY(tab.viewer()->formMode());
        tab.viewer()->setFormMode(false);
        QVERIFY(tab.suspend());
        QVERIFY(tab.resume());
        QVERIFY(!tab.viewer()->formMode());
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QVERIFY(tab.viewer()->viewport()->findChildren<QLineEdit *>(Qt::FindDirectChildrenOnly).isEmpty());
    }

    void backgroundRestoreRemainsUnloadedUntilRequested()
    {
        RenderEngine engine;
        TabPage tab(&engine);
        ViewState saved;
        saved.page = 2;
        saved.scale = 1.5;
        saved.zoomMode = QStringLiteral("custom");
        tab.initializeSuspended(QStringLiteral(MERVIN_FIXTURE_PDF), saved);
        QVERIFY(tab.isSuspended());
        QVERIFY(!tab.viewer()->document());
        QCOMPARE(tab.capturedViewState().page, 2);
        QCOMPARE(tab.tabTitle(), QStringLiteral("properties.pdf"));
        QCoreApplication::processEvents();
        QVERIFY(!tab.isLoaded());

        QSignalSpy resumed(&tab, &TabPage::resumeFinished);
        tab.resumeAsync();
        tab.resumeAsync(); // Selecting a loading tab again must not start another open.
        QTRY_COMPARE_WITH_TIMEOUT(resumed.size(), 1, 10000);
        QVERIFY(resumed.first().at(0).toBool());
        QVERIFY(tab.isLoaded());
        QCOMPARE(tab.viewer()->scale(), 1.5);
        QCOMPARE(tab.viewer()->currentPage(), 2);
    }

    void missingFileKeepsTheTabAndCanBeRetried()
    {
        QTemporaryDir files;
        const QString path = files.filePath(QStringLiteral("missing.pdf"));
        QVERIFY(QFile::copy(QStringLiteral(MERVIN_FIXTURE_PDF), path));
        RenderEngine engine;
        TabPage tab(&engine);
        QVERIFY(tab.open(path));
        QVERIFY(tab.suspend());
        QVERIFY(QFile::remove(path));
        QSignalSpy resumed(&tab, &TabPage::resumeFinished);
        tab.resumeAsync();
        QTRY_COMPARE_WITH_TIMEOUT(resumed.size(), 1, 10000);
        QVERIFY(!resumed.first().at(0).toBool());
        QVERIFY(!resumed.first().at(1).toString().isEmpty());
        QVERIFY(tab.isSuspended());
        QCOMPARE(tab.path(), path);
        QVERIFY(QFile::copy(QStringLiteral(MERVIN_FIXTURE_PDF), path));
        tab.resumeAsync();
        QTRY_COMPARE_WITH_TIMEOUT(resumed.size(), 2, 10000);
        QVERIFY(resumed.last().at(0).toBool());
        QVERIFY(tab.isLoaded());
    }

    void unloadedTabKeepsItsCloseShortcutEnabled()
    {
        RenderEngine engine;
        MainWindow window(&engine, nullptr);
        QVERIFY(window.openFile(profile_->filePath(QStringLiteral("missing.pdf")),
                                false, -1, true, {}, true));
        auto *tab = window.findChild<TabPage *>();
        QVERIFY(tab);
        QVERIFY(tab->isSuspended());
        QAction *closeTab = nullptr;
        const QKeySequence shortcut(Qt::CTRL | Qt::Key_W);
        for (QAction *action : window.findChildren<QAction *>())
            if (action->shortcuts().contains(shortcut))
                closeTab = action;
        QVERIFY(closeTab);
        QVERIFY(closeTab->isEnabled());
        closeTab->trigger();
        QCOMPARE(window.tabCount(), 0);
        QVERIFY(!closeTab->isEnabled());
    }

    void failedCheckpointKeepsTheEditedDocumentLoaded()
    {
        RenderEngine engine;
        TabPage tab(&engine);
        QVERIFY(tab.open(QStringLiteral(MERVIN_FIXTURE_PDF)));
        QVERIFY(tab.viewer()->annotModel()->addTextNote(
                    0, {60, 60}, Qt::yellow, {}, QStringLiteral("Must remain editable")) >= 0);
        const QString blocker = profile_->filePath(QStringLiteral("regular-file"));
        QVERIFY(writeFile(blocker, QByteArrayLiteral("Not a directory")));
        ConfigPaths::setOverrideDir(blocker);
        QString error;
        QVERIFY(!tab.suspend(&error));
        QVERIFY(!error.isEmpty());
        QVERIFY(tab.isLoaded());
        QVERIFY(tab.hasUnsavedEdits());
        QCOMPARE(tab.viewer()->annotModel()->pageAnnots(0).front().contents,
                 QStringLiteral("Must remain editable"));
        ConfigPaths::setOverrideDir(profile_->path());
        QVERIFY2(tab.suspend(&error), qPrintable(error));
        QVERIFY(tab.isSuspended());
        QVERIFY(tab.hasRecoverySnapshot());
    }

    void failedCheckpointKeepsCurrentDocumentThumbnails()
    {
        RenderEngine engine;
        MainWindow window(&engine, nullptr);
        showForLayout(window);
        QVERIFY(window.openFile(QStringLiteral(MERVIN_FIXTURE_PDF)));
        auto *tab = window.findChild<TabPage *>();
        auto *sidebar = window.findChild<ThumbnailSidebar *>();
        QVERIFY(tab);
        QVERIFY(sidebar);
        auto *rows = sidebar->findChild<QListWidget *>();
        QVERIFY(rows);
        QCOMPARE(rows->count(), tab->viewer()->pageCount());
        const int pageCount = rows->count();
        QVERIFY(tab->viewer()->annotModel()->addTextNote(
                    0, {60, 60}, Qt::yellow, {}, QStringLiteral("Keep the document loaded")) >= 0);
        const QString blocker = profile_->filePath(QStringLiteral("regular-file"));
        QVERIFY(writeFile(blocker, QByteArrayLiteral("Not a directory")));
        ConfigPaths::setOverrideDir(blocker);
        window.showMinimized();
        window.updateDocumentActivity(0, 30);
        window.updateDocumentActivity(31 * 60 * 1000, 30);
        ConfigPaths::setOverrideDir(profile_->path());
        QVERIFY(tab->isLoaded());
        QVERIFY(tab->hasUnsavedEdits());
        QCOMPARE(rows->count(), pageCount);
        window.showNormal();
        QCoreApplication::processEvents();
        QCOMPARE(rows->count(), pageCount);
    }

    void modifiedOriginalStillConflictsAfterCheckpointReload()
    {
        QTemporaryDir files;
        const QString source = files.filePath(QStringLiteral("source.pdf"));
        const QByteArray original = formPdf();
        QVERIFY(writeFile(source, original));
        RenderEngine engine;
        TabPage tab(&engine);
        QVERIFY(tab.open(source));
        QVERIFY(!tab.sourceChangedOnDisk());
        QVERIFY(tab.viewer()->annotModel()->addTextNote(
                    0, {40, 40}, Qt::yellow, {}, QStringLiteral("Keep my edit")) >= 0);
        QVERIFY(tab.suspend());
        QVERIFY(tab.resume());
        QVERIFY(!tab.sourceChangedOnDisk());
        QVERIFY(tab.suspend());

        // A same-size external edit must conflict even while our live source is a checkpoint.
        QByteArray changed = original;
        changed.replace("/T (name)", "/T (town)");
        QCOMPARE(changed.size(), original.size());
        const QDateTime modified = QFileInfo(source).lastModified().addSecs(5);
        QVERIFY(writeFile(source, changed));
        QFile file(source);
        QVERIFY(file.open(QIODevice::ReadWrite));
        QVERIFY(file.setFileTime(modified, QFileDevice::FileModificationTime));
        file.close();
        QVERIFY(tab.sourceChangedOnDisk());
        QVERIFY(tab.resume());
        QVERIFY(tab.sourceChangedOnDisk());
        QCOMPARE(tab.viewer()->annotModel()->pageAnnots(0).front().contents,
                 QStringLiteral("Keep my edit"));
        QCOMPARE(readFile(source), changed);
    }

    void recoveryManifestRestoresEditedDocumentWithoutOriginal_data()
    {
        QTest::addColumn<bool>("encrypted");
        QTest::newRow("plain") << false;
        QTest::newRow("encrypted") << true;
    }

    void recoveryManifestRestoresEditedDocumentWithoutOriginal()
    {
        QFETCH(bool, encrypted);
        QTemporaryDir files;
        const QString plain = files.filePath(QStringLiteral("plain.pdf"));
        QVERIFY(writeFile(plain, formPdf()));
        const QString password = encrypted ? QStringLiteral("recovery-test-password") : QString();
        const QString source = encrypted ? files.filePath(QStringLiteral("encrypted.pdf")) : plain;
        if (encrypted) {
            QpdfService service;
            QCOMPARE(service.encrypt(plain, source, {}, password, password,
                                     QpdfService::Algorithm::AES256, {}), QpdfService::Status::Ok);
        }
        RenderEngine engine;
        QString checkpoint;
        QByteArray checkpointBytes;
        QByteArray manifestBytes;
        {
            TabPage tab(&engine);
            showForLayout(tab);
            QVERIFY(tab.open(source, password));
            tab.viewer()->setFormMode(false);
            tab.viewer()->setScale(1.75);
            tab.viewer()->rotateRight();
            tab.viewer()->setLayoutMode({ViewLayout::Scroll::Single, true});
            QVERIFY(tab.viewer()->annotModel()->addTextNote(
                        0, {70, 70}, Qt::yellow, {}, QStringLiteral("Recovered edit")) >= 0);
            QVERIFY(tab.suspend());
            checkpoint = tab.recoveryPath();
            checkpointBytes = readFile(checkpoint);
            manifestBytes = readFile(checkpoint + QStringLiteral(".json"));
            QVERIFY(!checkpointBytes.isEmpty());
            QVERIFY(!manifestBytes.isEmpty());
            if (encrypted)
                QVERIFY(!manifestBytes.contains(password.toUtf8()));
        }
        QVERIFY(!QFileInfo::exists(checkpoint));
        QVERIFY(!QFileInfo::exists(checkpoint + QStringLiteral(".json")));
        // Recreate the exact files a terminated process leaves, with no in-memory owner.
        QVERIFY(writeFile(checkpoint, checkpointBytes));
        QVERIFY(writeFile(checkpoint + QStringLiteral(".json"), manifestBytes));
        QVERIFY(QFile::remove(source));
        QCOMPARE(TabPage::recoverablePaths().count(source), 1);
        {
            TabPage restored(&engine);
            showForLayout(restored);
            restored.initializeSuspended(source);
            QVERIFY(restored.isSuspended());
            QVERIFY(restored.hasUnsavedEdits());
            QCOMPARE(restored.recoveryPath(), checkpoint);
            QVERIFY(restored.password().isEmpty());
            QCOMPARE(restored.capturedViewState().scale, 1.75);
            QCOMPARE(restored.capturedViewState().rotation, 90);
            ViewState stale;
            stale.scale = 0.5;
            restored.setSavedViewState(stale);
            QCOMPARE(restored.capturedViewState().scale, 1.75);
            QString error;
            bool needsPassword = false;
            if (encrypted) {
                QVERIFY(!restored.resume(&error, &needsPassword));
                QVERIFY(needsPassword);
                QVERIFY(QFileInfo::exists(checkpoint));
                restored.setPassword(password);
            }
            QVERIFY2(restored.resume(&error, &needsPassword), qPrintable(error));
            QVERIFY(!needsPassword);
            QVERIFY(restored.hasUnsavedEdits());
            QVERIFY(QFileInfo::exists(checkpoint));
            QVERIFY(!restored.viewer()->formMode());
            QCOMPARE(restored.viewer()->layoutMode(), (ViewLayout::Mode{ViewLayout::Scroll::Single, true}));
            QCOMPARE(restored.viewer()->rotation(), 90);
            QCOMPARE(restored.viewer()->scale(), 1.75);
            QCOMPARE(restored.viewer()->annotModel()->pageAnnots(0).front().contents,
                     QStringLiteral("Recovered edit"));
            QVERIFY(restored.sourceChangedOnDisk());
        }
        QVERIFY(TabPage::recoverablePaths().isEmpty());
    }

    void failedSaveSnapshotGetsARecoverablePrivateCopy()
    {
        QTemporaryDir files;
        const QString source = files.filePath(QStringLiteral("source.pdf"));
        QVERIFY(writeFile(source, formPdf()));
        const QByteArray original = readFile(source);
        const QString staged = files.filePath(QStringLiteral(".mervin-save-edited.pdf"));
        RenderEngine engine;
        TabPage tab(&engine);
        QVERIFY(tab.open(source));
        tab.viewer()->setFormMode(false);
        tab.viewer()->setScale(1.5);
        QVERIFY(tab.viewer()->annotModel()->addTextNote(
                    0, {80, 80}, Qt::yellow, {}, QStringLiteral("Save failed, keep this")) >= 0);
        QString error;
        QVERIFY2(DocumentOutput::snapshot(*tab.viewer()->document(), tab.viewer()->measurementDocument(),
                                          staged, {}, &error), qPrintable(error));
        tab.detachDocument();
        QVERIFY2(tab.recoverSnapshot(staged, &error), qPrintable(error));
        QVERIFY(tab.isLoaded());
        QVERIFY(tab.hasUnsavedEdits());
        QVERIFY(tab.recoveryPath() != staged);
        QVERIFY(QFileInfo::exists(tab.recoveryPath() + QStringLiteral(".json")));
        QCOMPARE(TabPage::recoverablePaths().count(source), 1);
        QCOMPARE(readFile(source), original);
        QVERIFY(!tab.viewer()->formMode());
        QCOMPARE(tab.viewer()->scale(), 1.5);
        QVERIFY(!QFileInfo::exists(staged));
    }

    void unloadCancelsRunningSearchAndRender()
    {
        RenderEngine engine;
        TabPage tab(&engine);
        showForLayout(tab);
        QVERIFY(tab.open(QStringLiteral(MERVIN_FIXTURE_PDF)));
        tab.findCard()->open(); // the search must survive each reload, as with the card open
        for (int i = 0; i < 6; ++i) {
            tab.viewer()->setScale(1.0 + i * 0.5);
            tab.viewer()->viewport()->repaint();
            tab.viewer()->startFind(QStringLiteral("STANDARD"), false, false);
            const auto lifetime = tab.viewer()->document()->lifetime();
            QVERIFY(tab.suspend());
            {
                std::lock_guard lock(lifetime->mutex);
                QVERIFY(!lifetime->document);
            }
            QCoreApplication::processEvents();
            QVERIFY(!tab.viewer()->document());
            QVERIFY(tab.resume());
        }
        QTRY_COMPARE(tab.viewer()->matchCount(), 4);
        QVERIFY(!engine.renderPageImage(tab.viewer()->document(), 0, 1.0, 0).isNull());
    }

    void neverDisablesTimedAndTrayUnloading()
    {
        Settings settings = Settings::load();
        settings.unloadInactiveMinutes = 0;
        settings.closeToTray = true;
        QVERIFY(settings.save());
        RenderEngine engine;
        MainWindow window(&engine, nullptr);
        showForLayout(window);
        QVERIFY(window.openFile(QStringLiteral(MERVIN_FIXTURE_PDF)));
        QVERIFY(window.openFile(QStringLiteral(MERVIN_FIXTURE_PDF), true));
        const auto tabs = window.findChildren<TabPage *>();
        QCOMPARE(tabs.size(), 2);
        window.updateDocumentActivity(0, 0);
        window.updateDocumentActivity(24 * 60 * 60 * 1000, 0);
        for (TabPage *tab : tabs)
            QVERIFY(tab->isLoaded());
        window.hideToTray();
        QVERIFY(!window.isVisible());
        QCoreApplication::processEvents();
        for (TabPage *tab : tabs)
            QVERIFY(tab->isLoaded());
        window.restoreFromTray();
        QVERIFY(window.isVisible());
        for (TabPage *tab : tabs)
            QVERIFY(tab->isLoaded());
    }

    void timedUnloadKeepsShownDocumentAndResetsOnReturn()
    {
        RenderEngine engine;
        MainWindow window(&engine, nullptr);
        showForLayout(window);
        QVERIFY(window.openFile(QStringLiteral(MERVIN_FIXTURE_PDF)));
        QVERIFY(window.openFile(QStringLiteral(MERVIN_FIXTURE_PDF), true));
        auto *tabs = window.findChild<QTabWidget *>();
        QVERIFY(tabs);
        auto *first = qobject_cast<TabPage *>(tabs->widget(0));
        auto *second = qobject_cast<TabPage *>(tabs->widget(1));
        QVERIFY(first);
        QVERIFY(second);
        window.updateDocumentActivity(0, 30);
        window.updateDocumentActivity(29 * 60 * 1000, 30);
        QVERIFY(first->isLoaded());
        QVERIFY(second->isLoaded());
        tabs->setCurrentIndex(0);
        window.updateDocumentActivity(29 * 60 * 1000, 30);
        tabs->setCurrentIndex(1);
        window.updateDocumentActivity(29 * 60 * 1000 + 1, 30);
        window.updateDocumentActivity(31 * 60 * 1000, 30);
        QVERIFY(first->isLoaded());
        window.updateDocumentActivity(60 * 60 * 1000, 30);
        QVERIFY(first->isSuspended());
        QVERIFY(second->isLoaded());
        tabs->setCurrentIndex(0);
        QTRY_VERIFY_WITH_TIMEOUT(first->isLoaded(), 10000);
    }

    void trayUnloadsAllAndRestoreLoadsOnlySelectedTab()
    {
        RenderEngine engine;
        MainWindow window(&engine, nullptr);
        showForLayout(window);
        QVERIFY(window.openFile(QStringLiteral(MERVIN_FIXTURE_PDF)));
        QVERIFY(window.openFile(QStringLiteral(MERVIN_FIXTURE_PDF), true));
        auto *tabs = window.findChild<QTabWidget *>();
        QVERIFY(tabs);
        auto *first = qobject_cast<TabPage *>(tabs->widget(0));
        auto *second = qobject_cast<TabPage *>(tabs->widget(1));
        QVERIFY(first);
        QVERIFY(second);
        window.hideToTray();
        QVERIFY(!window.isVisible());
        QTRY_VERIFY(first->isSuspended());
        QTRY_VERIFY(second->isSuspended());
        window.restoreFromTray();
        QVERIFY(window.isVisible());
        QTRY_VERIFY_WITH_TIMEOUT(second->isLoaded(), 10000);
        QVERIFY(first->isSuspended());
    }

    void clickingAnUnloadedDocumentTabResumesIt_data()
    {
        QTest::addColumn<bool>("fromTray");
        QTest::addColumn<bool>("fromRecent");
        QTest::newRow("timed-unload") << false << false;
        QTest::newRow("tray-restored-inactive-tab") << true << false;
        QTest::newRow("recent-same-selected-tab") << false << true;
    }

    void clickingAnUnloadedDocumentTabResumesIt()
    {
        QFETCH(bool, fromTray);
        QFETCH(bool, fromRecent);
        RenderEngine engine;
        MainWindow window(&engine, nullptr);
        showForLayout(window);
        QVERIFY(window.openFile(QStringLiteral(MERVIN_FIXTURE_PDF)));
        auto *tabs = window.findChild<QTabWidget *>();
        auto *bar = window.findChild<QTabBar *>(QStringLiteral("docTabBar"));
        QVERIFY(tabs);
        QVERIFY(bar);
        auto *first = qobject_cast<TabPage *>(tabs->widget(0));
        QVERIFY(first);
        first->viewer()->setZoomEaseMs(0);
        first->viewer()->setLayoutMode({ViewLayout::Scroll::Single, false});
        first->viewer()->rotateRight();
        first->viewer()->setScale(1.5);
        first->viewer()->goToPage(2);
        QVERIFY(window.openFile(QStringLiteral(MERVIN_FIXTURE_PDF), true));
        QVERIFY(window.openFile(QStringLiteral(MERVIN_FIXTURE_PDF), true));
        auto *second = qobject_cast<TabPage *>(tabs->widget(1));
        auto *third = qobject_cast<TabPage *>(tabs->widget(2));
        QVERIFY(second);
        QVERIFY(third);
        QCoreApplication::processEvents();

        if (fromTray) {
            window.hideToTray();
            QTRY_VERIFY(first->isSuspended());
            QTRY_VERIFY(second->isSuspended());
            QTRY_VERIFY(third->isSuspended());
            window.restoreFromTray();
            QTRY_VERIFY_WITH_TIMEOUT(third->isLoaded(), 10000);
        } else {
            if (fromRecent) {
                tabs->setCurrentIndex(0);
                auto *recent = window.findChild<QPushButton *>(QStringLiteral("recentPillBtn"));
                QVERIFY(recent);
                QTest::mouseClick(recent, Qt::LeftButton);
                QVERIFY(recent->property("recentActive").toBool());
                QCoreApplication::processEvents(); // Finish the earlier tab selection before unloading.
            }
            window.updateDocumentActivity(0, 30);
            window.updateDocumentActivity(31 * 60 * 1000, 30);
        }
        QVERIFY(first->isSuspended());
        QVERIFY(second->isSuspended());
        QCOMPARE(third->isLoaded(), !fromRecent);
        QCOMPARE(bar->currentIndex(), fromRecent ? 0 : 2);

        auto *status = first->findChild<QLabel *>(QStringLiteral("documentLoadStatus"));
        QVERIFY(status);
        bool sawLoading = false;
        bool loadingWasVisible = false;
        QString loadingText;
        QObject loadingObserver;
        // Observe the transition synchronously so a fast load cannot hide the placeholder first.
        connect(first, &TabPage::stateChanged, &loadingObserver, [&] {
            if (first->isLoading()) {
                sawLoading = true;
                loadingWasVisible = status->isVisible();
                loadingText = status->text();
            }
        });
        QSignalSpy resumed(first, &TabPage::resumeFinished);
        QVERIFY(bar->isVisible());
        QTest::mouseClick(bar, Qt::LeftButton, Qt::NoModifier, bar->tabRect(0).center());
        QCOMPARE(tabs->currentIndex(), 0);
        QTRY_COMPARE_WITH_TIMEOUT(resumed.size(), 1, 10000);
        QVERIFY2(resumed.first().at(0).toBool(), qPrintable(resumed.first().at(1).toString()));
        QVERIFY(sawLoading);
        QVERIFY(loadingWasVisible);
        QCOMPARE(loadingText, QStringLiteral("Loading"));
        QVERIFY(first->isLoaded());
        QVERIFY(first->viewer()->isVisible());
        QVERIFY(!status->isVisible());
        QCOMPARE(first->viewer()->currentPage(), 2);
        QCOMPARE(first->viewer()->scale(), 1.5);
        QCOMPARE(first->viewer()->rotation(), 90);
        QVERIFY(second->isSuspended());
        QCOMPARE(third->isLoaded(), !fromRecent);
    }

    void losingFocusKeepsThePageButMinimizingStartsItsTimer()
    {
        RenderEngine engine;
        MainWindow window(&engine, nullptr);
        showForLayout(window);
        QVERIFY(window.openFile(QStringLiteral(MERVIN_FIXTURE_PDF)));
        auto *tab = window.findChild<TabPage *>();
        QVERIFY(tab);
        QEvent deactivated(QEvent::WindowDeactivate);
        QApplication::sendEvent(&window, &deactivated);
        window.updateDocumentActivity(0, 30);
        window.updateDocumentActivity(60 * 60 * 1000, 30);
        QVERIFY(tab->isLoaded());

        window.showMinimized();
        window.updateDocumentActivity(60 * 60 * 1000, 30);
        window.updateDocumentActivity(89 * 60 * 1000, 30);
        QVERIFY(tab->isLoaded());
        window.updateDocumentActivity(91 * 60 * 1000, 30);
        QVERIFY(tab->isSuspended());
        window.showNormal();
        QTRY_VERIFY_WITH_TIMEOUT(tab->isLoaded(), 10000);
    }

private:
    std::unique_ptr<QTemporaryDir> profile_;
};

QTEST_MAIN(TstDocumentSuspension)
#include "tst_document_suspension.moc"
