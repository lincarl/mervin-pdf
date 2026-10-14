#include "TestWaits.h"
// Rotate Pages and Split run as their tab's operation. Only the tab waits, a bar names
// what it waits for after a few seconds, Stop waiting gives up at once, and nothing
// the screen thread does meanwhile reads the tab's document.

#include "app/WindowManager.h"
#include "config/ConfigPaths.h"
#include "config/Settings.h"
#include "dialogs/ExportMeasureDialog.h"
#include "io/AppFiles.h"
#include "io/Background.h"
#include "io/JobContext.h"
#include "render/AnnotModel.h"
#include "render/Document.h"
#include "render/RenderEngine.h"
#include "security/QpdfService.h"
#include "security/MeasureExport.h"
#include "ui/MainWindow.h"
#include "ui/TabPage.h"
#include "ui/ViewerWidget.h"
#include "Stalls.h"

#include <QAction>
#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileDialog>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPointer>
#include <QPushButton>
#include <QScopeGuard>
#include <QScrollBar>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#include <QToolButton>
#include <QtTest>

#include <atomic>
#include <memory>

#ifdef Q_OS_UNIX
#  include <sys/stat.h>
#endif

using namespace mervin;
using mervin::testing::SavingJob;
using mervin::testing::Stall;

namespace {

void showForLayout(QWidget &widget)
{
    widget.setAttribute(Qt::WA_DontShowOnScreen);
    widget.resize(1000, 800);
    widget.show();
    QCoreApplication::processEvents();
}

// The tab a window shows.
TabPage *currentTab(MainWindow &window)
{
    auto *tabs = window.findChild<QTabWidget *>();
    return tabs ? qobject_cast<TabPage *>(tabs->currentWidget()) : nullptr;
}

// The wait bar's parts.
struct Bar
{
    explicit Bar(const QWidget &tab)
        : bar(tab.findChild<QWidget *>(QStringLiteral("waitBar")))
        , text(tab.findChild<QLabel *>(QStringLiteral("waitBarText")))
        , stop(tab.findChild<QPushButton *>(QStringLiteral("waitBarStop")))
        , still(tab.findChild<QWidget *>(QStringLiteral("operationStill")))
    {
    }
    bool valid() const { return bar && text && stop && still; }

    QWidget *bar;
    QLabel *text;
    QPushButton *stop;
    QWidget *still;
};

// `text` without the marks that only steer where its lines break, the word joiner and
// the zero width space.
QString withoutBreakMarks(QString text)
{
    return text.remove(QChar(0x2060)).remove(QChar(0x200B));
}

QString isolated(const QString &text)
{
    return QChar(0x2068) + text + QChar(0x2069);
}

QString savingTo(const QString &folder)
{
    return QStringLiteral("Saving to %1. The drive is not answering.")
        .arg(isolated(QDir::toNativeSeparators(folder)));
}

QString reading(const QString &name, const QString &folder)
{
    return QStringLiteral("Reading %1 from %2. The drive is not answering.")
        .arg(isolated(name), isolated(QDir::toNativeSeparators(folder)));
}

// Holds every report of `step` for `path`, as a drive that stopped answering would,
// until job::testing::release().
void holdAt(job::Step step, const QString &path)
{
    job::testing::holdWhen([step, path](job::Step reported, const QString &where) {
        return reported == step && where == path;
    });
}

// The files in `folder`, temporary files included.
QStringList filesIn(const QString &folder)
{
    return QDir(folder).entryList(QDir::Files | QDir::Hidden | QDir::System, QDir::Name);
}

QString took(const char *what, const QElapsedTimer &timer)
{
    return QStringLiteral("%1 took %2 ms").arg(QLatin1String(what)).arg(timer.elapsed());
}

// Waits until the background tasks are back to `tasks`, as when a job a test let go of
// has returned. The render workers of a live engine count too, so this cannot wait for
// none.
bool jobsEnded(int tasks)
{
    return QTest::qWaitFor([tasks] { return background::running() <= tasks; }, 10000);
}

} // namespace

// Checks page-tool jobs, wait controls, destination safety and completion ownership.
class TstTabOperations : public QObject
{
    Q_OBJECT

public:
    TstTabOperations()
    {
        // Native file dialogs cannot be driven from a test.
        QApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
        // Answers the prompts, file dialogs and boxes the commands raise, in order.
        connect(&dialogs_, &QTimer::timeout, this, [this] {
            QWidget *modal = QApplication::activeModalWidget();
            if (auto *box = qobject_cast<QMessageBox *>(modal)) {
                if (answeredModal_ == box)
                    return;
                answeredModal_ = box;
                told_ << box->windowTitle() + QStringLiteral(": ") + box->text();
                toldIn_ << box->parentWidget();
                QAbstractButton *button = answers_.isEmpty() ? nullptr
                                                             : box->button(answers_.takeFirst());
                if (!button) {
                    unexpected_ << told_.last();
                    box->reject();
                    return;
                }
                button->click();
            } else if (auto *exportDialog = qobject_cast<ExportMeasureDialog *>(modal)) {
                exportDialog->accept();
            } else if (auto *input = qobject_cast<QInputDialog *>(modal)) {
                if (answeredModal_ == input)
                    return;
                answeredModal_ = input;
                if (inputs_.isEmpty()) {
                    unexpected_ << input->windowTitle() + QStringLiteral(": ") + input->labelText();
                    input->reject();
                    return;
                }
                // A null value takes what the prompt offers.
                const QString value = inputs_.takeFirst();
                if (!value.isNull())
                    input->setTextValue(value);
                input->accept();
            } else if (auto *dialog = qobject_cast<QFileDialog *>(modal)) {
                if (submittedFileDialog_ == dialog)
                    return;
                auto *name = dialog->findChild<QLineEdit *>(QStringLiteral("fileNameEdit"));
                if (!name)
                    return;
                if (files_.isEmpty()) {
                    unexpected_ << dialog->windowTitle();
                    dialog->reject();
                    return;
                }
                // Editing or queued acceptance can leave this dialog active for
                // another timer turn. Submit each answer once, before any signals.
                submittedFileDialog_ = dialog;
                const QString path = QDir::toNativeSeparators(files_.takeFirst());
                dialog->selectFile(path);
                name->setText(path);
                // Keep driving a nested overwrite question. If acceptance leaves
                // the picker open, the next answer may choose a different path.
                const QPointer<QFileDialog> guarded(dialog);
                QMetaObject::invokeMethod(dialog, [this, guarded] {
                    if (!guarded)
                        return;
                    QMetaObject::invokeMethod(guarded, "accept", Qt::DirectConnection);
                    if (guarded && guarded->isVisible())
                        submittedFileDialog_.clear();
                }, Qt::QueuedConnection);
            }
        });
    }

private slots:
    void init()
    {
        profile_ = std::make_unique<QTemporaryDir>();
        QVERIFY(profile_->isValid());
        ConfigPaths::setOverrideDir(profile_->path());
        Settings settings;
        settings.autoUpdate = false;
        QVERIFY(settings.save());
        answers_.clear();
        inputs_.clear();
        files_.clear();
        answeredModal_.clear();
        submittedFileDialog_.clear();
        told_.clear();
        toldIn_.clear();
        unexpected_.clear();
        dialogs_.start(10);
    }

    void cleanup()
    {
        // A test that holds a job lets it go here too, so no job outlives its test.
        job::testing::release();
        TabPage::setSlowDelayForTesting(TabPage::kSlowMs);
        QVERIFY(background::settle(10000));
        QCoreApplication::processEvents();
        QVERIFY(Document::waitForCloses(10000));
        dialogs_.stop();
        QVERIFY(appfiles::flush(10000)); // before the profile goes
        ConfigPaths::setOverrideDir({});
        profile_.reset();
        QVERIFY2(unexpected_.isEmpty(), qPrintable(unexpected_.join(u'\n')));
        QVERIFY2(answers_.isEmpty() && inputs_.isEmpty() && files_.isEmpty(),
                 "a dialog the test expected did not show");
    }

    void copyAndExportRunOffTheScreenThread_data() { copyModes(); }

    // A copy captures the live edits and measurements. Export burns the marks into
    // content. Neither command marks the source saved or performs job I/O on the GUI.
    void copyAndExportRunOffTheScreenThread()
    {
        QFETCH(bool, flatten);
        QTemporaryDir files;
        const QString source = files.filePath(QStringLiteral("plan.pdf"));
        const QString output = files.filePath(QStringLiteral("copy.pdf"));
        QVERIFY(QFile::copy(QStringLiteral(MERVIN_FIXTURE_PDF), source));
        RenderEngine engine;
        MainWindow window(&engine, nullptr);
        showForLayout(window);
        QVERIFY(mervin::testing::openAndWait(window, source));
        TabPage *tab = currentTab(window);
        QVERIFY(tab);
        prepareCopy(*tab);
        const int guiSteps = job::testing::stepsOnMainThread();
        holdAt(job::Step::Writing, files.path());
        copy(window, output, flatten);
        QVERIFY(tab->isBusy());
        QTRY_COMPARE(job::testing::held(), 1);
        QCOMPARE(job::testing::stepsOnMainThread(), guiSteps);
        QVERIFY(tab->hasUnsavedEdits());
        QVERIFY(!QFile::exists(output));
        answers_ << QMessageBox::No;
        job::testing::release();
        QTRY_VERIFY_WITH_TIMEOUT(tab->isUsable(), 10000);
        QTRY_VERIFY(answers_.isEmpty());
        QVERIFY(tab->hasUnsavedEdits());
        auto saved = engine.openDocument(output);
        QVERIFY(saved);
        const auto notes = AnnotModel(*saved).pageAnnots(0);
        QCOMPARE(notes.size(), 1u);
        QCOMPARE(notes.front().contents, QStringLiteral("Kept edit"));
        const auto blob = MeasureExport::readMervinBlob(output);
        if (flatten) {
            QVERIFY(!blob);
            const QImage before = engine.renderPageImage(tab->viewer()->document(), 0, 1, 0);
            const QImage after = engine.renderPageImage(saved.get(), 0, 1, 0);
            QVERIFY(!after.isNull() && before != after);
        } else {
            QVERIFY(blob);
            MeasureDoc measurements;
            QVERIFY(parseMeasurements(*blob, &measurements));
            QCOMPARE(measurements.measurements.size(), 1u);
        }
    }

