#include "app/WindowManager.h"
#include "config/ConfigPaths.h"
#include "config/Settings.h"
#include "dialogs/MergeDialog.h"
#include "io/AppFiles.h"
#include "io/Background.h"
#include "io/JobContext.h"
#include "io/SaveCheck.h"
#include "render/Document.h"
#include "render/MeasureContent.h"
#include "render/RenderEngine.h"
#include "security/DocumentOutput.h"
#include "security/MeasureExport.h"
#include "security/PageOps.h"
#include "ui/MainWindow.h"
#include "ui/TabPage.h"
#include "ui/ViewerWidget.h"
#include "TestWaits.h"

#include <QApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPointer>
#include <QPushButton>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QTabWidget>
#include <QTimer>
#include <QtTest>

#include <functional>
#include <memory>

using namespace mervin;

namespace {
/// Reads exact destination bytes for preservation assertions.
QByteArray bytes(const QString &path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

/// Seeds an existing destination for Stop and replacement checks.
bool writeBytes(const QString &path, const QByteArray &data)
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(data) == data.size();
}
} // namespace

/// Exercises worker writes, recovery dialogs and result delivery through real controls.
class TstMergeOperations : public QObject
{
    Q_OBJECT

public:
    /// Drives Merge controls and nested questions while recording delivered results.
    TstMergeOperations()
    {
        QApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
        connect(&dialogs_, &QTimer::timeout, this, [this] {
            QWidget *modal = QApplication::activeModalWidget();
            if (elapsed_.elapsed() > 20000) {
                if (unexpected_.isEmpty())
                    unexpected_.append(QStringLiteral("Merge interaction timed out"));
                if (auto *dialog = qobject_cast<QDialog *>(modal))
                    dialog->reject();
                return;
            }
            if (auto *box = qobject_cast<QMessageBox *>(modal)) {
                if (beforeMessage_)
                    beforeMessage_(box);
                if (box->text().contains(QStringLiteral("Replace it?")))
                    box->button(QMessageBox::Yes)->click();
                else if (box->windowTitle() == QStringLiteral("Done")) {
                    successes_++;
                    box->button(QMessageBox::No)->click();
                } else if (box->windowTitle() == QStringLiteral("Unsaved changes")) {
                    quitAnswered_ = true;
                    box->button(QMessageBox::Cancel)->click();
                } else {
                    unexpected_.append(box->text());
                    box->reject();
                }
                return;
            }
            auto *dialog = qobject_cast<MergeDialog *>(modal);
            if (!dialog || dialog->reading())
                return;
            auto *check = dialog->findChild<SaveCheck *>();
            if (!check || check->pending())
                return;
            if (current_ != dialog) {
                current_ = dialog;
                submitted_ = false;
                dialogsSeen_++;
                const auto fields = dialog->findChildren<QLineEdit *>("mergeRowSpec");
                auto *output = dialog->findChild<QLineEdit *>("mergeOutput");
                if (fields.isEmpty() || !output) {
                    unexpected_.append(QStringLiteral("Merge controls were missing"));
                    dialog->reject();
                    return;
                }
                if (dialogsSeen_ == 1) {
                    fields.front()->setText(spec_);
                    output->setFocus();
                    output->selectAll();
                    QTest::keyClicks(output, QDir::toNativeSeparators(output_));
                    return;
                }
                reopenedSpec_ = fields.front()->text();
                reopenedOutput_ = output->text();
                reopenedError_ = dialog->findChild<QLabel *>("mergeError")->text();
                if (beforeReopen_)
                    beforeReopen_(dialog);
                if (repair_)
                    repair_();
                else {
                    dialog->reject();
                    return;
                }
            }
            if (check->pending() || submitted_)
                return;
            auto *merge = dialog->findChild<QPushButton *>("mergeAccept");
            if (merge && merge->isEnabled()) {
                submitted_ = true;
                if (beforeMerge_) {
                    const auto change = std::exchange(beforeMerge_, {});
                    change();
                }
                QTimer::singleShot(0, merge, &QPushButton::click);
            }
        });
    }

private slots:
    /// Creates isolated source files, settings and dialog answers for each case.
    void init()
    {
        files_ = std::make_unique<QTemporaryDir>();
        QVERIFY(files_->isValid());
        ConfigPaths::setOverrideDir(files_->filePath("profile"));
        Settings settings;
        settings.autoUpdate = false;
        QVERIFY(settings.save());
        source_ = files_->filePath("source.pdf");
        output_ = files_->filePath("merged.pdf");
        QVERIFY(QFile::copy(QStringLiteral(MERVIN_FIXTURE_PDF), source_));
        spec_ = QStringLiteral("2-3");
        current_.clear();
        dialogsSeen_ = successes_ = 0;
        submitted_ = false;
        quitAnswered_ = false;
        reopenedSpec_.clear();
        reopenedOutput_.clear();
        reopenedError_.clear();
        unexpected_.clear();
        repair_ = beforeMerge_ = {};
        beforeReopen_ = {};
        beforeMessage_ = {};
        elapsed_.start();
        dialogs_.start(10);
    }

