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
#include <QTimer>
#include <QtTest>

#include <functional>
#include <memory>

using namespace mervin;

namespace {
QByteArray bytes(const QString &path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

bool writeBytes(const QString &path, const QByteArray &data)
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(data) == data.size();
}
} // namespace

class TstMergeOperations : public QObject
{
    Q_OBJECT

public:
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
                if (box->text().contains(QStringLiteral("Replace it?")))
                    box->button(QMessageBox::Yes)->click();
                else if (box->windowTitle() == QStringLiteral("Done")) {
                    successes_++;
                    box->button(QMessageBox::No)->click();
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
        reopenedSpec_.clear();
        reopenedOutput_.clear();
        reopenedError_.clear();
        unexpected_.clear();
        repair_ = beforeMerge_ = {};
        elapsed_.start();
        dialogs_.start(10);
    }

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

    void stoppedMergeKeepsDestination_data()
    {
        QTest::addColumn<bool>("writing");
        QTest::newRow("input reading") << false;
        QTest::newRow("destination writing") << true;
    }

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
        job::testing::release();
        QTRY_VERIFY_WITH_TIMEOUT(tab && !tab->isBusy(), 10000);
        QCoreApplication::processEvents();
        QCoreApplication::processEvents();
        QVERIFY(!source->canChangeTabs());
        // Force the queue's owner to vanish while the tab survives elsewhere.
        target->adoptTab(source->releaseTab(0));
        delete source.data();
        modal.hide();
        QTRY_COMPARE_WITH_TIMEOUT(dialogsSeen_, 2, 10000);
        QCOMPARE(reopenedSpec_, spec_);
        QCOMPARE(reopenedOutput_, QDir::toNativeSeparators(output_));
        QVERIFY(reopenedError_.contains(QStringLiteral("source.pdf")));
        QVERIFY(tab && tab->window() == target);
        QVERIFY(!QFileInfo::exists(output_));
    }

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
    int dialogsSeen_ = 0;
    int successes_ = 0;
    std::function<void()> repair_, beforeMerge_;
    QTimer dialogs_;
    QElapsedTimer elapsed_;
};

QTEST_MAIN(TstMergeOperations)
#include "tst_merge_operations.moc"