    void copyAndExportDoNotWaitForDocumentAccess_data() { copyModes(); }

    // The still picture exists before another reader stalls access. Both commands
    // return promptly, then their worker waits without making the GUI read the document.
    void copyAndExportDoNotWaitForDocumentAccess()
    {
        QFETCH(bool, flatten);
        QTemporaryDir files;
        RenderEngine engine;
        MainWindow window(&engine, nullptr);
        showForLayout(window);
        QVERIFY(mervin::testing::openAndWait(window, QStringLiteral(MERVIN_FIXTURE_PDF)));
        TabPage *tab = currentTab(window);
        QVERIFY(tab);
        prepareCopy(*tab);
        const auto lifetime = tab->viewer()->document()->lifetime();
        const QString output = files.filePath(QStringLiteral("copy.pdf"));
        std::unique_ptr<Stall<std::mutex>> access;
        const auto busy = connect(tab, &TabPage::stateChanged, this, [&] {
            if (tab->isBusy() && !access)
                access = std::make_unique<Stall<std::mutex>>(
                    tab->viewer()->document()->accessMutex(), 5000);
        });
        const auto restore = qScopeGuard([&] {
            disconnect(busy);
            job::testing::release();
            if (access)
                access->release();
        });
        holdAt(job::Step::Writing, QDir::tempPath());
        QVERIFY(appfiles::flush(10000));
        const int tasks = background::running();
        const int guiSteps = job::testing::stepsOnMainThread();
        const int refusals = Document::testing::refusals();
        QElapsedTimer command;
        command.start();
        copy(window, output, flatten);
        QVERIFY2(command.elapsed() < 1000, qPrintable(took("Copy or Export", command)));
        QVERIFY(access);
        QTRY_COMPARE(job::testing::held(), 1);
        QCOMPARE(lifetime->holds.load(), 0);
        job::testing::release();
        QTRY_VERIFY(lifetime->holds.load() > 0);

        QElapsedTimer screen;
        screen.start();
        window.resize(900, 700);
        bool ran = false;
        QTimer::singleShot(0, &window, [&ran] { ran = true; });
        while (!ran && screen.elapsed() < 1000)
            QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
        QVERIFY2(ran && screen.elapsed() < 1000, qPrintable(took("the event loop", screen)));
        QCOMPARE(Document::testing::refusals(), refusals);
        QCOMPARE(job::testing::stepsOnMainThread(), guiSteps);
        QElapsedTimer stop;
        stop.start();
        tab->stopWaiting();
        QVERIFY2(stop.elapsed() < 1000, qPrintable(took("Stop", stop)));
        QVERIFY(tab->isBusy() && !tab->isUsable() && tab->isOperationStopped());
        const Bar bar(*tab);
        QVERIFY(bar.valid() && bar.bar->isVisible() && bar.stop->isHidden());
        access->release();
        QTRY_VERIFY_WITH_TIMEOUT(tab->isUsable(), 10000);
        QCOMPARE(lifetime->holds.load(), 0);
        QVERIFY(jobsEnded(tasks));
        QVERIFY(tab->viewer()->isVisible());
        QVERIFY(tab->hasUnsavedEdits());
        QVERIFY(filesIn(files.path()).isEmpty());
        QVERIFY(told_.isEmpty());
    }

    void stoppedCopyWaitsForDocumentRelease_data() { copyModes(); }

    // The document remains behind the still picture until the trapped write
    // releases it. Worker cleanup and a late result cannot publish an output.
    void stoppedCopyWaitsForDocumentRelease()
    {
        QFETCH(bool, flatten);
        QTemporaryDir files;
        RenderEngine engine;
        MainWindow window(&engine, nullptr);
        showForLayout(window);
        QVERIFY(mervin::testing::openAndWait(window, QStringLiteral(MERVIN_FIXTURE_PDF)));
        TabPage *tab = currentTab(window);
        QVERIFY(tab);
        prepareCopy(*tab);
        const QString output = files.filePath(QStringLiteral("copy.pdf"));
        const QString tempRoot = QDir::tempPath();
        const auto lifetime = tab->viewer()->document()->lifetime();
        QString stage;
        job::testing::holdWhen([&stage, tempRoot, destinationFolder = files.path()](job::Step step, const QString &path) {
            if (step != job::Step::Writing || path == tempRoot || path == destinationFolder
                || !path.startsWith(tempRoot + QLatin1Char('/')))
                return false;
            stage = path;
            return true;
        });
        // Clear the callback before its captured stage and window are destroyed.
        const auto release = qScopeGuard([] { job::testing::release(); });
        QSignalSpy finished(tab, &TabPage::operationFinished);
        const int refusals = Document::testing::refusals();
        copy(window, output, flatten);
        QTRY_COMPARE(job::testing::held(), 1);
        QVERIFY(lifetime->holds.load() > 0);
        QElapsedTimer timer;
        timer.start();
        tab->stopWaiting();
        QVERIFY2(timer.elapsed() < 1000, qPrintable(took("Stop", timer)));
        QVERIFY(tab->isBusy() && !tab->isUsable() && tab->isOperationStopped());
        QVERIFY(lifetime->holds.load() > 0);
        const Bar bar(*tab);
        QVERIFY(bar.valid() && !bar.bar->isHidden() && bar.stop->isHidden());
        QCOMPARE(bar.text->text(), QStringLiteral(
            "Stopped waiting. The document stays locked until the drive answers."));
        QCOMPARE(finished.size(), 0);
        window.resize(1050, 760);
        QCoreApplication::processEvents();
        QCOMPARE(Document::testing::refusals(), refusals);
        job::testing::release();
        QTRY_VERIFY_WITH_TIMEOUT(tab->isUsable(), 10000);
        QCOMPARE(lifetime->holds.load(), 0);
        QVERIFY(tab->viewer()->isVisible());
        QCOMPARE(finished.size(), 1);
        QVERIFY(!finished.first().first().toBool());
        QVERIFY(tab->hasUnsavedEdits());
        QTRY_VERIFY(!QFile::exists(stage));
        QVERIFY(!QFile::exists(output));
        QVERIFY(told_.isEmpty());
    }

    void stoppingSaveReleaseWaitCancelsTheClose_data() { copyModes(); }

    // A chosen Save waits for a stopped copy's live document. Stop at that
    // prerequisite cancels closing and restores the ordinary trapped notice.
    void stoppingSaveReleaseWaitCancelsTheClose()
    {
        QFETCH(bool, flatten);
        QTemporaryDir files;
        const QString source = files.filePath(QStringLiteral("plan.pdf"));
        const QString output = files.filePath(QStringLiteral("copy.pdf"));
        QVERIFY(QFile::copy(QStringLiteral(MERVIN_FIXTURE_PDF), source));
        RenderEngine engine;
        MainWindow window(&engine, nullptr);
        showForLayout(window);
        QVERIFY(testing::openAndWait(window, source));
        TabPage *tab = currentTab(window);
        prepareCopy(*tab);
        const QString tempRoot = QDir::tempPath();
        job::testing::holdWhen([tempRoot, destination = files.path()](job::Step step, const QString &path) {
            return step == job::Step::Writing && path != tempRoot && path != destination
                && path.startsWith(tempRoot + QLatin1Char('/'));
        });
        const auto release = qScopeGuard([] { job::testing::release(); });
        copy(window, output, flatten);
        QTRY_COMPARE(job::testing::held(), 1);
        tab->stopWaiting();
        QVERIFY(tab->isBusy() && tab->isOperationStopped());
        QVERIFY(Bar(*tab).stop->isHidden());
        answers_ << QMessageBox::Save;
        QVERIFY(QMetaObject::invokeMethod(&window, "closeTab", Q_ARG(int, 0)));
        QTRY_VERIFY(answers_.isEmpty());
        QTRY_VERIFY(tab->isClosing());
        QTRY_VERIFY(!Bar(*tab).stop->isHidden());
        Bar(*tab).stop->click();
        QTRY_VERIFY(!tab->isClosing());
        QTRY_VERIFY(Bar(*tab).stop->isHidden());
        QCOMPARE(window.tabCount(), 1);
        QVERIFY(tab->isBusy());
        job::testing::release();
        QTRY_VERIFY_WITH_TIMEOUT(tab->isUsable(), 10000);
        QVERIFY(tab->hasUnsavedEdits());
        QCOMPARE(window.tabCount(), 1);
        QVERIFY(!QFile::exists(output));
        auto original = engine.openDocument(source);
        QVERIFY(original);
        QVERIFY(AnnotModel(*original).pageAnnots(0).empty());
    }

    void stoppedCopyNeverBeginsItsHold_data() { copyModes(); }

    // Stop wins before the atomic document-use reservation, so a released
    // destination probe cannot take the now-usable document.
    void stoppedCopyNeverBeginsItsHold()
    {
        QFETCH(bool, flatten);
        QTemporaryDir files;
        RenderEngine engine;
        MainWindow window(&engine, nullptr);
        showForLayout(window);
        QVERIFY(mervin::testing::openAndWait(window, QStringLiteral(MERVIN_FIXTURE_PDF)));
        TabPage *tab = currentTab(window);
        prepareCopy(*tab);
        const auto lifetime = tab->viewer()->document()->lifetime();
        const QString output = files.filePath(QStringLiteral("copy.pdf"));
        holdAt(job::Step::Writing, files.path());
        QVERIFY(appfiles::flush(10000));
        const int tasks = background::running();
        copy(window, output, flatten);
        QTRY_COMPARE(job::testing::held(), 1);
        QCOMPARE(lifetime->holds.load(), 0);
        tab->stopWaiting();
        QVERIFY(tab->isUsable());
        job::testing::release();
        QTRY_COMPARE(job::testing::held(), 0);
        QVERIFY(jobsEnded(tasks));
        QCOMPARE(lifetime->holds.load(), 0);
        QVERIFY(!QFile::exists(output));
        QVERIFY(told_.isEmpty());
    }