    /// Releases held work and waits for background file cleanup between cases.
    void cleanup()
    {
        job::testing::release();
        dialogs_.stop();
        QVERIFY(background::settle(10000));
        QCoreApplication::processEvents();
        QVERIFY(Document::waitForCloses(10000));
        QVERIFY(appfiles::flush(10000));
        ConfigPaths::setOverrideDir({});
        files_.reset();
        QVERIFY2(unexpected_.isEmpty(), qPrintable(unexpected_.join('\n')));
    }

    /// Checks that retry preserves ranges, output choice and remapped measurements.
    void failedWriteKeepsThePlan()
    {
        RenderEngine engine;
        QString error;
        auto source = engine.openDocument(source_, {}, &error);
        QVERIFY2(source, qPrintable(error));
        MeasureDoc measurements;
        measurements.measurements.push_back({1, MeasureKind::Distance,
                                            {{40, 40}, {80, 40}}});
        const QString measured = files_->filePath("measured.pdf");
        QVERIFY2(DocumentOutput::snapshot(*source, measurements, measured, {}, &error),
                 qPrintable(error));
        source.reset();
        QVERIFY(QFile::remove(source_));
        QVERIFY(QFile::rename(measured, source_));
        QVERIFY(QFile::copy(source_, output_));
        QVERIFY(QFile::setPermissions(output_, QFileDevice::ReadOwner | QFileDevice::ReadUser));
        repair_ = [this] {
            QVERIFY(QFile::setPermissions(output_, QFileDevice::ReadOwner | QFileDevice::WriteOwner));
        };
        MainWindow window(&engine, nullptr);
        QVERIFY(testing::openAndWait(window, source_));
        QVERIFY(QMetaObject::invokeMethod(&window, "mergeDocuments"));
        QTRY_COMPARE_WITH_TIMEOUT(successes_, 1, 10000);
        QCOMPARE(dialogsSeen_, 2);
        QCOMPARE(reopenedSpec_, spec_);
        QCOMPARE(reopenedOutput_, QDir::toNativeSeparators(output_));
        QVERIFY(reopenedError_.contains(QStringLiteral("Could not write")));
        int pages = 0;
        QCOMPARE(PageOps::probe(output_, &pages, {}), PageOps::Status::Ok);
        QCOMPARE(pages, 2);
        const auto blob = MeasureExport::readMervinBlob(output_);
        QVERIFY(blob);
        MeasureDoc retained;
        QVERIFY(parseMeasurements(*blob, &retained));
        QCOMPARE(retained.measurements.size(), size_t(1));
        QCOMPARE(retained.measurements.front().page, 0);
        QCOMPARE(retained.measurements.front().pts, measurements.measurements.front().pts);
    }

