#include "config/ConfigPaths.h"
#include "config/Settings.h"
#include "io/AppFiles.h"
#include "io/Background.h"
#include "io/JobContext.h"
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
#include "Stalls.h"

#include <QAction>
#include <QApplication>
#include <QFile>
#include <QFileInfo>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QJsonDocument>
#include <QDeadlineTimer>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSignalSpy>
#include <QTabBar>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QTextLayout>
#include <QToolButton>
#include <QScopeGuard>
#include <QThread>
#include <QtTest>

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>

#ifdef Q_OS_UNIX
#  include <sys/stat.h>
#endif

using namespace mervin;
using mervin::testing::Stall;
#ifdef Q_OS_UNIX
using mervin::testing::PipeWriter;
#endif

namespace {
// A self-contained AcroForm exercises pending editor text as well as saved PDF objects.
// A `measured` one says in its catalog that it carries Mervin measurements, so a load
// reads the file a second time, for them, after it opened it.
QByteArray formPdf(bool measured = false)
{
    const QList<QByteArray> objects{
        QByteArray("<< /Type /Catalog /Pages 2 0 R ")
            + (measured ? "/Mervin_Measurements () " : "")
            + "/AcroForm << /Fields [4 0 R] /DR << /Font << /Helv 5 0 R >> >> "
              "/DA (/Helv 12 Tf 0 g) >> >>",
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

bool closed(const std::shared_ptr<DocumentLifetime> &lifetime)
{
    std::lock_guard lock(lifetime->mutex);
    return !lifetime->document;
}

// Holds every load of `path` at its first step, as a drive that stopped answering
// would, until job::testing::release(). `reads`, when given, counts the loads that
// reached it.
void holdReadsOf(const QString &path, std::atomic<int> *reads = nullptr)
{
    job::testing::holdWhen([path, reads](job::Step step, const QString &file) {
        if (step != job::Step::Reading || file != path)
            return false;
        if (reads)
            ++*reads;
        return true;
    });
}

// The placeholder's parts.
struct Placeholder
{
    explicit Placeholder(const QWidget &tab)
        : status(tab.findChild<QLabel *>(QStringLiteral("documentLoadStatus")))
        , retry(tab.findChild<QPushButton *>(QStringLiteral("retryDocumentLoad")))
        , stop(tab.findChild<QPushButton *>(QStringLiteral("stopDocumentLoad")))
    {
    }
    bool valid() const { return status && retry && stop; }

    QLabel *status;
    QPushButton *retry;
    QPushButton *stop;
};

// The notice a slow load shows, which isolates the name and the folder for
// right-to-left languages.
QString slowNotice(const QString &name, const QString &folder)
{
    const auto isolated = [](const QString &text) { return QChar(0x2068) + text + QChar(0x2069); };
    return QStringLiteral("Opening %1 from %2. The drive is not answering.")
        .arg(isolated(name), isolated(QDir::toNativeSeparators(folder)));
}

// `text` without the marks that only steer where its lines break, the word joiner and
// the zero width space.
QString withoutBreakMarks(QString text)
{
    return text.remove(QChar(0x2060)).remove(QChar(0x200B));
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
        // A test that holds a load lets it go here too, so no load outlives its test.
        job::testing::release();
        TabPage::setSlowDelayForTesting(TabPage::kSlowMs);
        QVERIFY(Document::waitForCloses(10000));
        QVERIFY(mervin::appfiles::flush(10000)); // before the profile goes
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
        QTRY_VERIFY_WITH_TIMEOUT(closed(lifetime), 10000); // on a background thread

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
        // The open recovery copy goes once its document has closed in the background.
        QTRY_VERIFY_WITH_TIMEOUT(!QFileInfo::exists(checkpoint), 10000);
        QVERIFY(mervin::appfiles::flush(10000));
        QCOMPARE(readFile(path), original);
    }

    void closingATabDoesNotWaitForItsDocument()
    {
        RenderEngine engine;
        auto tab = std::make_unique<TabPage>(&engine);
        QVERIFY(tab->open(QStringLiteral(MERVIN_FIXTURE_PDF)));
        const auto lifetime = tab->viewer()->document()->lifetime();
        Stall stall(lifetime->mutex);

        QElapsedTimer timer;
        timer.start();
        tab.reset();
        QVERIFY2(timer.elapsed() < 1000, qPrintable(QStringLiteral("closing took %1 ms")
                                                        .arg(timer.elapsed())));
        stall.release();
        QTRY_VERIFY_WITH_TIMEOUT(closed(lifetime), 10000); // on a background thread
    }

    // Closing a window to the tray, and the inactivity timer, unload documents.
    void unloadingDoesNotWaitForItsDocument()
    {
        RenderEngine engine;
        TabPage tab(&engine);
        QVERIFY(tab.open(QStringLiteral(MERVIN_FIXTURE_PDF)));
        const auto lifetime = tab.viewer()->document()->lifetime();
        Stall stall(lifetime->mutex);

        QElapsedTimer timer;
        timer.start();
        QString error;
        QVERIFY2(tab.suspend(&error), qPrintable(error));
        QVERIFY2(timer.elapsed() < 1000, qPrintable(QStringLiteral("unloading took %1 ms")
                                                        .arg(timer.elapsed())));
        QVERIFY(tab.isSuspended());
        stall.release();
        QTRY_VERIFY_WITH_TIMEOUT(closed(lifetime), 10000); // on a background thread
    }

#ifdef Q_OS_UNIX
    void closingALoadingTabRemovesItsRecoveryCopyAfterTheLoad()
    {
        const QDir recovery(QDir(ConfigPaths::configDir()).filePath(QStringLiteral("recovery")));
        QVERIFY(recovery.mkpath(QStringLiteral(".")));
        const QString snapshot = recovery.filePath(QStringLiteral("document-stalled.pdf"));
        QCOMPARE(::mkfifo(QFile::encodeName(snapshot).constData(), 0600), 0);
        const QString original = recovery.filePath(QStringLiteral("original.pdf"));
        QVERIFY(writeFile(snapshot + QStringLiteral(".json"),
                          QJsonDocument(QJsonObject{{"version", 1},
                                                    {"original", original},
                                                    {"canonical", original},
                                                    {"snapshot", "document-stalled.pdf"}})
                              .toJson()));
        QCOMPARE(TabPage::recoverablePaths(), QStringList{original});
        PipeWriter writer(snapshot);
        RenderEngine engine;
        auto tab = std::make_unique<TabPage>(&engine);
        tab->initializeSuspended(original);
        QCOMPARE(tab->recoveryPath(), snapshot);
        tab->resumeAsync();
        QTest::qWait(200); // the load waits in the open of the recovery copy

        QElapsedTimer timer;
        timer.start();
        tab.reset();
        QVERIFY2(timer.elapsed() < 1000, qPrintable(QStringLiteral("closing took %1 ms")
                                                        .arg(timer.elapsed())));
        QVERIFY(mervin::appfiles::flush(10000));
        QVERIFY(TabPage::recoverablePaths().isEmpty());
        QVERIFY(QFileInfo::exists(snapshot)); // the load still has it open
        writer.release();
        QTRY_VERIFY_WITH_TIMEOUT(!QFileInfo::exists(snapshot), 10000);
    }
#endif

    // Closing a tab whose load waits for the drive stops that load, so once the drive
    // answers it reads nothing more, here the measurements it would read next.
    void closingALoadingTabStopsItsRead()
    {
        QTemporaryDir files;
        const QString path = files.filePath(QStringLiteral("measured.pdf"));
        QVERIFY(writeFile(path, formPdf(true)));
        std::atomic<int> reads{0};
        job::testing::failWhen([path, &reads](job::Step step, const QString &file) {
            if (step == job::Step::Reading && file == path)
                ++reads;
            return false; // counts, fails nothing
        });
        const auto stopCounting = qScopeGuard([] { job::testing::failWhen({}); });
        holdReadsOf(path);
        RenderEngine engine;
        auto tab = std::make_unique<TabPage>(&engine);
        tab->initializeSuspended(path);
        const int tasks = mervin::background::running();
        tab->resumeAsync();
        QTRY_COMPARE(job::testing::held(), 1);
        tab.reset();
        job::testing::release();
        QTRY_COMPARE_WITH_TIMEOUT(mervin::background::running(), tasks, 10000); // it ended
        QCOMPARE(reads.load(), 1);
    }

    // A load says "Loading" at once. After two seconds without progress it names the
    // file and the folder it waits for and offers Stop waiting, and not before.
    void slowLoadNamesItsFolderAfterTwoSeconds()
    {
        QTemporaryDir files;
        const QString path = files.filePath(QStringLiteral("plan.pdf"));
        QVERIFY(QFile::copy(QStringLiteral(MERVIN_FIXTURE_PDF), path));
        holdReadsOf(path);
        const auto release = qScopeGuard([] { job::testing::release(); });
        RenderEngine engine;
        TabPage tab(&engine);
        showForLayout(tab);
        tab.initializeSuspended(path);
        const Placeholder placeholder(tab);
        QVERIFY(placeholder.valid());

        QElapsedTimer timer;
        timer.start();
        tab.resumeAsync();
        QVERIFY(tab.isLoading());
        QCOMPARE(placeholder.status->text(), QStringLiteral("Loading"));
        QVERIFY(placeholder.status->isVisible());
        QVERIFY(!placeholder.stop->isVisible());
        QVERIFY(!placeholder.retry->isVisible());
        QTRY_COMPARE(job::testing::held(), 1);
        // The clock starts after the timer, so a check that reads the timer below the
        // delay cannot see the notice, however late a busy machine runs it.
        while (timer.elapsed() < TabPage::kSlowMs - 250) {
            QTest::qWait(50);
            const qint64 elapsed = timer.elapsed();
            if (elapsed < TabPage::kSlowMs) {
                QVERIFY2(!placeholder.stop->isVisible(),
                         qPrintable(QStringLiteral("shown after %1 ms").arg(elapsed)));
                QCOMPARE(placeholder.status->text(), QStringLiteral("Loading"));
            }
        }
        QTRY_VERIFY_WITH_TIMEOUT(placeholder.stop->isVisible(), 3000);
        // The poll runs every 250 ms. The rest is room for a busy machine.
        QVERIFY2(timer.elapsed() >= TabPage::kSlowMs && timer.elapsed() < TabPage::kSlowMs + 1000,
                 qPrintable(QStringLiteral("shown after %1 ms").arg(timer.elapsed())));
        QCOMPARE(withoutBreakMarks(placeholder.status->text()),
                 slowNotice(QStringLiteral("plan.pdf"), files.path()));
        QVERIFY(!placeholder.retry->isVisible());
        QVERIFY(tab.isLoading());

        job::testing::release();
        QTRY_VERIFY_WITH_TIMEOUT(tab.isLoaded(), 10000);
        QVERIFY(tab.viewer()->isVisible());
        QVERIFY(!placeholder.status->isVisible());
    }

    void slowNoticeWrapsFoldersAtSeparators_data()
    {
        QTest::addColumn<QString>("name");
        QTest::addColumn<QString>("folder");
        QTest::addColumn<QString>("line"); // the notice is as wide as this text
        const QString deep = QStringLiteral("/nas/projects/structural/revised/drawings");
        QTest::newRow("network share")
            << QStringLiteral("plan.pdf")
            << QStringLiteral("\\\\fileserver\\projects\\structural\\revised\\drawings")
            << QStringLiteral("Opening plan.pdf from \\\\");
        QTest::newRow("drive") << QStringLiteral("plan.pdf")
                               << QStringLiteral("C:\\projects\\structural\\revised\\drawings")
                               << QStringLiteral("Opening plan.pdf from C:");
        QTest::newRow("posix") << QStringLiteral("plan.pdf") << deep
                               << QStringLiteral("Opening plan.pdf from /");
        // A name with nowhere to wrap is shortened. The folder wraps and stays whole.
        QTest::newRow("unbroken name")
            << QStringLiteral("Revision").repeated(30) + QStringLiteral(".pdf") << deep
            << QStringLiteral("Opening plan.pdf from");
    }

    // The notice wraps a folder after a separator that ends a folder name, on Windows
    // too. No line ends in the separators a path starts with, in "C:" or in the
    // ellipsis of a shortened name, and only a part too wide for a line is shortened.
    // The names here have no spaces or hyphens, so a line can end only at a space of
    // the sentence or after such a separator.
    void slowNoticeWrapsFoldersAtSeparators()
    {
        QFETCH(QString, name);
        QFETCH(QString, folder);
        QFETCH(QString, line);
        const QFont font = QApplication::font();
        const QFontMetrics metrics(font);
        const int width = metrics.horizontalAdvance(line) + 1;
        const QString notice = TabPage::testing::slowNotice(name, folder, metrics, width);
        QTextLayout layout(notice, font);
        QTextOption option;
        option.setWrapMode(QTextOption::WordWrap);
        layout.setTextOption(option);
        QStringList lines;
        layout.beginLayout();
        for (QTextLine next = layout.createLine(); next.isValid(); next = layout.createLine()) {
            next.setLineWidth(width);
            lines << withoutBreakMarks(notice.mid(next.textStart(), next.textLength()))
                         .remove(QChar(0x2068))
                         .remove(QChar(0x2069));
            QVERIFY2(next.naturalTextWidth() <= width, qPrintable(lines.join(u'|')));
        }
        layout.endLayout();
        const auto separator = [](QChar c) { return c == u'/' || c == u'\\'; };
        for (qsizetype i = 0; i + 1 < lines.size(); ++i) {
            const QString &text = lines.at(i);
            const bool atSpace = text.endsWith(u' ');
            const bool afterName = text.size() > 1 && separator(text.back())
                                   && text.at(text.size() - 2).isLetterOrNumber();
            QVERIFY2(atSpace || afterName, qPrintable(lines.join(u'|')));
        }
        QVERIFY2(lines.join(QString()).contains(folder), qPrintable(lines.join(u'|')));
    }

    // Stop waiting leaves the tab unloaded with Retry, and selecting it again reads
    // nothing until Retry. Without that rule, every selection would load it again.
    void stoppedLoadWaitsForRetry()
    {
        QTemporaryDir files;
        const QString path = files.filePath(QStringLiteral("plan.pdf"));
        QVERIFY(QFile::copy(QStringLiteral(MERVIN_FIXTURE_PDF), path));
        TabPage::setSlowDelayForTesting(0);
        RenderEngine engine;
        MainWindow window(&engine, nullptr);
        showForLayout(window);
        QVERIFY(window.openFile(QStringLiteral(MERVIN_FIXTURE_PDF)));
        QVERIFY(window.openFile(path, false, -1, false, {}, true));
        auto *tabs = window.findChild<QTabWidget *>();
        QVERIFY(tabs);
        auto *tab = qobject_cast<TabPage *>(tabs->widget(1));
        QVERIFY(tab);
        QVERIFY(tab->isSuspended());
        const Placeholder placeholder(*tab);
        QVERIFY(placeholder.valid());
        std::atomic<int> reads{0};
        holdReadsOf(path, &reads);
        const auto release = qScopeGuard([] { job::testing::release(); });

        tabs->setCurrentIndex(1);
        QTRY_COMPARE(job::testing::held(), 1);
        QTRY_VERIFY(placeholder.stop->isVisible());
        QCOMPARE(withoutBreakMarks(placeholder.status->text()),
                 slowNotice(QStringLiteral("plan.pdf"), files.path()));
        QElapsedTimer timer;
        timer.start();
        QTest::mouseClick(placeholder.stop, Qt::LeftButton);
        QVERIFY2(timer.elapsed() < 1000, qPrintable(QStringLiteral("stopping took %1 ms")
                                                        .arg(timer.elapsed())));
        QVERIFY(tab->isSuspended());
        QCOMPARE(placeholder.status->text(), QStringLiteral("Document is unloaded."));
        QVERIFY(placeholder.retry->isVisible());
        QVERIFY(!placeholder.stop->isVisible());

        tabs->setCurrentIndex(0);
        QTRY_VERIFY_WITH_TIMEOUT(qobject_cast<TabPage *>(tabs->widget(0))->isLoaded(), 10000);
        tabs->setCurrentIndex(1);
        QTest::qWait(300); // the window resumes a selected tab on its next turn
        QVERIFY(tab->isSuspended());
        QCOMPARE(placeholder.status->text(), QStringLiteral("Document is unloaded."));
        QVERIFY(placeholder.retry->isVisible());
        QCOMPARE(reads.load(), 1);
        QCOMPARE(job::testing::held(), 1);

        job::testing::release();
        QTest::mouseClick(placeholder.retry, Qt::LeftButton);
        QTRY_VERIFY_WITH_TIMEOUT(tab->isLoaded(), 10000);
        QVERIFY(tab->viewer()->isVisible());
    }

    // Retry while the stopped read still waits takes that read back instead of
    // starting another one.
    void retryTakesBackAStoppedRead()
    {
        QTemporaryDir files;
        const QString path = files.filePath(QStringLiteral("plan.pdf"));
        QVERIFY(QFile::copy(QStringLiteral(MERVIN_FIXTURE_PDF), path));
        std::atomic<int> reads{0};
        holdReadsOf(path, &reads);
        const auto release = qScopeGuard([] { job::testing::release(); });
        RenderEngine engine;
        TabPage tab(&engine);
        showForLayout(tab);
        tab.initializeSuspended(path);
        const Placeholder placeholder(tab);
        QVERIFY(placeholder.valid());
        QSignalSpy resumed(&tab, &TabPage::resumeFinished);
        tab.resumeAsync();
        QTRY_COMPARE(job::testing::held(), 1);
        tab.stopWaiting();
        QVERIFY(tab.isSuspended());
        QVERIFY(placeholder.retry->isVisible());

        // A new read would start a background task at once.
        const int tasks = mervin::background::running();
        QTest::mouseClick(placeholder.retry, Qt::LeftButton);
        QCOMPARE(mervin::background::running(), tasks);
        QVERIFY(tab.isLoading());
        QCOMPARE(placeholder.status->text(), QStringLiteral("Loading"));
        QTest::qWait(300);
        QCOMPARE(job::testing::held(), 1);
        QCOMPARE(reads.load(), 1);

        job::testing::release();
        QTRY_COMPARE_WITH_TIMEOUT(resumed.size(), 1, 10000);
        QVERIFY2(resumed.first().at(0).toBool(), qPrintable(resumed.first().at(1).toString()));
        QVERIFY(tab.isLoaded());
        QCOMPARE(reads.load(), 1);
    }

    void resultStoredBeforeStopClosesInTheBackground_data()
    {
        QTest::addColumn<bool>("stalled");
        QTest::newRow("free") << false;
        QTest::newRow("stalled") << true;
    }

    // A load stored its document just before Stop waiting. The stop hands it to a
    // background close, so neither the stop nor the load's late delivery waits for it,
    // and the tab stays unloaded.
    void resultStoredBeforeStopClosesInTheBackground()
    {
        QFETCH(bool, stalled);
        RenderEngine engine;
        TabPage tab(&engine);
        showForLayout(tab);
        tab.initializeSuspended(QStringLiteral(MERVIN_FIXTURE_PDF));
        QSignalSpy resumed(&tab, &TabPage::resumeFinished);
        tab.resumeAsync();
        // Without processing events, so the delivery is still queued.
        std::shared_ptr<DocumentLifetime> lifetime;
        for (QDeadlineTimer deadline(10000); !lifetime && !deadline.hasExpired();
             QThread::msleep(5))
            lifetime = TabPage::testing::storedDocument(tab);
        QVERIFY(lifetime);
        std::optional<Stall<std::timed_mutex>> stall;
        if (stalled)
            stall.emplace(lifetime->mutex);

        QElapsedTimer timer;
        timer.start();
        tab.stopWaiting();
        QVERIFY2(timer.elapsed() < 1000, qPrintable(QStringLiteral("stopping took %1 ms")
                                                        .arg(timer.elapsed())));
        QCoreApplication::processEvents(); // the load's delivery finds the tab stopped
        QVERIFY2(timer.elapsed() < 1000, qPrintable(QStringLiteral("delivery took %1 ms")
                                                        .arg(timer.elapsed())));
        QVERIFY(tab.isSuspended());
        QVERIFY(!tab.viewer()->document());
        QCOMPARE(resumed.size(), 0);
        if (stall)
            stall->release();
        QTRY_VERIFY_WITH_TIMEOUT(closed(lifetime), 1000);
    }

    void stoppedLoadEndsOnlyAfterItsDocumentClosed_data()
    {
        QTest::addColumn<QString>("then");
        QTest::newRow("close the tab") << QStringLiteral("close");
        QTest::newRow("retry") << QStringLiteral("retry");
        QTest::newRow("retry and close the tab") << QStringLiteral("retry and close");
        QTest::newRow("resume") << QStringLiteral("resume");
    }

    // A load stored the recovery copy's document just before Stop waiting, and the
    // background close of that document is stuck. The load ends only after it, so a
    // closed tab keeps the copy until then, Retry reads the copy again only then, and a
    // synchronous resume waits for it.
    void stoppedLoadEndsOnlyAfterItsDocumentClosed()
    {
        QFETCH(QString, then);
        const QDir recovery(QDir(ConfigPaths::configDir()).filePath(QStringLiteral("recovery")));
        QVERIFY(recovery.mkpath(QStringLiteral(".")));
        const QString snapshot = recovery.filePath(QStringLiteral("document-copy.pdf"));
        QVERIFY(QFile::copy(QStringLiteral(MERVIN_FIXTURE_PDF), snapshot));
        const QString original = profile_->filePath(QStringLiteral("docs/plan.pdf"));
        QVERIFY(writeFile(snapshot + QStringLiteral(".json"),
                          QJsonDocument(QJsonObject{{"version", 1},
                                                    {"original", original},
                                                    {"canonical", original},
                                                    {"snapshot", "document-copy.pdf"}})
                              .toJson()));
        QCOMPARE(TabPage::recoverablePaths(), QStringList{original});
        RenderEngine engine;
        auto tab = std::make_unique<TabPage>(&engine);
        showForLayout(*tab);
        tab->initializeSuspended(original);
        QCOMPARE(tab->recoveryPath(), snapshot);
        tab->resumeAsync();
        // Without processing events, so the delivery is still queued.
        std::shared_ptr<DocumentLifetime> lifetime;
        for (QDeadlineTimer deadline(10000); !lifetime && !deadline.hasExpired();
             QThread::msleep(5))
            lifetime = TabPage::testing::storedDocument(*tab);
        QVERIFY(lifetime);
        Stall<std::timed_mutex> stall(lifetime->mutex, 30000);
        tab->stopWaiting();
        QCoreApplication::processEvents(); // the load's delivery finds the tab stopped
        QVERIFY(tab->isSuspended());

        if (then == QLatin1String("resume")) {
            std::atomic<bool> released{false};
            std::thread releaser([&stall, &released] {
                std::this_thread::sleep_for(std::chrono::milliseconds(300));
                released = true;
                stall.release();
            });
            QString error;
            const bool resumed = tab->resume(&error);
            const bool waited = released.load();
            releaser.join();
            QVERIFY2(resumed, qPrintable(error));
            QVERIFY2(waited, "resume did not wait for the stopped load's document to close");
            QVERIFY(closed(lifetime));
            return;
        }

        std::atomic<int> reads{0};
        holdReadsOf(snapshot, &reads);
        if (then != QLatin1String("close")) {
            const Placeholder placeholder(*tab);
            QVERIFY(placeholder.valid());
            QTest::mouseClick(placeholder.retry, Qt::LeftButton);
            QVERIFY(tab->isLoading());
            QTest::qWait(300); // a read that does not wait for the close starts at once
            QCOMPARE(reads.load(), 0);
        }
        if (then == QLatin1String("retry")) {
            stall.release();
            QTRY_COMPARE_WITH_TIMEOUT(reads.load(), 1, 10000);
            job::testing::release();
            QTRY_VERIFY_WITH_TIMEOUT(tab->isLoaded(), 10000);
            QVERIFY(closed(lifetime));
            return;
        }
        tab.reset();
        QVERIFY(mervin::appfiles::flush(10000));
        QVERIFY(TabPage::recoverablePaths().isEmpty());
        QVERIFY2(QFileInfo::exists(snapshot), "the copy went while a document had it open");
        stall.release();
        QTRY_VERIFY_WITH_TIMEOUT(closed(lifetime), 10000);
        job::testing::release(); // a read Retry started gives up, as its tab closed
        QTRY_VERIFY_WITH_TIMEOUT(!QFileInfo::exists(snapshot), 10000);
    }

#ifdef Q_OS_UNIX
    // A recovery copy that never answers. Stop waiting returns at once and keeps the
    // copy for later. The notice names the recovery folder, where the load waits, not
    // the document's own folder.
    void stoppingALoadOfAStuckRecoveryCopyDoesNotWait()
    {
        const QDir recovery(QDir(ConfigPaths::configDir()).filePath(QStringLiteral("recovery")));
        QVERIFY(recovery.mkpath(QStringLiteral(".")));
        const QString snapshot = recovery.filePath(QStringLiteral("document-stalled.pdf"));
        QCOMPARE(::mkfifo(QFile::encodeName(snapshot).constData(), 0600), 0);
        const QString original = profile_->filePath(QStringLiteral("docs/plan.pdf"));
        QVERIFY(writeFile(snapshot + QStringLiteral(".json"),
                          QJsonDocument(QJsonObject{{"version", 1},
                                                    {"original", original},
                                                    {"canonical", original},
                                                    {"snapshot", "document-stalled.pdf"}})
                              .toJson()));
        QCOMPARE(TabPage::recoverablePaths(), QStringList{original});
        PipeWriter writer(snapshot);
        TabPage::setSlowDelayForTesting(0);
        RenderEngine engine;
        auto tab = std::make_unique<TabPage>(&engine);
        showForLayout(*tab);
        tab->initializeSuspended(original);
        QCOMPARE(tab->recoveryPath(), snapshot);
        const Placeholder placeholder(*tab);
        QVERIFY(placeholder.valid());
        tab->resumeAsync();
        // The load waits in the open of the recovery copy.
        QTRY_VERIFY(placeholder.stop->isVisible());
        QCOMPARE(withoutBreakMarks(placeholder.status->text()),
                 slowNotice(QStringLiteral("plan.pdf"), recovery.path()));

        QElapsedTimer timer;
        timer.start();
        QTest::mouseClick(placeholder.stop, Qt::LeftButton);
        QVERIFY2(timer.elapsed() < 1000, qPrintable(QStringLiteral("stopping took %1 ms")
                                                        .arg(timer.elapsed())));
        QVERIFY(tab->isSuspended());
        QVERIFY(placeholder.retry->isVisible());
        QCOMPARE(tab->recoveryPath(), snapshot);
        QVERIFY(tab->hasUnsavedEdits());
        QVERIFY(mervin::appfiles::flush(10000));
        QVERIFY(QFileInfo::exists(snapshot));
        QVERIFY(QFileInfo::exists(snapshot + QStringLiteral(".json")));

        // Closing the tab removes the copy once the stopped load let go of it.
        tab.reset();
        writer.release();
        QTRY_VERIFY_WITH_TIMEOUT(!QFileInfo::exists(snapshot), 10000);
        QVERIFY(mervin::appfiles::flush(10000));
        QVERIFY(TabPage::recoverablePaths().isEmpty());
    }
#endif

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

    void ocrButtonTracksTheActiveMarkingTool()
    {
        const QString path = profile_->filePath(QStringLiteral("form.pdf"));
        QVERIFY(writeFile(path, formPdf()));
        RenderEngine engine;
        MainWindow window(&engine, nullptr);
        showForLayout(window);
        QVERIFY(window.openFile(path));
        auto *tab = window.findChild<TabPage *>();
        QVERIFY(tab);
        auto *viewer = tab->viewer();
        QAction *ocr = nullptr;
        for (QAction *action : window.findChildren<QAction *>())
            if (action->shortcut() == QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_O))
                ocr = action;
        QVERIFY(ocr);
        QToolButton *button = nullptr;
        for (QToolButton *candidate : window.findChildren<QToolButton *>())
            if (candidate->defaultAction() == ocr)
                button = candidate;
        QVERIFY(button);
        QVERIFY(!button->isChecked());

        QTest::mouseClick(button, Qt::LeftButton);
        QVERIFY(viewer->ocrMode());
        QVERIFY(button->isChecked());
        QTest::mouseClick(button, Qt::LeftButton);
        QVERIFY(!viewer->ocrMode());
        QVERIFY(!button->isChecked());

        QTest::mouseClick(button, Qt::LeftButton);
        // A click finishes the one-shot gesture without opening recognition for a region.
        QTest::mouseClick(viewer->viewport(), Qt::LeftButton, Qt::NoModifier, QPoint(400, 300));
        QVERIFY(!viewer->ocrMode());
        QVERIFY(!button->isChecked());

        ocr->trigger();
        viewer->setMeasureMode(true);
        QVERIFY(viewer->measureMode());
        QVERIFY(!viewer->ocrMode());
        QVERIFY(!button->isChecked());
        ocr->trigger();
        viewer->setFormMode(true);
        QVERIFY(viewer->formMode());
        QVERIFY(!viewer->ocrMode());
        QVERIFY(!button->isChecked());
        ocr->trigger();
        viewer->setCommentToolEnabled(true);
        QVERIFY(viewer->commentToolEnabled());
        QVERIFY(!viewer->ocrMode());
        QVERIFY(!button->isChecked());
    }

    void ocrActionFollowsTabSwitchesAndReload()
    {
        RenderEngine engine;
        MainWindow window(&engine, nullptr);
        showForLayout(window);
        QVERIFY(window.openFile(QStringLiteral(MERVIN_FIXTURE_PDF)));
        auto *tabs = window.findChild<QTabWidget *>();
        QVERIFY(tabs);
        auto *first = qobject_cast<TabPage *>(tabs->widget(0));
        QVERIFY(first);
        QAction *ocr = nullptr;
        for (QAction *action : window.findChildren<QAction *>())
            if (action->shortcut() == QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_O))
                ocr = action;
        QVERIFY(ocr);
        ocr->trigger();
        QVERIFY(first->viewer()->ocrMode());
        QVERIFY(ocr->isChecked());

        QVERIFY(window.openFile(QStringLiteral(MERVIN_FIXTURE_PDF), true));
        auto *second = qobject_cast<TabPage *>(tabs->widget(1));
        QVERIFY(second);
        QVERIFY(!second->viewer()->ocrMode());
        QVERIFY(!ocr->isChecked());
        QVERIFY(first->viewer()->ocrMode());
        tabs->setCurrentIndex(0);
        QVERIFY(ocr->isChecked());

        QVERIFY(first->suspend());
        QVERIFY(!first->viewer()->ocrMode());
        QVERIFY(!ocr->isChecked());
        QVERIFY(!ocr->isEnabled());
        QVERIFY(first->resume());
        QVERIFY(first->viewer()->ocrMode());
        QVERIFY(ocr->isChecked());
        QVERIFY(ocr->isEnabled());

        tabs->setCurrentIndex(1);
        QVERIFY(!ocr->isChecked());
        // Changes in a background tab must not change the current tab's action.
        first->viewer()->setOcrMode(false);
        first->viewer()->setOcrMode(true);
        QVERIFY(!ocr->isChecked());
        tabs->setCurrentIndex(0);
        QVERIFY(ocr->isChecked());
    }