    void stoppedCopyPreservesExistingDestination_data() { copyModes(); }

    void stoppedCopyPreservesExistingDestination()
    {
        QFETCH(bool, flatten);
        QTemporaryDir files;
        const QString output = files.filePath(QStringLiteral("copy.pdf"));
        const QByteArray original("The existing destination must survive.");
        QFile existing(output);
        QVERIFY(existing.open(QIODevice::WriteOnly));
        QCOMPARE(existing.write(original), qint64(original.size()));
        existing.close();
        RenderEngine engine;
        MainWindow window(&engine, nullptr);
        showForLayout(window);
        QVERIFY(mervin::testing::openAndWait(window, QStringLiteral(MERVIN_FIXTURE_PDF)));
        TabPage *tab = currentTab(window);
        prepareCopy(*tab);
        holdAt(job::Step::Commit, output);
        answers_ << QMessageBox::Yes; // file picker's overwrite question
        copy(window, output, flatten);
        QTRY_COMPARE_WITH_TIMEOUT(job::testing::held(), 1, 10000);
        tab->stopWaiting();
        QVERIFY(tab->isUsable());
        job::testing::release();
        QTRY_COMPARE(job::testing::held(), 0);
        QTRY_COMPARE(filesIn(files.path()), QStringList{QStringLiteral("copy.pdf")});
        QVERIFY(existing.open(QIODevice::ReadOnly));
        QCOMPARE(existing.readAll(), original);
        QVERIFY(told_.size() == 1); // only the overwrite question
    }

#ifdef Q_OS_UNIX
    // Opening a named pipe without a writer waits, as a read from a share that stopped
    // answering does. Rotate reads the tab's file in the background, so the command
    // returns at once. Before, the window waited until the pipe was opened.
    void rotateDoesNotWaitForASourceThatDoesNotAnswer()
    {
        QTemporaryDir files;
        QVERIFY(QDir(files.path()).mkpath(QStringLiteral("out")));
        const QString path = files.filePath(QStringLiteral("plan.pdf"));
        const QString out = files.filePath(QStringLiteral("out/plan-rotated.pdf"));
        QVERIFY(QFile::copy(QStringLiteral(MERVIN_FIXTURE_PDF), path));
        RenderEngine engine;
        MainWindow window(&engine, nullptr);
        showForLayout(window);
        QVERIFY(mervin::testing::openAndWait(window, path));
        TabPage *tab = currentTab(window);
        QVERIFY(tab);
        // The open document keeps its file. The name now leads to a pipe.
        QVERIFY(QFile::remove(path));
        QCOMPARE(::mkfifo(QFile::encodeName(path).constData(), 0600), 0);
        mervin::testing::PipeWriter writer(path, 5000);
        QVERIFY(appfiles::flush(10000));
        const int tasks = background::running();

        QElapsedTimer timer;
        timer.start();
        rotate(window, out);
        QVERIFY2(timer.elapsed() < 1000, qPrintable(took("Rotate Pages", timer)));
        QVERIFY(tab->isBusy());
        QVERIFY(!tab->viewer()->isVisible());

        // Stopped first, so letting the read through writes and says nothing.
        tab->stopWaiting();
        QVERIFY(tab->isUsable());
        writer.release();
        QVERIFY(jobsEnded(tasks));
        QCoreApplication::processEvents();
        QVERIFY(filesIn(QFileInfo(out).path()).isEmpty());
    }
#endif