    /// Checks that a removed input reopens its plan with the failing filename.
    void failedInputNamesItsRow()
    {
        beforeMerge_ = [this] { QVERIFY(QFile::remove(source_)); };
        RenderEngine engine;
        MainWindow window(&engine, nullptr);
        QVERIFY(testing::openAndWait(window, source_));
        QVERIFY(QMetaObject::invokeMethod(&window, "mergeDocuments"));
        QTRY_COMPARE_WITH_TIMEOUT(dialogsSeen_, 2, 10000);
        QCOMPARE(reopenedSpec_, spec_);
        QCOMPARE(reopenedOutput_, QDir::toNativeSeparators(output_));
        QVERIFY(reopenedError_.contains(QStringLiteral("source.pdf")));
        QVERIFY(!QFileInfo::exists(output_));
    }

    /// Covers Stop during the input read and destination write.
    void stoppedMergeKeepsDestination_data()
    {
        QTest::addColumn<bool>("writing");
        QTest::newRow("input reading") << false;
        QTest::newRow("destination writing") << true;
    }

    /// Covers a newly created destination and changed bytes of a confirmed existing one.
    void changedDestinationReopensThePlan_data()
    {
        QTest::addColumn<bool>("existing");
        QTest::newRow("absent output") << false;
        QTest::newRow("confirmed output") << true;
    }

    /// Rejects stale consent even when the competing change finishes before worker validation.
    void changedDestinationReopensThePlan()
    {
        QFETCH(bool, existing);
        const QByteArray original("original contents");
        const QByteArray changed("changed contents!");
        QCOMPARE(original.size(), changed.size());
        if (existing)
            QVERIFY(writeBytes(output_, original));
        int questions = 0;
        beforeMessage_ = [&questions](QMessageBox *box) {
            if (box->text().contains(QStringLiteral("Replace it?")))
                ++questions;
        };
        beforeMerge_ = [this] {
            job::testing::holdWhen([folder = files_->path(), first = true]
                                  (job::Step step, const QString &path) mutable {
                if (step != job::Step::Writing || path != folder || !first)
                    return false;
                first = false;
                return true;
            });
        };
        const auto release = qScopeGuard([] { job::testing::release(); });
        RenderEngine engine;
        MainWindow window(&engine, nullptr);
        QVERIFY(testing::openAndWait(window, source_));
        const int guiSteps = job::testing::stepsOnMainThread();
        QVERIFY(QMetaObject::invokeMethod(&window, "mergeDocuments"));
        QTRY_COMPARE_WITH_TIMEOUT(job::testing::held(), 1, 10000);
        QCOMPARE(questions, existing ? 1 : 0);
        QVERIFY(writeBytes(output_, changed));
        job::testing::release();
        QTRY_COMPARE_WITH_TIMEOUT(dialogsSeen_, 2, 10000);
        QCOMPARE(reopenedSpec_, spec_);
        QCOMPARE(reopenedOutput_, QDir::toNativeSeparators(output_));
        QCOMPARE(reopenedError_, QStringLiteral("The destination changed. Press Merge again to review it."));
        QCOMPARE(bytes(output_), changed);
        QCOMPARE(successes_, 0);
        QCOMPARE(job::testing::stepsOnMainThread(), guiSteps);
    }

    /// Keeps the version shown by the first question and asks again before replacing newer bytes.
    void replacementQuestionKeepsItsOwnConsent()
    {
        const QByteArray original("original contents");
        const QByteArray changed("changed contents!");
        QVERIFY(writeBytes(output_, original));
        int questions = 0;
        beforeMessage_ = [this, &questions, changed](QMessageBox *box) {
            if (box->text().contains(QStringLiteral("Replace it?")) && ++questions == 1)
                QVERIFY(writeBytes(output_, changed));
        };
        beforeReopen_ = [this, changed](MergeDialog *) { QCOMPARE(bytes(output_), changed); };
        repair_ = [] {}; // retry must obtain fresh consent through the existing replacement question
        RenderEngine engine;
        MainWindow window(&engine, nullptr);
        QVERIFY(testing::openAndWait(window, source_));
        QVERIFY(QMetaObject::invokeMethod(&window, "mergeDocuments"));
        QTRY_COMPARE_WITH_TIMEOUT(successes_, 1, 10000);
        QCOMPARE(dialogsSeen_, 2);
        QCOMPARE(questions, 2);
        QCOMPARE(reopenedError_, QStringLiteral("The destination changed. Press Merge again to review it."));
        QCOMPARE(PageOps::pageCount(output_), 2);
    }