    void ocrActionStaysInactiveOnRecent_data()
    {
        QTest::addColumn<bool>("armed");
        QTest::newRow("marking-tool-active") << true;
        QTest::newRow("marking-tool-inactive") << false;
    }

    void ocrActionStaysInactiveOnRecent()
    {
        QFETCH(bool, armed);
        RenderEngine engine;
        MainWindow window(&engine, nullptr);
        showForLayout(window);
        QVERIFY(window.openFile(QStringLiteral(MERVIN_FIXTURE_PDF)));
        auto *tab = window.findChild<TabPage *>();
        auto *recent = window.findChild<QPushButton *>(QStringLiteral("recentPillBtn"));
        auto *bar = window.findChild<QTabBar *>(QStringLiteral("docTabBar"));
        QVERIFY(tab);
        QVERIFY(recent);
        QVERIFY(bar);
        QAction *ocr = nullptr;
        for (QAction *action : window.findChildren<QAction *>())
            if (action->shortcut() == QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_O))
                ocr = action;
        QVERIFY(ocr);
        auto *viewer = tab->viewer();
        viewer->setOcrMode(armed);
        QCOMPARE(ocr->isChecked(), armed);
        QVERIFY(ocr->isEnabled());

        QTest::mouseClick(recent, Qt::LeftButton);
        QVERIFY(recent->property("recentActive").toBool());
        QVERIFY(!ocr->isEnabled());
        QVERIFY(!ocr->isChecked());
        QCOMPARE(viewer->ocrMode(), armed);
        ocr->trigger();
        QTest::keyClick(&window, Qt::Key_O, Qt::ControlModifier | Qt::ShiftModifier);
        QCOMPARE(viewer->ocrMode(), armed);
        QVERIFY(!ocr->isChecked());

        // Changes in the hidden viewer must not reactivate the Recent view's action.
        viewer->setOcrMode(!armed);
        QVERIFY(!ocr->isEnabled());
        QVERIFY(!ocr->isChecked());
        viewer->setOcrMode(armed);
        QVERIFY(!ocr->isEnabled());
        QVERIFY(!ocr->isChecked());

        // Returning through the same tab does not emit currentChanged.
        QTest::mouseClick(bar, Qt::LeftButton, Qt::NoModifier, bar->tabRect(0).center());
        QVERIFY(!recent->property("recentActive").toBool());
        QVERIFY(ocr->isEnabled());
        QCOMPARE(ocr->isChecked(), armed);
        QCOMPARE(viewer->ocrMode(), armed);
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
        QVERIFY(mervin::appfiles::flush(10000)); // recovery files go in the background
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
        QVERIFY(mervin::appfiles::flush(10000)); // recovery files go in the background
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
            // The document closes on a background thread once its render and search
            // work has stopped.
            QTRY_VERIFY_WITH_TIMEOUT(closed(lifetime), 10000);
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