    // With its write held, as on a share that stopped answering, Rotate has returned,
    // its tab is busy and the window made no file call for it. Released, the copy is
    // written and the window offers to open it.
    void rotateRunsOffTheScreenThread()
    {
        QTemporaryDir files;
        QVERIFY(QDir(files.path()).mkpath(QStringLiteral("out")));
        const QString path = files.filePath(QStringLiteral("plan.pdf"));
        const QString folder = files.filePath(QStringLiteral("out"));
        const QString out = files.filePath(QStringLiteral("out/plan-rotated.pdf"));
        QVERIFY(QFile::copy(QStringLiteral(MERVIN_FIXTURE_PDF), path));
        RenderEngine engine;
        MainWindow window(&engine, nullptr);
        showForLayout(window);
        QVERIFY(mervin::testing::openAndWait(window, path));
        TabPage *tab = currentTab(window);
        QVERIFY(tab);
        holdAt(job::Step::Writing, folder);
        const int steps = job::testing::stepsOnMainThread();

        QElapsedTimer timer;
        timer.start();
        rotate(window, out);
        QVERIFY2(timer.elapsed() < 1000, qPrintable(took("Rotate Pages", timer)));
        QVERIFY(tab->isBusy());
        QTRY_COMPARE(job::testing::held(), 1);
        QCOMPARE(job::testing::stepsOnMainThread(), steps);
        QVERIFY(!QFileInfo::exists(out));

        QSignalSpy finished(tab, &TabPage::operationFinished);
        answers_ << QMessageBox::No; // "Open it now?"
        job::testing::release();
        QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 10000);
        QCOMPARE(finished.first().first().toBool(), true);
        QVERIFY(tab->isUsable());
        QVERIFY(tab->viewer()->isVisible());
        QTRY_VERIFY(answers_.isEmpty());
        QVERIFY(told_.last().startsWith(QStringLiteral("Done: Saved to:")));
        QCOMPARE(window.tabCount(), 1);
        QCOMPARE(job::testing::stepsOnMainThread(), steps);
        auto rotated = engine.openDocument(out); // a step on this thread
        QVERIFY(rotated);
        QCOMPARE(rotated->pageCount(), 4);
    }

    // The bar shows after two seconds without progress, not before, and names the
    // folder the copy is written to.
    void waitBarAppearsAfterTwoSeconds()
    {
        QTemporaryDir files;
        QVERIFY(QDir(files.path()).mkpath(QStringLiteral("out")));
        const QString path = files.filePath(QStringLiteral("plan.pdf"));
        const QString folder = files.filePath(QStringLiteral("out"));
        QVERIFY(QFile::copy(QStringLiteral(MERVIN_FIXTURE_PDF), path));
        RenderEngine engine;
        MainWindow window(&engine, nullptr);
        showForLayout(window);
        QVERIFY(mervin::testing::openAndWait(window, path));
        TabPage *tab = currentTab(window);
        QVERIFY(tab);
        const Bar bar(*tab);
        QVERIFY(bar.valid());
        holdAt(job::Step::Writing, folder);

        QElapsedTimer timer;
        timer.start();
        rotate(window, files.filePath(QStringLiteral("out/plan-rotated.pdf")));
        QVERIFY(tab->isBusy());
        QVERIFY(bar.still->isVisible());
        QVERIFY(!tab->viewer()->isVisible());
        QTRY_COMPARE(job::testing::held(), 1);
        // The clock starts after the timer, so a check that reads the timer below the
        // delay cannot see the bar, however late a busy machine runs it.
        while (timer.elapsed() < TabPage::kSlowMs - 250) {
            QTest::qWait(50);
            const qint64 elapsed = timer.elapsed();
            if (elapsed < TabPage::kSlowMs)
                QVERIFY2(!bar.bar->isVisible(),
                         qPrintable(QStringLiteral("shown after %1 ms").arg(elapsed)));
        }
        QTRY_VERIFY_WITH_TIMEOUT(bar.bar->isVisible(), 3000);
        // The poll runs every 250 ms. The rest is room for a busy machine.
        QVERIFY2(timer.elapsed() >= TabPage::kSlowMs && timer.elapsed() < TabPage::kSlowMs + 1000,
                 qPrintable(QStringLiteral("shown after %1 ms").arg(timer.elapsed())));
        QCOMPARE(withoutBreakMarks(bar.text->text()), savingTo(folder));
        QVERIFY(bar.stop->isVisible());
        // From the left edge to the scroll bar's column, over the still picture.
        const QRect onTab = bar.bar->geometry();
        QCOMPARE(onTab.topLeft(), QPoint(0, 0));
        const auto *scrollBar = tab->viewer()->verticalScrollBar();
        QCOMPARE(onTab.width(),
                 tab->width() - (scrollBar->isVisibleTo(tab->viewer()) ? scrollBar->width() : 0));
        QVERIFY(onTab.height() >= 43);
        QCOMPARE(bar.still->geometry(), tab->rect());

        answers_ << QMessageBox::No; // "Open it now?"
        job::testing::release();
        QTRY_VERIFY_WITH_TIMEOUT(tab->isUsable(), 10000);
        QVERIFY(!bar.bar->isVisible());
        QVERIFY(!bar.still->isVisible());
        QTRY_VERIFY(answers_.isEmpty());
    }

    void waitBarNamesWhatTheToolWaitsFor_data()
    {
        QTest::addColumn<bool>("write");     // the write held, or else the read
        QTest::addColumn<bool>("elsewhere"); // the copy goes to another folder
        QTest::newRow("write") << true << true;
        QTest::newRow("read") << false << true;
        QTest::newRow("read in the destination") << false << false;
    }

    // Progress delays the bar. Once shown, the bar stays and follows the job's step.
    void waitBarFollowsProgressAndStepChanges()
    {
        QTemporaryDir files;
        const QString firstFolder = files.filePath(QStringLiteral("first"));
        const QString nextFolder = files.filePath(QStringLiteral("next"));
        const QString source = files.filePath(QStringLiteral("source/plan.pdf"));
        TabPage::setSlowDelayForTesting(500);
        RenderEngine engine;
        MainWindow window(&engine, nullptr);
        showForLayout(window);
        QVERIFY(mervin::testing::openAndWait(window, QStringLiteral(MERVIN_FIXTURE_PDF)));
        TabPage *tab = currentTab(window);
        QVERIFY(tab);
        const Bar bar(*tab);
        QVERIFY(bar.valid());

        auto stage = std::make_shared<std::atomic<int>>(0);
        TabPage::Operation operation;
        operation.destination = firstFolder;
        operation.work = [stage, firstFolder, nextFolder, source] {
            int previous = -1;
            while (!job::stopped()) {
                const int current = stage->load();
                if (current == 4)
                    return;
                if (current != previous) {
                    if (current == 2)
                        job::reading(source);
                    else if (current == 3)
                        job::writing(nextFolder);
                    else if (current == 0)
                        job::writing(firstFolder);
                    previous = current;
                }
                if (current != 1)
                    job::progressed();
                QThread::msleep(25);
            }
        };
        tab->startOperation(std::move(operation));
        const auto stop = qScopeGuard([tab] { tab->stopWaiting(); });

        QElapsedTimer active;
        active.start();
        while (active.elapsed() < 1250) {
            QTest::qWait(50);
            QVERIFY(!bar.bar->isVisible());
        }
        stage->store(1);
        QTRY_VERIFY_WITH_TIMEOUT(bar.bar->isVisible(), 2000);
        QCOMPARE(withoutBreakMarks(bar.text->text()), savingTo(firstFolder));

        stage->store(2);
        QTRY_COMPARE(withoutBreakMarks(bar.text->text()),
                     reading(QStringLiteral("plan.pdf"), QFileInfo(source).absolutePath()));
        QTest::qWait(750);
        QVERIFY(bar.bar->isVisible());
        stage->store(3);
        QTRY_COMPARE(withoutBreakMarks(bar.text->text()), savingTo(nextFolder));
        QVERIFY(bar.bar->isVisible());

        stage->store(4);
        QTRY_VERIFY_WITH_TIMEOUT(tab->isUsable(), 10000);
        QVERIFY(!bar.bar->isVisible());
    }

    // A write names its folder, a read the file and its folder, and a read in the
    // folder the copy goes to counts as saving there.
    void waitBarNamesWhatTheToolWaitsFor()
    {
        QFETCH(bool, write);
        QFETCH(bool, elsewhere);
        QTemporaryDir files;
        QVERIFY(QDir(files.path()).mkpath(QStringLiteral("docs")));
        QVERIFY(QDir(files.path()).mkpath(QStringLiteral("out")));
        const QString docs = files.filePath(QStringLiteral("docs"));
        const QString path = files.filePath(QStringLiteral("docs/plan.pdf"));
        const QString folder = elsewhere ? files.filePath(QStringLiteral("out")) : docs;
        QVERIFY(QFile::copy(QStringLiteral(MERVIN_FIXTURE_PDF), path));
        TabPage::setSlowDelayForTesting(0);
        RenderEngine engine;
        MainWindow window(&engine, nullptr);
        showForLayout(window);
        QVERIFY(mervin::testing::openAndWait(window, path));
        TabPage *tab = currentTab(window);
        QVERIFY(tab);
        const Bar bar(*tab);
        QVERIFY(bar.valid());
        if (write)
            holdAt(job::Step::Writing, folder);
        else
            holdAt(job::Step::Reading, path);

        rotate(window, QDir(folder).filePath(QStringLiteral("plan-rotated.pdf")));
        QTRY_COMPARE(job::testing::held(), 1);
        QTRY_VERIFY(bar.bar->isVisible());
        QCOMPARE(withoutBreakMarks(bar.text->text()),
                 write || !elsewhere ? savingTo(folder) : reading(QStringLiteral("plan.pdf"), docs));

        answers_ << QMessageBox::No; // "Open it now?"
        job::testing::release();
        QTRY_VERIFY_WITH_TIMEOUT(tab->isUsable(), 10000);
        QTRY_VERIFY(answers_.isEmpty());
    }

    // While one tab waits, its Save, Print and Document commands are off, and another
    // tab of the window and another window work as usual. A highlight drawn with the
    // mouse lands in the other tab's document.
    void onlyTheBusyTabWaits()
    {
        QTemporaryDir files;
        QVERIFY(QDir(files.path()).mkpath(QStringLiteral("out")));
        const QString busyPath = files.filePath(QStringLiteral("busy.pdf"));
        const QString otherPath = files.filePath(QStringLiteral("other.pdf"));
        const QString thirdPath = files.filePath(QStringLiteral("third.pdf"));
        const QString folder = files.filePath(QStringLiteral("out"));
        for (const QString &path : {busyPath, otherPath, thirdPath})
            QVERIFY(QFile::copy(QStringLiteral(MERVIN_FIXTURE_PDF), path));
        WindowManager manager;
        MainWindow *window = manager.createWindow();
        window->resize(1000, 800);
        QVERIFY(QTest::qWaitForWindowExposed(window));
        QVERIFY(mervin::testing::openAndWait(*window, otherPath));
        QVERIFY(mervin::testing::openAndWait(*window, busyPath));
        TabPage *busy = currentTab(*window);
        QVERIFY(busy);
        QCOMPARE(busy->path(), busyPath);
        const auto action = [window](const QKeySequence &key) -> QAction * {
            for (QAction *a : window->findChildren<QAction *>())
                if (a->shortcuts().contains(key))
                    return a;
            return nullptr;
        };
        QAction *save = action(QKeySequence(QKeySequence::Save));
        QAction *print = action(QKeySequence(QKeySequence::Print));
        QAction *comment = action(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_N));
        auto *document = window->findChild<QToolButton *>(QStringLiteral("documentButton"));
        QVERIFY(save && print && comment && document);
        QVERIFY(save->isEnabled() && print->isEnabled() && document->isEnabled());

        holdAt(job::Step::Writing, folder);
        rotate(*window, files.filePath(QStringLiteral("out/busy-rotated.pdf")));
        QVERIFY(busy->isBusy());
        QTRY_COMPARE(job::testing::held(), 1);
        for (QObject *command : {static_cast<QObject *>(save), static_cast<QObject *>(print),
                                 static_cast<QObject *>(comment), static_cast<QObject *>(document)})
            QVERIFY2(!command->property("enabled").toBool(), qPrintable(command->objectName()));

        // In the other tab the commands are on, and the Comment tool's markup drag adds a
        // highlight.
        auto *tabs = window->findChild<QTabWidget *>();
        QVERIFY(tabs);
        tabs->setCurrentIndex(0);
        TabPage *other = currentTab(*window);
        QCOMPARE(other->path(), otherPath);
        QVERIFY(other->isUsable());
        QVERIFY(save->isEnabled() && print->isEnabled() && document->isEnabled());
        ViewerWidget *viewer = other->viewer();
        QTRY_VERIFY(viewer->isVisible());
        comment->trigger();
        QVERIFY(viewer->commentToolEnabled());
        QVERIFY(viewer->highlightMode());
        // Across "The STANDARD TEST document" near the top of the first page, which
        // the viewer shows at its width from the top.
        const double scale = viewer->scale();
        const auto onPage = [&](double x, double y) {
            return QPoint(16 + qRound(x * scale), 16 + qRound(y * scale));
        };
        QWidget *pages = viewer->viewport();
        QTest::mousePress(pages, Qt::LeftButton, {}, onPage(40, 52));
        for (int x = 60; x <= 300; x += 40)
            QTest::mouseMove(pages, onPage(x, 52));
        QTest::mouseRelease(pages, Qt::LeftButton, {}, onPage(300, 52));
        QTRY_COMPARE(viewer->annotModel()->pageAnnots(0).size(), size_t(1));
        QCOMPARE(viewer->annotModel()->pageAnnots(0).front().type, AnnotType::Highlight);

        // Another window opens and shows a document.
        MainWindow *second = manager.createWindow();
        QVERIFY(QTest::qWaitForWindowExposed(second));
        QVERIFY(mervin::testing::openAndWait(*second, thirdPath));
        TabPage *third = currentTab(*second);
        QVERIFY(third && third->isUsable());
        QTRY_VERIFY(third->viewer()->isVisible());

        answers_ << QMessageBox::No; // "Open it now?"
        job::testing::release();
        QTRY_VERIFY_WITH_TIMEOUT(busy->isUsable(), 10000);
        QTRY_VERIFY(answers_.isEmpty());
        tabs->setCurrentIndex(1);
        QVERIFY(save->isEnabled() && print->isEnabled() && document->isEnabled());
        // The edited tab is closed without its question.
        other->viewer()->annotModel()->clearDirty();
    }

    // Stop waiting makes the tab usable at once. The copy the job held at its commit
    // is never written, and its temporary file goes once the job returns.
    void stopLeavesNoOutput()
    {
        QTemporaryDir files;
        QVERIFY(QDir(files.path()).mkpath(QStringLiteral("out")));
        const QString path = files.filePath(QStringLiteral("plan.pdf"));
        const QString out = files.filePath(QStringLiteral("out/plan-rotated.pdf"));
        QVERIFY(QFile::copy(QStringLiteral(MERVIN_FIXTURE_PDF), path));
        TabPage::setSlowDelayForTesting(0);
        RenderEngine engine;
        MainWindow window(&engine, nullptr);
        showForLayout(window);
        QVERIFY(mervin::testing::openAndWait(window, path));
        TabPage *tab = currentTab(window);
        QVERIFY(tab);
        const Bar bar(*tab);
        QVERIFY(bar.valid());
        holdAt(job::Step::Commit, out);
        QVERIFY(appfiles::flush(10000));
        const int tasks = background::running();
        rotate(window, out);
        // The job wrote its temporary file and waits to replace the destination.
        QTRY_COMPARE(job::testing::held(), 1);
        QTRY_VERIFY(bar.stop->isVisible());

        QSignalSpy finished(tab, &TabPage::operationFinished);
        QElapsedTimer timer;
        timer.start();
        QTest::mouseClick(bar.stop, Qt::LeftButton);
        QVERIFY2(timer.elapsed() < 1000, qPrintable(took("Stop waiting", timer)));
        QCOMPARE(finished.size(), 1);
        QCOMPARE(finished.first().first().toBool(), false);
        QVERIFY(tab->isUsable());
        QVERIFY(tab->viewer()->isVisible());
        QVERIFY(!bar.bar->isVisible());
        QVERIFY(!bar.still->isVisible());

        job::testing::release();
        QVERIFY(jobsEnded(tasks));
        QCoreApplication::processEvents();
        QVERIFY2(filesIn(QFileInfo(out).path()).isEmpty(),
                 qPrintable(filesIn(QFileInfo(out).path()).join(u' ')));
        QCOMPARE(finished.size(), 1);
    }

    void passwordChainStoresTheTypedPassword_data()
    {
        QTest::addColumn<QString>("typed");
        QTest::newRow("right") << QStringLiteral("secret");
        QTest::newRow("wrong") << QStringLiteral("guess");
    }

    // The tab's password no longer opens the file, so Rotate asks for one in the tab's
    // window and runs again with it. The tab keeps a password that worked.
    void passwordChainStoresTheTypedPassword()
    {
        QFETCH(QString, typed);
        const bool right = typed == QStringLiteral("secret");
        QTemporaryDir files;
        const QString path = files.filePath(QStringLiteral("locked.pdf"));
        const QString out = files.filePath(QStringLiteral("locked-rotated.pdf"));
        QVERIFY(QpdfService().encrypt(QStringLiteral(MERVIN_FIXTURE_PDF), path, {},
                                      QStringLiteral("secret"), {},
                                      QpdfService::Algorithm::AES256, {}, nullptr)
                == QpdfService::Status::Ok);
        RenderEngine engine;
        MainWindow window(&engine, nullptr);
        showForLayout(window);
        QVERIFY(mervin::testing::openAndWait(window, path, false, -1, true, QStringLiteral("secret")));
        TabPage *tab = currentTab(window);
        QVERIFY(tab);
        tab->setPassword({});

        QSignalSpy finished(tab, &TabPage::operationFinished);
        rotate(window, out);
        // Asked once the first run found the password wrong, after rotate() returned.
        inputs_ << typed;
        answers_ << (right ? QMessageBox::No : QMessageBox::Ok); // "Open it now?" or the failure
        QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 2, 10000);
        QTRY_VERIFY(answers_.isEmpty());
        QVERIFY(inputs_.isEmpty());
        QCOMPARE(tab->password(), right ? typed : QString());
        QCOMPARE(QFileInfo::exists(out), right);
        if (right) {
            QpdfService::Info info;
            QCOMPARE(QpdfService().readInfo(out, typed, info, nullptr), QpdfService::Status::Ok);
            QVERIFY(info.encrypted);
        } else {
            QCOMPARE(told_.last(), QStringLiteral("Operation failed: A password is required."));
        }
    }

    // A close continuation must let the tool's password completion run first,
    // then wait for the retried operation rather than deleting its tab early.
    void closeWaitIncludesPasswordRetry()
    {
        QTemporaryDir files;
        const QString path = files.filePath(QStringLiteral("locked.pdf"));
        const QString out = files.filePath(QStringLiteral("locked-rotated.pdf"));
        QCOMPARE(QpdfService().encrypt(QStringLiteral(MERVIN_FIXTURE_PDF), path, {},
                                       QStringLiteral("secret"), {},
                                       QpdfService::Algorithm::AES256, {}, nullptr),
                 QpdfService::Status::Ok);
        RenderEngine engine;
        MainWindow window(&engine, nullptr);
        showForLayout(window);
        QVERIFY(mervin::testing::openAndWait(window, path, false, -1, true, QStringLiteral("secret")));
        const QPointer<TabPage> tab(currentTab(window));
        QVERIFY(tab);
        tab->setPassword({});
        QSignalSpy finished(tab, &TabPage::operationFinished);
        holdAt(job::Step::Reading, path);
        rotate(window, out);
        QTRY_COMPARE(job::testing::held(), 1);
        inputs_ << QStringLiteral("secret");
        answers_ << QMessageBox::No;
        QVERIFY(QMetaObject::invokeMethod(&window, "closeTab", Q_ARG(int, 0)));
        QTRY_VERIFY(!Bar(*tab).bar->isHidden());
        job::testing::release();
        QTRY_VERIFY_WITH_TIMEOUT(!tab, 10000);
        QCOMPARE(finished.size(), 2);
        QVERIFY(inputs_.isEmpty() && answers_.isEmpty());
        QCOMPARE(window.tabCount(), 0);
        QVERIFY(QFileInfo::exists(out));
    }

    // While a test thread holds the busy tab's document, as a save job would, the
    // window answers switching tabs, the pointer over the tab, a resize, a theme change
    // and the form highlight setting at once, and asks the document nothing.
    void busyTabMakesNoDocumentCalls()
    {
        QTemporaryDir files;
        QVERIFY(QDir(files.path()).mkpath(QStringLiteral("out")));
        const QString busyPath = files.filePath(QStringLiteral("busy.pdf"));
        const QString otherPath = files.filePath(QStringLiteral("other.pdf"));
        const QString folder = files.filePath(QStringLiteral("out"));
        for (const QString &path : {busyPath, otherPath})
            QVERIFY(QFile::copy(QStringLiteral(MERVIN_FIXTURE_PDF), path));
        TabPage::setSlowDelayForTesting(0);
        WindowManager manager;
        MainWindow *window = manager.createWindow();
        window->resize(1000, 800);
        QVERIFY(QTest::qWaitForWindowExposed(window));
        QVERIFY(mervin::testing::openAndWait(*window, otherPath));
        QVERIFY(mervin::testing::openAndWait(*window, busyPath));
        TabPage *busy = currentTab(*window);
        QVERIFY(busy);
        const Bar bar(*busy);
        QVERIFY(bar.valid());
        holdAt(job::Step::Writing, folder);
        rotate(*window, files.filePath(QStringLiteral("out/busy-rotated.pdf")));
        QTRY_COMPARE(job::testing::held(), 1);
        QTRY_VERIFY(bar.bar->isVisible());

        QAction *highlightFields = nullptr;
        for (QAction *a : window->findChildren<QAction *>())
            if (a->text() == QStringLiteral("Highlight Form Fields"))
                highlightFields = a;
        QVERIFY(highlightFields);
        auto *tabs = window->findChild<QTabWidget *>();
        QVERIFY(tabs);
        const int refusals = Document::testing::refusals();
        SavingJob job{busy->viewer()->document()->lifetime()};
        Stall<SavingJob> held(job, 5000);
        QVERIFY(job.answered);
        // Each step returns, and the event loop runs a posted call, within a second.
        const auto answers = [&](const char *what, const std::function<void()> &step) {
            QElapsedTimer timer;
            timer.start();
            step();
            bool ran = false;
            QTimer::singleShot(0, window, [&ran] { ran = true; });
            while (!ran && timer.elapsed() < 1000)
                QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
            QVERIFY2(ran && timer.elapsed() < 1000, qPrintable(took(what, timer)));
            QVERIFY2(Document::testing::refusals() == refusals, what);
        };
        answers("switching to another tab", [&] { tabs->setCurrentIndex(0); });
        answers("switching back", [&] { tabs->setCurrentIndex(1); });
        answers("the pointer over the tab", [&] {
            for (int y = 10; y < busy->height(); y += 60)
                QTest::mouseMove(busy, QPoint(busy->width() / 2, y));
        });
        answers("a resize", [&] { window->resize(900, 700); });
        answers("the dark theme", [&] { manager.setColorScheme(QStringLiteral("dark")); });
        answers("the light theme", [&] { manager.setColorScheme(QStringLiteral("light")); });
        answers("the comfort page theme", [&] { manager.setDocumentTheme(QStringLiteral("comfort")); });
        answers("the form highlight setting", [&] { highlightFields->toggle(); });
        QVERIFY(busy->isBusy());

        held.release();
        answers_ << QMessageBox::No; // "Open it now?"
        job::testing::release();
        QTRY_VERIFY_WITH_TIMEOUT(busy->isUsable(), 10000);
        QTRY_VERIFY(answers_.isEmpty());
        highlightFields->toggle();
    }

    // Split stopped while it waits to commit its second file keeps only the first.
    void splitStopsBeforeTheNextFile()
    {
        QTemporaryDir files;
        QVERIFY(QDir(files.path()).mkpath(QStringLiteral("out")));
        const QString path = files.filePath(QStringLiteral("plan.pdf"));
        const QString folder = files.filePath(QStringLiteral("out"));
        QVERIFY(QFile::copy(QStringLiteral(MERVIN_FIXTURE_PDF), path));
        RenderEngine engine;
        MainWindow window(&engine, nullptr);
        showForLayout(window);
        QVERIFY(mervin::testing::openAndWait(window, path));
        TabPage *tab = currentTab(window);
        QVERIFY(tab);
        holdAt(job::Step::Commit, QDir(folder).filePath(QStringLiteral("plan-002.pdf")));
        QVERIFY(appfiles::flush(10000));
        const int tasks = background::running();
        split(window, folder);
        QVERIFY(tab->isBusy());
        QTRY_COMPARE(job::testing::held(), 1);
        tab->stopWaiting();
        QVERIFY(tab->isUsable());
        job::testing::release();
        QVERIFY(jobsEnded(tasks));
        QCoreApplication::processEvents();
        QCOMPARE(filesIn(folder), QStringList{QStringLiteral("plan-001.pdf")});
    }

    void pageToolRefusesADestinationOpenInATab_data()
    {
        QTest::addColumn<bool>("splitPages");
        QTest::newRow("split") << true;
        QTest::newRow("rotate") << false;
    }

    // Neither tool writes when its destination is open in a tab, and it says why.
    void pageToolRefusesADestinationOpenInATab()
    {
        QFETCH(bool, splitPages);
        QTemporaryDir files;
        QVERIFY(QDir(files.path()).mkpath(QStringLiteral("out")));
        const QString path = files.filePath(QStringLiteral("plan.pdf"));
        const QString folder = files.filePath(QStringLiteral("out"));
        const QString open = QDir(folder).filePath(QStringLiteral("plan-002.pdf"));
        QVERIFY(QFile::copy(QStringLiteral(MERVIN_FIXTURE_PDF), path));
        QVERIFY(QFile::copy(QStringLiteral(MERVIN_FIXTURE_PDF), open));
        const QByteArray before = [&open] {
            QFile file(open);
            return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
        }();
        RenderEngine engine;
        MainWindow window(&engine, nullptr);
        showForLayout(window);
        QVERIFY(mervin::testing::openAndWait(window, open));
        QVERIFY(mervin::testing::openAndWait(window, path));
        TabPage *tab = currentTab(window);
        QVERIFY(tab);
        QCOMPARE(tab->path(), path);
        QSignalSpy finished(tab, &TabPage::operationFinished);
        if (!splitPages)
            answers_ << QMessageBox::Yes; // the file picker's overwrite question
        answers_ << QMessageBox::Ok;
        if (splitPages)
            split(window, folder);
        else
            rotate(window, open);
        QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 10000);
        QTRY_VERIFY(answers_.isEmpty());
        QCOMPARE(told_.last(), splitPages
                                  ? QStringLiteral("Split Pages: The destination is open in a tab.")
                                  : QStringLiteral("Rotate Pages: The destination is open in a tab."));
        QCOMPARE(filesIn(folder), QStringList{QStringLiteral("plan-002.pdf")});
        QFile file(open);
        QVERIFY(file.open(QIODevice::ReadOnly));
        QCOMPARE(file.readAll(), before);
    }

    // The dialog must survive owner destruction during its nested event loop.
    void destroyingWindowWhileExportExplanationIsOpen()
    {
        QTemporaryDir files;
        const QString source = files.filePath(QStringLiteral("plan.pdf"));
        QVERIFY(QFile::copy(QStringLiteral(MERVIN_FIXTURE_PDF), source));
        WindowManager manager;
        const QPointer<MainWindow> host(manager.createWindow());
        const QPointer<MainWindow> other(manager.createWindow());
        showForLayout(*host);
        showForLayout(*other);
        QVERIFY(mervin::testing::openAndWait(*host, source));
        TabPage *tab = currentTab(*host);
        QVERIFY(tab);
        tab->viewer()->loadMeasurements({{0, MeasureKind::Distance, {{50, 100}, {150, 100}}}},
                                       {}, MeasureUnit::Millimeter, 2, 2.0);
        QVERIFY(!tab->hasUnsavedEdits());
        dialogs_.stop();
        bool requested = false;
        QTimer quit;
        connect(&quit, &QTimer::timeout, this, [&] {
            if (!qobject_cast<ExportMeasureDialog *>(QApplication::activeModalWidget()))
                return;
            quit.stop();
            requested = true;
            delete host.data();
        });
        quit.start(10);
        QVERIFY(QMetaObject::invokeMethod(host, "exportMeasuredCopy"));
        QVERIFY(requested);
        QTRY_VERIFY(host.isNull());
        QVERIFY(other);
    }

    // Supplies tool prompt types and queued Quit or direct owner destruction.
    void quittingWhileAToolPromptIsOpen_data()
    {
        QTest::addColumn<QString>("prompt");
        QTest::addColumn<bool>("destroyParent");
        QTest::newRow("password Quit") << QStringLiteral("password") << false;
        QTest::newRow("success Quit") << QStringLiteral("success") << false;
        QTest::newRow("failure Quit") << QStringLiteral("failure") << false;
        QTest::newRow("password parent destruction") << QStringLiteral("password") << true;
        QTest::newRow("success parent destruction") << QStringLiteral("success") << true;
        QTest::newRow("failure parent destruction") << QStringLiteral("failure") << true;
    }

    // Quit waits until the prompt is answered. Direct owner destruction also
    // keeps the inherited prompt-lifetime regression coverage.
    void quittingWhileAToolPromptIsOpen()
    {
        QFETCH(QString, prompt);
        QFETCH(bool, destroyParent);
        QTemporaryDir files;
        const QString source = files.filePath(QStringLiteral("plan.pdf"));
        const QString output = files.filePath(QStringLiteral("rotated.pdf"));
        if (prompt == QStringLiteral("password"))
            QCOMPARE(QpdfService().encrypt(QStringLiteral(MERVIN_FIXTURE_PDF), source, {},
                         QStringLiteral("secret"), {}, QpdfService::Algorithm::AES256, {}),
                     QpdfService::Status::Ok);
        else
            QVERIFY(QFile::copy(QStringLiteral(MERVIN_FIXTURE_PDF), source));
        WindowManager manager;
        const QPointer<MainWindow> host(manager.createWindow());
        const QPointer<MainWindow> other(manager.createWindow());
        showForLayout(*host);
        showForLayout(*other);
        QVERIFY(testing::openAndWait(*host, source, false, -1, true,
                                     prompt == QStringLiteral("password") ? QStringLiteral("secret")
                                                                          : QString()));
        TabPage *tab = currentTab(*host);
        QVERIFY(tab);
        if (prompt == QStringLiteral("password"))
            tab->setPassword({}); // the page tool must ask again
        if (prompt == QStringLiteral("failure"))
            job::testing::failWhen([folder = files.path()](job::Step step, const QString &path) {
                return step == job::Step::Writing && path == folder;
            });
        const auto restore = qScopeGuard([] { job::testing::failWhen({}); });
        rotate(*host, output);
        dialogs_.stop();
        bool requested = false;
        QTimer quit;
        connect(&quit, &QTimer::timeout, this, [&] {
            QWidget *modal = QApplication::activeModalWidget();
            const bool expected = prompt == QStringLiteral("password")
                ? qobject_cast<QInputDialog *>(modal) != nullptr
                : qobject_cast<QMessageBox *>(modal) != nullptr;
            if (!expected)
                return;
            quit.stop();
            requested = true;
            if (destroyParent) {
                delete host.data();
                delete other.data();
            } else {
                manager.closeAllForQuit();
                QVERIFY(host && other);
                modal->close();
            }
        });
        quit.start(10);
        QTRY_VERIFY_WITH_TIMEOUT(requested, 10000);
        QTRY_VERIFY(host.isNull() && other.isNull());
    }

    void runningToolsReserveEveryOutput_data()
    {
        QTest::addColumn<bool>("splitPages");
        QTest::newRow("rotate") << false;
        QTest::newRow("split") << true;
    }

    // A writer reserves all outputs before its first write. An attempted new tab
    // cannot admit an old version and save edits that the tool would then overwrite.
    void runningToolsReserveEveryOutput()
    {
        QFETCH(bool, splitPages);
        QTemporaryDir files;
        QVERIFY(QDir(files.path()).mkpath(QStringLiteral("out")));
        const QString source = files.filePath(QStringLiteral("plan.pdf"));
        const QString folder = files.filePath(QStringLiteral("out"));
        const QString output = QDir(folder).filePath(QStringLiteral("plan-002.pdf"));
        QVERIFY(QFile::copy(QStringLiteral(MERVIN_FIXTURE_PDF), source));
        QVERIFY(QFile::copy(QStringLiteral(MERVIN_FIXTURE_PDF), output));
        RenderEngine engine;
        MainWindow window(&engine, nullptr);
        showForLayout(window);
        QVERIFY(testing::openAndWait(window, source));
        TabPage *tab = currentTab(window);
        QVERIFY(tab);
        job::testing::holdWhen([folder, splitPages, writes = 0](
                                  job::Step step, const QString &path) mutable {
            if (step != job::Step::Writing || path != folder)
                return false;
            ++writes;
            return !splitPages || writes > 1; // Split's first report is its preflight
        });
        if (splitPages)
            split(window, folder);
        else {
            answers_ << QMessageBox::Yes;
            rotate(window, output);
        }
        QTRY_COMPARE(job::testing::held(), 1);
        QVERIFY(window.openFile(output));
        auto *pending = currentTab(window);
        QVERIFY(pending && pending != tab);
        QTRY_VERIFY(!pending->isLoading());
        QCOMPARE(window.tabCount(), 2);
        QCOMPARE(pending->findChild<QLabel *>(QStringLiteral("documentLoadStatus"))->text(),
                 TabPage::pathBeingWrittenError());
        tab->stopWaiting();
        QTest::mouseClick(pending->findChild<QPushButton *>(QStringLiteral("retryDocumentLoad")), Qt::LeftButton);
        QTRY_VERIFY(pending->isLoaded()); // canceled writer releases admission at once
        QCOMPARE(window.tabCount(), 2);
        job::testing::release();
        QTRY_COMPARE(job::testing::held(), 0);
        QTRY_COMPARE(filesIn(folder), QStringList{QStringLiteral("plan-002.pdf")});
    }

    // Split cannot know its names until qpdf reads the source. A tab admitted before
    // that reservation wins, and Split must preserve the edits it saved meanwhile.
    void tabOpenedDuringSplitPreventsEveryWrite()
    {
        QTemporaryDir files;
        QVERIFY(QDir(files.path()).mkpath(QStringLiteral("out")));
        const QString source = files.filePath(QStringLiteral("plan.pdf"));
        const QString folder = files.filePath(QStringLiteral("out"));
        const QString output = QDir(folder).filePath(QStringLiteral("plan-002.pdf"));
        QVERIFY(QFile::copy(QStringLiteral(MERVIN_FIXTURE_PDF), source));
        QVERIFY(QFile::copy(QStringLiteral(MERVIN_FIXTURE_PDF), output));
        WindowManager manager;
        MainWindow *host = manager.createWindow();
        showForLayout(*host);
        QVERIFY(testing::openAndWait(*host, source));
        TabPage *busy = currentTab(*host);
        QSignalSpy finished(busy, &TabPage::operationFinished);
        holdAt(job::Step::Reading, source);
        split(*host, folder);
        QTRY_COMPARE(job::testing::held(), 1);
        MainWindow *editor = manager.createWindow();
        showForLayout(*editor);
        QVERIFY(testing::openAndWait(*editor, output));
        QVERIFY(currentTab(*editor)->viewer()->annotModel()->addTextNote(
                    0, {50, 50}, Qt::yellow, {}, QStringLiteral("Saved during Split")) >= 0);
        QVERIFY(QMetaObject::invokeMethod(editor, "saveMeasurements"));
        QVERIFY(!currentTab(*editor)->hasUnsavedEdits());
        answers_ << QMessageBox::Ok;
        job::testing::release();
        QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 10000);
        QTRY_VERIFY(answers_.isEmpty());
        QVERIFY(told_.last().contains(QStringLiteral("The destination is open in a tab.")));
        QCOMPARE(filesIn(folder), QStringList{QStringLiteral("plan-002.pdf")});
        auto saved = manager.engine()->openDocument(output);
        QVERIFY(saved);
        const auto notes = AnnotModel(*saved).pageAnnots(0);
        QCOMPARE(notes.size(), 1u);
        QCOMPARE(notes.front().contents, QStringLiteral("Saved during Split"));
    }

    // A tab can move to another window while it waits. Its tool asks there once done.
    void finishedToolAsksInTheTabsWindow()
    {
        QTemporaryDir files;
        QVERIFY(QDir(files.path()).mkpath(QStringLiteral("out")));
        const QString busyPath = files.filePath(QStringLiteral("busy.pdf"));
        const QString otherPath = files.filePath(QStringLiteral("other.pdf"));
        const QString folder = files.filePath(QStringLiteral("out"));
        for (const QString &path : {busyPath, otherPath})
            QVERIFY(QFile::copy(QStringLiteral(MERVIN_FIXTURE_PDF), path));
        WindowManager manager;
        MainWindow *first = manager.createWindow();
        QVERIFY(QTest::qWaitForWindowExposed(first));
        QVERIFY(mervin::testing::openAndWait(*first, otherPath));
        QVERIFY(mervin::testing::openAndWait(*first, busyPath));
        TabPage *busy = currentTab(*first);
        QVERIFY(busy);
        holdAt(job::Step::Writing, folder);
        rotate(*first, files.filePath(QStringLiteral("out/busy-rotated.pdf")));
        QTRY_COMPARE(job::testing::held(), 1);

        manager.detachTab(first, 1, QPoint(60, 60));
        auto *second = qobject_cast<MainWindow *>(busy->window());
        QVERIFY(second && second != first);
        QCOMPARE(first->tabCount(), 1);
        QVERIFY(busy->isBusy());
        answers_ << QMessageBox::No; // "Open it now?"
        job::testing::release();
        QTRY_VERIFY_WITH_TIMEOUT(busy->isUsable(), 10000);
        QTRY_VERIFY(answers_.isEmpty());
        QCOMPARE(toldIn_.size(), 1);
        QCOMPARE(toldIn_.first().data(), static_cast<QWidget *>(second));
    }

    // Supplies result kinds and tab movement for discarded owner queues.
    void movedToolResultSurvivesItsQueuedOwner_data()
    {
        QTest::addColumn<QString>("result");
        QTest::addColumn<bool>("unmoved");
        QTest::newRow("success") << QStringLiteral("success") << false;
        QTest::newRow("failure") << QStringLiteral("failure") << false;
        QTest::newRow("password") << QStringLiteral("password") << false;
        QTest::newRow("unmoved success") << QStringLiteral("success") << true;
        QTest::newRow("unmoved failure") << QStringLiteral("failure") << true;
        QTest::newRow("unmoved password") << QStringLiteral("password") << true;
    }

    // Checks result delivery in a moved tab's surviving window and silence after tab destruction.
    void movedToolResultSurvivesItsQueuedOwner()
    {
        QFETCH(QString, result);
        QFETCH(bool, unmoved);
        QTemporaryDir files;
        const QString source = files.filePath(QStringLiteral("plan.pdf"));
        const QString kept = files.filePath(QStringLiteral("kept.pdf"));
        const QString output = files.filePath(QStringLiteral("rotated.pdf"));
        QVERIFY(QFile::copy(QStringLiteral(MERVIN_FIXTURE_PDF), kept));
        if (result == QStringLiteral("password")) {
            QCOMPARE(QpdfService().encrypt(QStringLiteral(MERVIN_FIXTURE_PDF), source, {},
                         QStringLiteral("secret"), {}, QpdfService::Algorithm::AES256, {}),
                     QpdfService::Status::Ok);
        } else {
            QVERIFY(QFile::copy(QStringLiteral(MERVIN_FIXTURE_PDF), source));
        }
        WindowManager manager;
        QPointer<MainWindow> first(manager.createWindow());
        MainWindow *second = manager.createWindow();
        showForLayout(*first);
        showForLayout(*second);
        QVERIFY(testing::openAndWait(*second, kept));
        QVERIFY(testing::openAndWait(*first, source, false, -1, true,
                     result == QStringLiteral("password") ? QStringLiteral("secret") : QString()));
        QPointer<TabPage> page(currentTab(*first));
        if (result == QStringLiteral("password"))
            page->setPassword({});
        if (result == QStringLiteral("failure"))
            job::testing::failWhen([folder = files.path()](job::Step step, const QString &path) {
                return step == job::Step::Writing && path == folder;
            });
        const auto restore = qScopeGuard([] { job::testing::failWhen({}); });
        holdAt(job::Step::Reading, source);
        rotate(*first, output);
        QTRY_COMPARE(job::testing::held(), 1);
        QDialog guard;
        guard.setWindowModality(Qt::ApplicationModal);
        guard.show();
        QSignalSpy finished(page, &TabPage::operationFinished);
        job::testing::release();
        QTRY_COMPARE(finished.size(), 1);
        QVERIFY(page->isUsable());
        QVERIFY(told_.isEmpty());
        if (unmoved) {
            delete first.data();
            guard.close();
            QTest::qWait(100);
            QVERIFY(!first && !page);
            QVERIFY(told_.isEmpty());
            QVERIFY(unexpected_.isEmpty());
            QCOMPARE(second->tabPaths(), QStringList{kept});
            return;
        }
        second->adoptTab(first->releaseTab(0));
        // Destroy the former owner while the result still waits in its queue.
        delete first.data();
        QVERIFY(!first && page && page->window() == second);
        if (result == QStringLiteral("password"))
            inputs_ << QStringLiteral("secret");
        answers_ << (result == QStringLiteral("failure") ? QMessageBox::Ok : QMessageBox::No);
        guard.close();
        QTRY_VERIFY_WITH_TIMEOUT(answers_.isEmpty(), 1000);
        QVERIFY(inputs_.isEmpty());
        QCOMPARE(toldIn_.size(), 1);
        QCOMPARE(toldIn_.front().data(), static_cast<QWidget *>(second));
        QVERIFY(page && page->isUsable());
        QCOMPARE(QFileInfo::exists(output), result != QStringLiteral("failure"));
        if (result == QStringLiteral("password"))
            QCOMPARE(page->password(), QStringLiteral("secret"));
    }

    // A completion observer can destroy its sender before the tool delivers its
    // result. Use direct destruction because an ordinary close is now queued.
    void completionSlotMayCloseItsTab()
    {
        QTemporaryDir files;
        const QString path = files.filePath(QStringLiteral("plan.pdf"));
        const QString out = files.filePath(QStringLiteral("plan-rotated.pdf"));
        QVERIFY(QFile::copy(QStringLiteral(MERVIN_FIXTURE_PDF), path));
        RenderEngine engine;
        MainWindow window(&engine, nullptr);
        showForLayout(window);
        QVERIFY(mervin::testing::openAndWait(window, path));
        const QPointer<TabPage> tab(currentTab(window));
        QVERIFY(tab);
        connect(tab, &TabPage::operationFinished, &window, [tab](bool completed) {
            if (completed)
                delete tab.data();
        });

        rotate(window, out);
        QTRY_VERIFY_WITH_TIMEOUT(tab.isNull(), 10000);
        QCOMPARE(window.tabCount(), 0);
        QVERIFY(QFileInfo::exists(out));
        QVERIFY(told_.isEmpty());
    }

    void stateChangeMayCloseAnEndingOperation_data()
    {
        QTest::addColumn<bool>("stop");
        QTest::newRow("completion") << false;
        QTest::newRow("stop") << true;
    }

    // State observers can close the tab as soon as its document becomes usable.
    void stateChangeMayCloseAnEndingOperation()
    {
        QFETCH(bool, stop);
        RenderEngine engine;
        MainWindow window(&engine, nullptr);
        showForLayout(window);
        QVERIFY(mervin::testing::openAndWait(window, QStringLiteral(MERVIN_FIXTURE_PDF)));
        const QPointer<TabPage> tab(currentTab(window));
        QVERIFY(tab);
        connect(tab, &TabPage::stateChanged, &window, [tab] {
            if (tab && tab->isUsable())
                delete tab.data();
        });
        const QString path = tab->path();
        holdAt(job::Step::Reading, path);
        bool delivered = false;
        TabPage::Operation operation;
        operation.work = [path] { job::reading(path); };
        operation.finished = [&delivered] { delivered = true; };
        tab->startOperation(std::move(operation));
        QTRY_COMPARE(job::testing::held(), 1);
        if (stop)
            tab->stopWaiting();
        job::testing::release();

        QTRY_VERIFY_WITH_TIMEOUT(tab.isNull(), 10000);
        QCOMPARE(window.tabCount(), 0);
        QVERIFY(!delivered);
    }

    // Starting an operation also allows a state observer to close the tab safely.
    void stateChangeMayCloseAStartingOperation()
    {
        RenderEngine engine;
        MainWindow window(&engine, nullptr);
        showForLayout(window);
        QVERIFY(mervin::testing::openAndWait(window, QStringLiteral(MERVIN_FIXTURE_PDF)));
        const QPointer<TabPage> tab(currentTab(window));
        QVERIFY(tab);
        connect(tab, &TabPage::stateChanged, &window, [tab] {
            if (tab && tab->isBusy())
                delete tab.data();
        });
        const QString path = tab->path();
        holdAt(job::Step::Reading, path);
        bool delivered = false;
        TabPage::Operation operation;
        operation.work = [path] { job::reading(path); };
        operation.finished = [&delivered] { delivered = true; };
        tab->startOperation(std::move(operation));

        QVERIFY(tab.isNull());
        QCOMPARE(window.tabCount(), 0);
        QTRY_COMPARE(job::testing::held(), 1);
        job::testing::release();
        QVERIFY(!delivered);
    }

    // A window hidden in the tray shows nothing once its tab's tool is done.
    void trayWindowAsksNothing()
    {
        QTemporaryDir files;
        QVERIFY(QDir(files.path()).mkpath(QStringLiteral("out")));
        const QString path = files.filePath(QStringLiteral("plan.pdf"));
        const QString folder = files.filePath(QStringLiteral("out"));
        const QString out = files.filePath(QStringLiteral("out/plan-rotated.pdf"));
        QVERIFY(QFile::copy(QStringLiteral(MERVIN_FIXTURE_PDF), path));
        RenderEngine engine;
        MainWindow window(&engine, nullptr);
        showForLayout(window);
        QVERIFY(mervin::testing::openAndWait(window, path));
        TabPage *tab = currentTab(window);
        QVERIFY(tab);
        holdAt(job::Step::Writing, folder);
        rotate(window, out);
        QTRY_COMPARE(job::testing::held(), 1);
        window.hideToTray();
        QVERIFY(window.isInTray());
        QVERIFY(tab->isBusy()); // the tray does not unload a busy tab

        job::testing::release();
        QTRY_VERIFY_WITH_TIMEOUT(QFileInfo::exists(out), 10000);
        QTRY_VERIFY(!tab->isBusy());
        QTest::qWait(200); // the window's queue runs its changes meanwhile
        QVERIFY(told_.isEmpty());
        window.restoreFromTray();
    }

    // Supplies window and tab close variants for Save after stopping the operation wait.
    void stoppingCloseWaitContinuesWithSave_data()
    {
        QTest::addColumn<bool>("window"); // close the window, or else the tab
        QTest::newRow("tab") << false;
        QTest::newRow("window") << true;
    }

    void cancelingCloseKeepsTheOperation_data()
    {
        QTest::addColumn<QString>("intent");
        QTest::newRow("tab") << QStringLiteral("tab");
        QTest::newRow("window") << QStringLiteral("window");
        QTest::newRow("quit canceled in a later window") << QStringLiteral("quit");
    }

    // A canceled close must leave the requested tool running, including tools in
    // earlier windows whose close questions already passed during a canceled quit.
    void cancelingCloseKeepsTheOperation()
    {
        QFETCH(QString, intent);
        QTemporaryDir files;
        const QString path = files.filePath(QStringLiteral("plan.pdf"));
        const QString out = files.filePath(QStringLiteral("plan-rotated.pdf"));
        QVERIFY(QFile::copy(QStringLiteral(MERVIN_FIXTURE_PDF), path));
        WindowManager manager;
        MainWindow *host = manager.createWindow();
        showForLayout(*host);
        QVERIFY(mervin::testing::openAndWait(*host, path));
        const QPointer<TabPage> tab(currentTab(*host));
        QVERIFY(tab);
        if (intent != QStringLiteral("quit"))
            QVERIFY(tab->viewer()->annotModel()->addTextNote(
                        0, {50, 50}, Qt::yellow, {}, QStringLiteral("Kept")) >= 0);
        holdAt(job::Step::Reading, path);
        rotate(*host, out);
        QTRY_COMPARE(job::testing::held(), 1);
        QSignalSpy finished(tab, &TabPage::operationFinished);

        if (intent == QStringLiteral("quit")) {
            const QString laterPath = files.filePath(QStringLiteral("later.pdf"));
            QVERIFY(QFile::copy(QStringLiteral(MERVIN_FIXTURE_PDF), laterPath));
            MainWindow *later = manager.createWindow();
            showForLayout(*later);
            QVERIFY(mervin::testing::openAndWait(*later, laterPath));
            QVERIFY(currentTab(*later)->viewer()->annotModel()->addTextNote(
                        0, {50, 50}, Qt::yellow, {}, QStringLiteral("Kept")) >= 0);
        }
        answers_ << QMessageBox::No << QMessageBox::Cancel;
        if (intent == QStringLiteral("tab"))
            QVERIFY(QMetaObject::invokeMethod(host, "closeTab", Q_ARG(int, 0)));
        else if (intent == QStringLiteral("window"))
            QVERIFY(!host->close());
        else
            manager.closeAllForQuit();
        QTRY_VERIFY(!Bar(*tab).bar->isHidden());
        QCOMPARE(answers_.size(), 2);
        QVERIFY(tab && tab->isBusy());
        QCOMPARE(finished.size(), 0);

        job::testing::release();
        QTRY_VERIFY_WITH_TIMEOUT(tab && tab->isUsable(), 10000);
        QTRY_VERIFY(answers_.isEmpty());
        QCOMPARE(finished.size(), 1);
        QVERIFY(finished.first().first().toBool());
        QVERIFY(QFileInfo::exists(out));
    }

    // Close waits for the operation. Stop continues with the dirty question,
    // and the chosen Save writes the edits. The stopped tool creates no copy.
    void stoppingCloseWaitContinuesWithSave()
    {
        QFETCH(bool, window);
        QTemporaryDir files;
        QVERIFY(QDir(files.path()).mkpath(QStringLiteral("out")));
        const QString path = files.filePath(QStringLiteral("plan.pdf"));
        const QString out = files.filePath(QStringLiteral("out/plan-rotated.pdf"));
        QVERIFY(QFile::copy(QStringLiteral(MERVIN_FIXTURE_PDF), path));
        RenderEngine engine;
        auto host = std::make_unique<MainWindow>(&engine, nullptr);
        showForLayout(*host);
        QVERIFY(mervin::testing::openAndWait(*host, path));
        TabPage *tab = currentTab(*host);
        QVERIFY(tab);
        QVERIFY(tab->viewer()->annotModel()->addTextNote(0, {50, 50}, Qt::yellow, {},
                                                         QStringLiteral("Kept"))
                >= 0);
        // Held before the tool opens the file, which an in-place save on Windows could
        // not replace while it is open.
        holdAt(job::Step::Reading, path);
        QVERIFY(appfiles::flush(10000));
        const int tasks = background::running();
        rotate(*host, out);
        QTRY_COMPARE(job::testing::held(), 1);
        QVERIFY(tab->isBusy());

        answers_ << QMessageBox::Save; // "Save changes to plan.pdf?"
        QElapsedTimer timer;
        timer.start();
        if (window)
            QVERIFY(!host->close());
        else
            QVERIFY(QMetaObject::invokeMethod(host.get(), "closeTab", Q_ARG(int, 0)));
        QVERIFY2(timer.elapsed() < 1000, qPrintable(took("closing", timer)));
        QTRY_VERIFY(!Bar(*tab).bar->isHidden());
        QCOMPARE(answers_.size(), 1);
        tab->stopWaiting();
        QTRY_VERIFY(answers_.isEmpty());
        if (window)
            QTRY_VERIFY(!host->isVisible());
        else
            QTRY_COMPARE(host->tabCount(), 0);
        host.reset();

        job::testing::release();
        QVERIFY(jobsEnded(tasks));
        QCoreApplication::processEvents();
        QVERIFY(filesIn(QFileInfo(out).path()).isEmpty());
        auto saved = engine.openDocument(path);
        QVERIFY(saved);
        const auto notes = AnnotModel(*saved).pageAnnots(0);
        QCOMPARE(notes.size(), size_t(1));
        QCOMPARE(notes.front().contents, QStringLiteral("Kept"));
    }