    /// Stop during the final streamed destination check preserves bytes and suppresses completion.
    void stopDuringDestinationConsentRead()
    {
        const QByteArray original(3 * 1024 * 1024, 'x');
        QVERIFY(writeBytes(output_, original));
        beforeMessage_ = [this](QMessageBox *box) {
            if (box->text().contains(QStringLiteral("Replace it?")))
                job::testing::holdWhen([output = output_, reads = 0]
                                      (job::Step step, const QString &path) mutable {
                    return step == job::Step::Reading && path == output && ++reads == 3;
                });
        };
        const auto release = qScopeGuard([] { job::testing::release(); });
        RenderEngine engine;
        MainWindow window(&engine, nullptr);
        QVERIFY(testing::openAndWait(window, source_));
        auto *tab = window.findChild<TabPage *>();
        QVERIFY(tab);
        const int tasks = background::running();
        QVERIFY(QMetaObject::invokeMethod(&window, "mergeDocuments"));
        QTRY_COMPARE_WITH_TIMEOUT(job::testing::held(), 1, 10000);
        QVERIFY(tab->isBusy());
        tab->stopWaiting();
        QVERIFY(tab->isUsable());
        job::testing::release();
        QTRY_VERIFY_WITH_TIMEOUT(background::running() <= tasks, 10000);
        QCOMPARE(bytes(output_), original);
        QCOMPARE(dialogsSeen_, 1);
        QCOMPARE(successes_, 0);
    }

    /// Checks that failure recovery selects its source after the user switches tabs.
    void failedMergeSelectsItsLaunchingTab()
    {
        beforeMerge_ = [this] {
            job::testing::holdWhen([source = source_, reads = 0]
                                  (job::Step step, const QString &path) mutable {
                return step == job::Step::Reading && path == source && ++reads == 2;
            });
        };
        const auto release = qScopeGuard([] { job::testing::release(); });
        WindowManager manager;
        MainWindow *window = manager.createWindow();
        QVERIFY(testing::openAndWait(*window, source_));
        const QPointer<TabPage> tab(window->findChild<TabPage *>());
        auto *tabs = window->findChild<QTabWidget *>();
        QVERIFY(QMetaObject::invokeMethod(window, "mergeDocuments"));
        QTRY_COMPARE_WITH_TIMEOUT(job::testing::held(), 1, 10000);
        const QString kept = files_->filePath(QStringLiteral("kept.pdf"));
        QVERIFY(QFile::copy(QStringLiteral(MERVIN_FIXTURE_PDF), kept));
        QVERIFY(testing::openAndWait(*window, kept));
        QVERIFY(tabs->currentWidget() != tab);
        beforeReopen_ = [window, tabs, tab](MergeDialog *dialog) {
            QCOMPARE(dialog->parentWidget(), window);
            QCOMPARE(tabs->currentWidget(), tab.data());
        };
        QVERIFY(QFile::remove(source_));
        job::testing::release();
        QTRY_COMPARE_WITH_TIMEOUT(dialogsSeen_, 2, 10000);
        QCOMPARE(reopenedSpec_, spec_);
        QCOMPARE(reopenedOutput_, QDir::toNativeSeparators(output_));
        QVERIFY(reopenedError_.contains(QStringLiteral("source.pdf")));
        QVERIFY(!QFileInfo::exists(output_));
    }

    /// Covers success and failure around canceled and confirmed Quit.
    void queuedMergeResultsRespectQuit_data()
    {
        QTest::addColumn<bool>("failure");
        QTest::addColumn<bool>("cancel");
        QTest::newRow("success after canceled quit") << false << true;
        QTest::newRow("failure after canceled quit") << true << true;
        QTest::newRow("success during confirmed quit") << false << false;
        QTest::newRow("failure during confirmed quit") << true << false;
    }

    /// Checks result delivery after cancellation and suppression after confirmed shutdown.
    void queuedMergeResultsRespectQuit()
    {
        QFETCH(bool, failure);
        QFETCH(bool, cancel);
        beforeMerge_ = [this] {
            job::testing::holdWhen([source = source_, reads = 0]
                                  (job::Step step, const QString &path) mutable {
                return step == job::Step::Reading && path == source && ++reads == 2;
            });
        };
        const auto release = qScopeGuard([] { job::testing::release(); });
        WindowManager manager;
        const QPointer<MainWindow> first(manager.createWindow());
        const QPointer<MainWindow> second(manager.createWindow());
        QVERIFY(testing::openAndWait(*first, source_));
        const QPointer<TabPage> tab(first->findChild<TabPage *>());
        const QString kept = files_->filePath(QStringLiteral("kept.pdf"));
        QVERIFY(QFile::copy(QStringLiteral(MERVIN_FIXTURE_PDF), kept));
        QVERIFY(testing::openAndWait(*second, kept));
        if (cancel) {
            auto *keptTab = second->findChild<TabPage *>();
            keptTab->viewer()->loadMeasurements({{0, MeasureKind::Distance, {{15, 80}, {85, 80}}}},
                                                {}, MeasureUnit::Millimeter, 2, 2);
            keptTab->viewer()->setMeasurePrecision(3);
            QVERIFY(keptTab->hasUnsavedEdits());
        }
        bool deliveredAfterShutdown = false;
        beforeMessage_ = [&](QMessageBox *) {
            deliveredAfterShutdown = deliveredAfterShutdown || manager.isClosing();
        };
        beforeReopen_ = [&](MergeDialog *) {
            deliveredAfterShutdown = deliveredAfterShutdown || manager.isClosing();
        };
        QVERIFY(QMetaObject::invokeMethod(first, "mergeDocuments"));
        QTRY_COMPARE_WITH_TIMEOUT(job::testing::held(), 1, 10000);
        QDialog guard;
        guard.setWindowModality(Qt::ApplicationModal);
        guard.show();
        if (failure)
            QVERIFY(QFile::remove(source_));
        job::testing::release();
        QTRY_VERIFY_WITH_TIMEOUT(tab->isUsable(), 10000);
        manager.quitAll();
        guard.close();
        if (cancel) {
            QTRY_VERIFY(quitAnswered_);
            QVERIFY(!manager.isClosing());
            QTRY_COMPARE(dialogsSeen_, failure ? 2 : 1);
            QTRY_COMPARE(successes_, failure ? 0 : 1);
            QVERIFY(first && second && tab);
        } else {
            QTRY_VERIFY(manager.isClosing());
            QTRY_VERIFY(!first && !second && !tab);
            QTest::qWait(50);
        }
        QVERIFY(!deliveredAfterShutdown);
    }