private:
    static void copyModes()
    {
        QTest::addColumn<bool>("flatten");
        QTest::newRow("copy") << false;
        QTest::newRow("export") << true;
    }

    static void prepareCopy(TabPage &tab)
    {
        tab.viewer()->loadMeasurements({{0, MeasureKind::Distance, {{50, 100}, {150, 100}}}},
                                       {}, MeasureUnit::Millimeter, 2, 2.0);
        QVERIFY(tab.viewer()->annotModel()->addTextNote(
                    0, {50, 50}, Qt::yellow, {}, QStringLiteral("Kept edit")) >= 0);
    }

    void copy(MainWindow &window, const QString &output, bool flatten)
    {
        files_ << output;
        QVERIFY(QMetaObject::invokeMethod(&window, flatten ? "exportMeasuredCopy" : "saveAsCopy"));
        QVERIFY(files_.isEmpty());
    }

    // Runs Rotate Pages on the tab `window` shows, for every page, a quarter turn
    // clockwise, into `out`.
    void rotate(MainWindow &window, const QString &out)
    {
        inputs_ << QString() << QString(); // the pages offered, all, and the first angle
        files_ << out;
        QVERIFY(QMetaObject::invokeMethod(&window, "rotatePagesOp"));
        QVERIFY(files_.isEmpty());
    }

    // Runs Split on the tab `window` shows, into `folder`.
    void split(MainWindow &window, const QString &folder)
    {
        files_ << folder;
        QVERIFY(QMetaObject::invokeMethod(&window, "splitDocument"));
        QVERIFY(files_.isEmpty());
    }

    std::unique_ptr<QTemporaryDir> profile_;
    QTimer dialogs_;
    QPointer<QWidget> answeredModal_;
    QPointer<QFileDialog> submittedFileDialog_;
    QList<QMessageBox::StandardButton> answers_; // for the boxes, in order
    QStringList inputs_;                         // for the prompts, in order
    QStringList files_;                          // for the file dialogs, in order
    QStringList told_;                           // every box shown, "title: text"
    QList<QPointer<QWidget>> toldIn_;            // the window of each box shown
    QStringList unexpected_;                     // dialogs nobody answered
};

QTEST_MAIN(TstTabOperations)
#include "tst_tab_operations.moc"