    /// Checks that an earlier queued last-tab move preserves its later failure result.
    void queuedFailureSurvivesOldOwnerDestruction()
    {
        beforeMerge_ = [this] {
            job::testing::holdWhen([source = source_, reads = 0]
                                  (job::Step step, const QString &path) mutable {
                return step == job::Step::Reading && path == source && ++reads == 2;
            });
        };
        const auto release = qScopeGuard([] { job::testing::release(); });
        WindowManager manager;
        const QPointer<MainWindow> source = manager.createWindow();
        MainWindow *target = manager.createWindow();
        QVERIFY(testing::openAndWait(*source, source_));
        const QPointer<TabPage> tab(source->findChild<TabPage *>());
        QVERIFY(QMetaObject::invokeMethod(source, "mergeDocuments"));
        QTRY_COMPARE_WITH_TIMEOUT(job::testing::held(), 1, 10000);
        QVERIFY(QFile::remove(source_));
        QDialog modal;
        modal.setWindowModality(Qt::ApplicationModal);
        modal.show();
        // An earlier queued move closes the old owner before its result runs.
        source->whenTabsCanChange([&manager, source, target] {
            if (source)
                manager.mergeTab(source, 0, target, -1);
        });
        job::testing::release();
        QTRY_VERIFY_WITH_TIMEOUT(tab && !tab->isBusy(), 10000);
        QCoreApplication::processEvents();
        QCoreApplication::processEvents();
        QVERIFY(!source->canChangeTabs());
        modal.hide();
        QTRY_VERIFY(!source);
        QTRY_COMPARE_WITH_TIMEOUT(dialogsSeen_, 2, 10000);
        QCOMPARE(reopenedSpec_, spec_);
        QCOMPARE(reopenedOutput_, QDir::toNativeSeparators(output_));
        QVERIFY(reopenedError_.contains(QStringLiteral("source.pdf")));
        QVERIFY(tab && tab->window() == target);
        QVERIFY(!QFileInfo::exists(output_));
    }

    /// Checks exact prior destination bytes and no late UI after Stop and release.
    void stoppedMergeKeepsDestination()
    {
        QFETCH(bool, writing);
        const QByteArray original("Prior destination bytes");
        QVERIFY(writeBytes(output_, original));
        beforeMerge_ = [this, writing] {
            job::testing::holdWhen([source = source_, folder = files_->path(), writing,
                                   reads = 0, writes = 0]
                                  (job::Step step, const QString &path) mutable {
                if (writing)
                    return step == job::Step::Writing && path == folder && ++writes == 2;
                return step == job::Step::Reading && path == source && ++reads == 2;
            });
        };
        const auto release = qScopeGuard([] { job::testing::release(); });
        RenderEngine engine;
        MainWindow window(&engine, nullptr);
        QVERIFY(testing::openAndWait(window, source_));
        auto *tab = window.findChild<TabPage *>();
        QVERIFY(tab);
        const int tasks = background::running();
        const int guiSteps = job::testing::stepsOnMainThread();
        QElapsedTimer elapsed;
        elapsed.start();
        QVERIFY(QMetaObject::invokeMethod(&window, "mergeDocuments"));
        QVERIFY2(elapsed.elapsed() < 1000,
                 qPrintable(QString("Merge took %1 ms").arg(elapsed.elapsed())));
        QTRY_COMPARE_WITH_TIMEOUT(job::testing::held(), 1, 10000);
        QVERIFY(tab->isBusy());
        QCOMPARE(job::testing::stepsOnMainThread(), guiSteps);
        tab->stopWaiting();
        QVERIFY(tab->isUsable());
        QCOMPARE(bytes(output_), original);
        job::testing::release();
        QTRY_VERIFY_WITH_TIMEOUT(background::running() <= tasks, 10000);
        QCOMPARE(bytes(output_), original);
        QCOMPARE(dialogsSeen_, 1);
        QCOMPARE(successes_, 0);
    }

private:
    std::unique_ptr<QTemporaryDir> files_;
    QString source_, output_, spec_;
    QString reopenedSpec_, reopenedOutput_, reopenedError_;
    QStringList unexpected_;
    QPointer<MergeDialog> current_;
    bool submitted_ = false;
    bool quitAnswered_ = false;
    int dialogsSeen_ = 0;
    int successes_ = 0;
    std::function<void()> repair_, beforeMerge_;
    std::function<void(MergeDialog *)> beforeReopen_;
    std::function<void(QMessageBox *)> beforeMessage_;
    QTimer dialogs_;
    QElapsedTimer elapsed_;
};

QTEST_MAIN(TstMergeOperations)
#include "tst_merge_operations.moc"
