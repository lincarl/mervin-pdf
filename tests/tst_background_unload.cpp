#include "TestWaits.h"
#include "Stalls.h"
#include "app/WindowManager.h"
#include "config/ConfigPaths.h"
#include "config/Settings.h"
#include "io/AppFiles.h"
#include "io/Background.h"
#include "io/JobContext.h"
#include "render/AnnotModel.h"
#include "render/Document.h"
#include "render/RenderEngine.h"
#include "ui/MainWindow.h"
#include "ui/TabPage.h"
#include "ui/ViewerWidget.h"

#include <QDir>
#include <QFile>
#include <QLabel>
#include <QMessageBox>
#include <QPointer>
#include <QPushButton>
#include <QScopeGuard>
#include <QSignalSpy>
#include <QStatusBar>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QTimer>
#include <QtTest>

#include <mupdf/fitz.h>

#include <optional>

using namespace mervin;

namespace {
/// Creates an isolated source fixture for checkpoint and replacement checks.
QString copy(QTemporaryDir &files, const QString &name,
             const QString &source = QStringLiteral(MERVIN_FIXTURE_PDF))
{
    const QString path = files.filePath(name);
    return QFile::copy(source, path) ? path : QString();
}

/// Reads fixture bytes for original and destination preservation checks.
QByteArray bytes(const QString &path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

/// Adds a retained annotation through the real viewer model.
bool edit(TabPage &tab, const QString &text = QStringLiteral("Retained edit"))
{
    return tab.viewer()->annotModel()->addTextNote(0, {40, 40}, Qt::yellow, {}, text) >= 0;
}

/// Lists checkpoint files to detect abandoned recovery work.
QStringList recoveryEntries()
{
    return QDir(QDir(ConfigPaths::configDirPath()).filePath(QStringLiteral("recovery")))
        .entryList(QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot);
}

/// Whether MuPDF's shared resource store lists an entry whose key contains `key`, as
/// MuPDF's store dump describes it, for example "(image " for a decoded image, or no
/// value when MuPDF could not write the dump. MuPDF formats each key after it releases
/// the store lock, so call this only while no other thread trims or filters the store.
/// Reaps wait until the dump has finished.
std::optional<bool> storeLists(fz_context *base, const char *key)
{
    fz_buffer *buffer = nullptr;
    fz_output *out = nullptr;
    bool found = false;
    bool dumped = false;
    fz_var(buffer);
    fz_var(out);
    fz_var(found);
    fz_var(dumped);
    fz_defer_reap_start(base);
    fz_try(base) {
        // MuPDF writes the dump while holding its allocation lock, so the buffer must
        // not need to grow. A few store entries take well under a kilobyte.
        buffer = fz_new_buffer(base, 1 << 16);
        out = fz_new_output_with_buffer(base, buffer);
        fz_debug_store(base, out);
        fz_close_output(base, out);
        unsigned char *data = nullptr;
        const size_t size = fz_buffer_storage(base, buffer, &data);
        found = QByteArray::fromRawData(reinterpret_cast<const char *>(data),
                                        static_cast<qsizetype>(size))
                    .contains(key);
        dumped = true;
    }
    fz_always(base) {
        fz_drop_output(base, out);
        fz_drop_buffer(base, buffer);
        fz_defer_reap_end(base);
    }
    fz_catch(base) {
        dumped = false;
    }
    return dumped ? std::optional<bool>(found) : std::nullopt;
}

/// Reads MuPDF's glyph byte counter while no worker is rasterizing.
std::optional<quint64> glyphBytes(fz_context *ctx)
{
    fz_buffer *buffer = nullptr;
    fz_output *out = nullptr;
    quint64 bytes = 0;
    bool parsed = false;
    fz_var(buffer);
    fz_var(out);
    fz_var(bytes);
    fz_var(parsed);
    fz_try(ctx) {
        buffer = fz_new_buffer(ctx, 1024);
        out = fz_new_output_with_buffer(ctx, buffer);
        fz_dump_glyph_cache_stats(ctx, out);
        fz_close_output(ctx, out);
        unsigned char *data = nullptr;
        const size_t size = fz_buffer_storage(ctx, buffer, &data);
        const QByteArray dump(reinterpret_cast<const char *>(data), qsizetype(size));
        const QByteArray prefix("Glyph Cache Size: ");
        if (dump.startsWith(prefix))
            bytes = dump.mid(prefix.size()).split('\n').first().toULongLong(&parsed);
    }
    fz_always(ctx) {
        fz_drop_output(ctx, out);
        fz_drop_buffer(ctx, buffer);
    }
    fz_catch(ctx) {
        parsed = false;
    }
    return parsed ? std::optional<quint64>(bytes) : std::nullopt;
}

/// A value the test puts in MuPDF's shared store, keyed by its own address. Give it
/// static storage, since the store can outlive the test that filled it.
struct StoreProbe
{
    fz_storable storable;
};

/// Leaves the probe alone, since the test owns its storage.
void dropProbe(fz_context *, fz_storable *) {}

/// Store type for StoreProbe entries, hashed by address as MuPDF hashes pointer keys.
const fz_store_type probeType = {
    "test probe",
    [](fz_context *, fz_store_hash *hash, void *key) {
        hash->u.pi.ptr = key;
        return 1;
    },
    [](fz_context *, void *key) { return key; },
    [](fz_context *, void *) {},
    [](fz_context *, void *a, void *b) { return a == b ? 0 : 1; },
    [](fz_context *, char *buf, size_t size, void *) { fz_strlcpy(buf, "(test probe)", size); },
    nullptr,
};

/// Puts `probe` in the shared store. A held probe keeps the test's reference as well as
/// the store's, as a thread that has just found an entry holds it.
void storeProbe(fz_context *base, StoreProbe &probe, bool held)
{
    FZ_INIT_STORABLE(&probe, 1, dropProbe);
    fz_store_item(base, &probe, &probe, sizeof probe, &probeType);
    if (!held)
        fz_drop_storable(base, &probe.storable);
}

/// Whether the shared store still holds `probe`.
bool storeHolds(fz_context *base, StoreProbe &probe)
{
    void *found = fz_find_item(base, dropProbe, &probe, &probeType);
    fz_drop_storable(base, static_cast<fz_storable *>(found));
    return found == &probe;
}
}

/// Exercises checkpoint ownership through selection, Stop, close and adoption.
class TstBackgroundUnload : public QObject
{
    Q_OBJECT
    std::unique_ptr<QTemporaryDir> profile_;

private slots:
    /// Creates an isolated profile with deterministic activity and wait-bar settings.
    void init()
    {
        profile_ = std::make_unique<QTemporaryDir>();
        QVERIFY(profile_->isValid());
        ConfigPaths::setOverrideDir(profile_->path());
        Settings settings;
        settings.autoUpdate = false;
        settings.unloadInactiveMinutes = 0;
        QVERIFY(settings.save());
        TabPage::setSlowDelayForTesting(0);
    }

    /// Releases stalls and drains document cleanup before removing the profile.
    void cleanup()
    {
        job::testing::release();
        job::testing::failWhen({});
        QCoreApplication::processEvents();
        QVERIFY(background::settle(10000));
        QVERIFY(Document::waitForCloses(10000));
        QVERIFY(appfiles::flush(10000));
        ConfigPaths::setOverrideDir({});
        TabPage::setSlowDelayForTesting(TabPage::kSlowMs);
        profile_.reset();
    }

    /// Keeps path capture free of directory creation while preserving normal config setup.
    void profilePathDoesNotCreateItsDirectory()
    {
        const QString path = profile_->filePath(QStringLiteral("not-created/profile"));
        ConfigPaths::setOverrideDir(path);
        QCOMPARE(ConfigPaths::configDirPath(), path);
        QVERIFY(!QFileInfo::exists(path));
        QCOMPARE(ConfigPaths::configDir(), path);
        QVERIFY(QFileInfo(path).isDir());
    }

    /// Keeps the activity timer responsive while document access is held elsewhere.
    void activityTickReturnsDuringAnActualDocumentStall()
    {
        QTemporaryDir files;
        const QString firstPath = copy(files, QStringLiteral("inactive.pdf"));
        const QString secondPath = copy(files, QStringLiteral("active.pdf"));
        QVERIFY(!firstPath.isEmpty() && !secondPath.isEmpty());
        RenderEngine engine;
        MainWindow window(&engine, nullptr);
        window.show();
        QVERIFY(testing::openAndWait(window, firstPath));
        auto *first = window.findChild<TabPage *>();
        QVERIFY(first && edit(*first));
        QVERIFY(testing::openAndWait(window, secondPath));
        auto *tabs = window.findChild<QTabWidget *>();
        QVERIFY(tabs);
        auto *second = qobject_cast<TabPage *>(tabs->currentWidget());
        QVERIFY(second && second != first);
        testing::Stall stall(first->viewer()->document()->accessMutex(), 20000);
        const int guiSteps = job::testing::stepsOnMainThread();
        window.updateDocumentActivity(0, 1);
        QElapsedTimer elapsed;
        elapsed.start();
        QVERIFY(window.updateDocumentActivity(61000, 1));
        QVERIFY2(elapsed.elapsed() < 1000, qPrintable(QString::number(elapsed.elapsed())));
        QVERIFY(first->isUnloading() && first->isBusy() && first->isLoaded());
        QVERIFY(window.hasResidentDocuments());
        QVERIFY(first->hasPendingRecoveryWrite());
        QVERIFY(edit(*second, QStringLiteral("Another tab stays editable")));
        QCOMPARE(job::testing::stepsOnMainThread(), guiSteps);
        stall.release();
        QTRY_VERIFY_WITH_TIMEOUT(first->isSuspended(), 10000);
        QVERIFY(first->hasRecoverySnapshot());
        tabs->setCurrentWidget(first);
        QTRY_VERIFY_WITH_TIMEOUT(first->isUsable(), 10000);
        QCOMPARE(first->viewer()->annotModel()->pageAnnots(0).size(), size_t(1));
        QCOMPARE(second->viewer()->annotModel()->pageAnnots(0).size(), size_t(1));
        QCOMPARE(first->viewer()->annotModel()->pageAnnots(0).front().contents,
                 QStringLiteral("Retained edit"));
        QCOMPARE(second->viewer()->annotModel()->pageAnnots(0).front().contents,
                 QStringLiteral("Another tab stays editable"));
    }

    /// Preserves the live document when selection wins the first-Hold race.
    void selectionBeforeReservationCancelsWithoutRecoveryFiles()
    {
        RenderEngine engine;
        TabPage tab(&engine);
        QVERIFY(tab.open(QStringLiteral(MERVIN_FIXTURE_PDF)));
        QVERIFY(edit(tab));
        Document *document = tab.viewer()->document();
        const QString directory = QDir(profile_->path()).filePath(QStringLiteral("recovery"));
        job::testing::holdWhen([directory](job::Step step, const QString &path) {
            return step == job::Step::Writing && path == directory;
        });
        const auto release = qScopeGuard([] { job::testing::release(); });
        QVERIFY(tab.suspend());
        QTRY_COMPARE(job::testing::held(), 1);
        QVERIFY(tab.hasPendingRecoveryWrite());
        tab.selectForUse();
        QVERIFY(tab.isUsable());
        QVERIFY(!tab.isUnloading());
        QCOMPARE(tab.viewer()->document(), document);
        QVERIFY(tab.hasUnsavedEdits());
        job::testing::release();
        engine.shutdown();
        QVERIFY(background::settle(10000));
        QVERIFY(appfiles::flush(10000));
        QVERIFY(recoveryEntries().isEmpty());
    }

    /// Applies selection protection before the later resume callback can run.
    void selectingTheActualTabCancelsBeforeTheQueuedResume()
    {
        QTemporaryDir files;
        const QString inactive = copy(files, QStringLiteral("inactive.pdf"));
        const QString active = copy(files, QStringLiteral("active.pdf"));
        QVERIFY(!inactive.isEmpty() && !active.isEmpty());
        RenderEngine engine;
        MainWindow window(&engine, nullptr);
        window.show();
        QVERIFY(testing::openAndWait(window, inactive));
        auto *tabs = window.findChild<QTabWidget *>();
        QVERIFY(tabs);
        const QPointer<TabPage> first = qobject_cast<TabPage *>(tabs->currentWidget());
        QVERIFY(first && edit(*first));
        Document *document = first->viewer()->document();
        QVERIFY(testing::openAndWait(window, active));
        const QString directory = QDir(profile_->path()).filePath(QStringLiteral("recovery"));
        job::testing::holdWhen([directory](job::Step step, const QString &path) {
            return step == job::Step::Writing && path == directory;
        });
        const auto release = qScopeGuard([] { job::testing::release(); });
        window.updateDocumentActivity(0, 1);
        QVERIFY(window.updateDocumentActivity(61000, 1));
        QTRY_COMPARE(job::testing::held(), 1);
        QVERIFY(first->isUnloading());
        tabs->setCurrentWidget(first);
        // No event processing between the actual selection and this assertion.
        QVERIFY(first->isUsable());
        QCOMPARE(first->viewer()->document(), document);
        job::testing::release();
        engine.shutdown();
        QVERIFY(background::settle(10000));
        QVERIFY(appfiles::flush(10000));
        QVERIFY(recoveryEntries().isEmpty());
    }

    /// Covers selection during Hold and after its first reservation has released.
    void selectionAfterReservation_data()
    {
        QTest::addColumn<bool>("afterRelease");
        QTest::newRow("during snapshot Hold") << false;
        QTest::newRow("after Hold before manifest") << true;
    }

    /// Retains the live document and removes a checkpoint completed after selection.
    void selectionAfterReservation()
    {
        QFETCH(bool, afterRelease);
        RenderEngine engine;
        TabPage tab(&engine);
        QVERIFY(tab.open(QStringLiteral(MERVIN_FIXTURE_PDF)));
        QVERIFY(edit(tab));
        Document *document = tab.viewer()->document();
        job::testing::holdWhen([afterRelease](job::Step step, const QString &path) {
            return afterRelease ? step == job::Step::Commit && path.endsWith(QStringLiteral(".json"))
                : step == job::Step::Writing && QFileInfo(path).fileName().startsWith(QStringLiteral(".unload-"));
        });
        const auto release = qScopeGuard([] { job::testing::release(); });
        QVERIFY(tab.suspend());
        QTRY_COMPARE(job::testing::held(), 1);
        tab.selectForUse();
        QVERIFY(tab.isBusy());
        QVERIFY(tab.isUnloading());
        QVERIFY(tab.hasPendingRecoveryWrite());
        QVERIFY(!tab.isOperationStopped());
        job::testing::release();
        // Let the worker commit, without processing its GUI adoption callback.
        QElapsedTimer elapsed;
        elapsed.start();
        while (tab.hasPendingRecoveryWrite() && elapsed.elapsed() < 10000)
            QThread::msleep(1);
        QVERIFY(!tab.hasPendingRecoveryWrite());
        QCOMPARE(TabPage::recoverablePaths().size(), 1);
        QTRY_VERIFY_WITH_TIMEOUT(tab.isUsable(), 10000);
        QCOMPARE(tab.viewer()->document(), document);
        QVERIFY(tab.hasUnsavedEdits());
        QVERIFY(!tab.hasRecoverySnapshot());
        engine.shutdown();
        QVERIFY(background::settle(10000));
        QVERIFY(appfiles::flush(10000));
        QVERIFY(recoveryEntries().isEmpty());
    }

    /// Keeps stopped snapshots locked until the worker releases document access.
    void stopDuringHoldKeepsTheTrappedNotice()
    {
        RenderEngine engine;
        TabPage tab(&engine);
        tab.show();
        QVERIFY(tab.open(QStringLiteral(MERVIN_FIXTURE_PDF)));
        QVERIFY(edit(tab));
        job::testing::holdWhen([](job::Step step, const QString &path) {
            return step == job::Step::Writing && QFileInfo(path).fileName().startsWith(QStringLiteral(".unload-"));
        });
        const auto release = qScopeGuard([] { job::testing::release(); });
        QVERIFY(tab.suspend());
        QTRY_COMPARE(job::testing::held(), 1);
        tab.stopWaiting();
        QVERIFY(tab.isBusy());
        QVERIFY(!tab.isUsable());
        auto *stop = tab.findChild<QPushButton *>(QStringLiteral("waitBarStop"));
        QVERIFY(stop && stop->isHidden());
        auto *notice = tab.findChild<QLabel *>(QStringLiteral("waitBarText"));
        QVERIFY(notice);
        QCOMPARE(notice->text(), TabPage::tr(
            "Stopped waiting. The document stays locked until the drive answers."));
        job::testing::release();
        QTRY_VERIFY_WITH_TIMEOUT(tab.isUsable(), 10000);
        QVERIFY(tab.hasUnsavedEdits());
        engine.shutdown();
        QVERIFY(background::settle(10000));
        QVERIFY(appfiles::flush(10000));
        QVERIFY(recoveryEntries().isEmpty());
    }

    /// Covers durability observations for loaded Save and edited unload.
    void workerManifestCommitEndsTheLocalDurabilityWindow_data()
    {
        QTest::addColumn<bool>("save");
        QTest::newRow("loaded Save") << true;
        QTest::newRow("edited unload") << false;
    }

    /// Publishes durable recovery before queued GUI adoption.
    void workerManifestCommitEndsTheLocalDurabilityWindow()
    {
        QFETCH(bool, save);
        QTemporaryDir files;
        const QString path = copy(files, QStringLiteral("edited.pdf"));
        QVERIFY(!path.isEmpty());
        RenderEngine engine;
        TabPage tab(&engine);
        QVERIFY(tab.open(path));
        QVERIFY(edit(tab));
        job::testing::holdWhen([](job::Step step, const QString &file) {
            return step == job::Step::Commit && file.endsWith(QStringLiteral(".json"));
        });
        const auto release = qScopeGuard([] { job::testing::release(); });
        if (save)
            tab.startSave({});
        else
            QVERIFY(tab.suspend());
        QVERIFY(tab.hasPendingRecoveryWrite());
        QTRY_COMPARE(job::testing::held(), 1);
        QVERIFY(TabPage::recoverablePaths().isEmpty());
        job::testing::release();
        QElapsedTimer elapsed;
        elapsed.start();
        while (tab.hasPendingRecoveryWrite() && elapsed.elapsed() < 10000)
            QThread::msleep(1);
        QVERIFY(!tab.hasPendingRecoveryWrite());
        // Save waits at adoption and unload waits in the posted GUI callback.
        QCOMPARE(TabPage::recoverablePaths(), QStringList{path});
        QVERIFY(tab.isBusy());
        tab.stopWaiting();
        QVERIFY(tab.isUsable());
        QVERIFY(tab.hasUnsavedEdits());
        engine.shutdown();
        QVERIFY(background::settle(10000));
        QCoreApplication::processEvents();
        QVERIFY(background::settle(10000));
        QVERIFY(appfiles::flush(10000));
        QVERIFY(recoveryEntries().isEmpty());
    }

    /// Covers owner deletion during snapshot access and an admitted manifest commit.
    void deletingTheTabDoesNotWaitOrKeepNewRecoveryFiles_data()
    {
        QTest::addColumn<bool>("committing");
        QTest::newRow("snapshot Hold") << false;
        QTest::newRow("manifest commit already began") << true;
    }

    /// Lets deleted owners leave worker cleanup without waiting or keeping orphan files.
    void deletingTheTabDoesNotWaitOrKeepNewRecoveryFiles()
    {
        QFETCH(bool, committing);
        RenderEngine engine;
        auto *tab = new TabPage(&engine);
        const QPointer<TabPage> page(tab);
        QVERIFY(tab->open(QStringLiteral(MERVIN_FIXTURE_PDF)));
        QVERIFY(edit(*tab));
        if (committing)
            job::testing::holdCommitWhen([](const QString &path) {
                return path.endsWith(QStringLiteral(".json"));
            });
        else
            job::testing::holdWhen([](job::Step step, const QString &path) {
                return step == job::Step::Writing && QFileInfo(path).fileName().startsWith(QStringLiteral(".unload-"));
            });
        const auto release = qScopeGuard([] { job::testing::release(); });
        QVERIFY(tab->suspend());
        QTRY_COMPARE(job::testing::held(), 1);
        QElapsedTimer elapsed;
        elapsed.start();
        delete tab;
        QVERIFY(!page);
        QVERIFY(elapsed.elapsed() < 1000);
        job::testing::release();
        engine.shutdown();
        QVERIFY(background::settle(10000));
        QVERIFY(Document::waitForCloses(10000));
        QVERIFY(appfiles::flush(10000));
        QVERIFY(recoveryEntries().isEmpty());
    }

    /// Covers successful and failed removal of a superseded recovery PDF.
    void supersededRecoveryKeepsItsManifestUntilPdfRemoval_data()
    {
        QTest::addColumn<bool>("removalFails");
        QTest::newRow("removed PDF releases its manifest") << false;
        QTest::newRow("failed PDF removal retains its manifest") << true;
    }

    /// Retains the old cleanup record on removal failure without losing the new checkpoint.
    void supersededRecoveryKeepsItsManifestUntilPdfRemoval()
    {
        QFETCH(bool, removalFails);
        QTemporaryDir files;
        const QString path = copy(files, QStringLiteral("edited.pdf"));
        QVERIFY(!path.isEmpty());
        const QByteArray original = bytes(path);
        RenderEngine engine;
        TabPage tab(&engine);
        QVERIFY(tab.open(path));
        QVERIFY(edit(tab, QStringLiteral("First retained edit")));
        QVERIFY(testing::suspendAndWait(tab));
        const QString priorCopy = tab.recoveryPath();
        const QString priorManifest = priorCopy + QStringLiteral(".json");
        const QByteArray priorMetadata = bytes(priorManifest);
        QVERIFY(!priorMetadata.isEmpty());
        QVERIFY(tab.resume());
        QVERIFY(edit(tab, QStringLiteral("Second retained edit")));
        if (removalFails) {
            // The open PDF owns its bytes. A nonempty directory makes removal
            // fail on every platform without relying on filesystem permissions.
            QVERIFY(QFile::remove(priorCopy));
            QVERIFY(QDir().mkpath(priorCopy));
            QFile blocker(QDir(priorCopy).filePath(QStringLiteral("keep")));
            QVERIFY(blocker.open(QIODevice::WriteOnly));
            QCOMPARE(blocker.write("occupied"), qint64(8));
            blocker.close();
        }
        QVERIFY(testing::suspendAndWait(tab));
        const QString currentCopy = tab.recoveryPath();
        QVERIFY(currentCopy != priorCopy);
        QVERIFY(tab.hasRecoverySnapshot() && tab.hasUnsavedEdits());
        QVERIFY(!bytes(currentCopy).isEmpty());
        QVERIFY(!bytes(currentCopy + QStringLiteral(".json")).isEmpty());
        engine.shutdown();
        QVERIFY(background::settle(10000));
        QVERIFY(Document::waitForCloses(10000));
        QVERIFY(appfiles::flush(10000));
        if (removalFails) {
            QVERIFY(QFileInfo(priorCopy).isDir());
            QCOMPARE(bytes(priorManifest), priorMetadata);
        } else {
            QVERIFY(!QFileInfo::exists(priorCopy));
            QVERIFY(!QFileInfo::exists(priorManifest));
        }
        QCOMPARE(bytes(path), original);
        QVERIFY(tab.resume());
        QCOMPARE(tab.viewer()->annotModel()->pageAnnots(0).size(), size_t(2));
    }

    /// Preserves adopted edits when viewer signals synchronously request Stop.
    void stopDuringViewerDetachmentKeepsTheCommittedRecovery()
    {
        RenderEngine engine;
        TabPage tab(&engine);
        QVERIFY(tab.open(QStringLiteral(MERVIN_FIXTURE_PDF)));
        QVERIFY(edit(tab));
        bool stopped = false;
        connect(tab.viewer(), &ViewerWidget::formModeChanged, &tab, [&](bool) {
            if (tab.isBusy() && tab.hasRecoverySnapshot() && !stopped) {
                stopped = true;
                tab.stopWaiting();
            }
        });
        QVERIFY(tab.suspend());
        QTRY_VERIFY_WITH_TIMEOUT(stopped && tab.isUsable(), 10000);
        QVERIFY(tab.hasRecoverySnapshot());
        QVERIFY(tab.hasUnsavedEdits());
        QCOMPARE(tab.viewer()->annotModel()->pageAnnots(0).size(), size_t(1));
        QCOMPARE(tab.viewer()->annotModel()->pageAnnots(0).front().contents,
                 QStringLiteral("Retained edit"));
        QVERIFY(QFileInfo::exists(tab.recoveryPath()));
        QVERIFY(QFileInfo::exists(tab.recoveryPath() + QStringLiteral(".json")));
    }

    /// Transfers cleanup ownership before adoption signals can delete the tab.
    void deletingDuringAdoptionClosesTheDocumentAndRecovery()
    {
        RenderEngine engine;
        auto *tab = new TabPage(&engine);
        const QPointer<TabPage> page(tab);
        QVERIFY(tab->open(QStringLiteral(MERVIN_FIXTURE_PDF)));
        QVERIFY(edit(*tab));
        const auto lifetime = tab->viewer()->document()->lifetime();
        connect(tab, &TabPage::stateChanged, &engine, [page] {
            if (page && !page->isBusy() && page->hasRecoverySnapshot())
                delete page.data();
        });
        QVERIFY(tab->suspend());
        QTRY_VERIFY_WITH_TIMEOUT(!page, 10000);
        engine.shutdown();
        QVERIFY(background::settle(10000));
        QVERIFY(Document::waitForCloses(10000));
        QVERIFY(appfiles::flush(10000));
        {
            std::lock_guard<std::timed_mutex> lock(lifetime->mutex);
            QVERIFY(!lifetime->document);
        }
        QVERIFY(recoveryEntries().isEmpty());
    }

    /// Covers Save and discard decisions after closing a held housekeeping snapshot.
    void closeStopsHousekeepingSilently_data()
    {
        QTest::addColumn<bool>("save");
        QTest::newRow("Don't save during snapshot") << false;
        QTest::newRow("Save after document release") << true;
    }

    /// Separates internal unload abandonment from the user pressing Stop waiting.
    void closeStopsHousekeepingSilently()
    {
        QFETCH(bool, save);
        QTemporaryDir files;
        const QString path = copy(files, QStringLiteral("edited.pdf"));
        QVERIFY(!path.isEmpty());
        const QByteArray original = bytes(path);
        WindowManager manager;
        MainWindow *window = manager.createWindow();
        QVERIFY(testing::openAndWait(*window, path));
        const QPointer<TabPage> page = window->findChild<TabPage *>();
        QVERIFY(page && edit(*page));
        job::testing::holdWhen([](job::Step step, const QString &where) {
            return step == job::Step::Writing && QFileInfo(where).fileName().startsWith(QStringLiteral(".unload-"));
        });
        const auto release = qScopeGuard([] { job::testing::release(); });
        QVERIFY(page->suspend());
        QTRY_COMPARE(job::testing::held(), 1);
        QSignalSpy userStopped(page, &TabPage::waitingStopped);
        int questions = 0;
        QTimer dialogs;
        connect(&dialogs, &QTimer::timeout, this, [&] {
            auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
            if (!box)
                return;
            ++questions;
            box->done(save ? QMessageBox::Save : QMessageBox::Discard);
        });
        dialogs.start(5);
        QVERIFY(QMetaObject::invokeMethod(window, "closeTab", Q_ARG(int, 0)));
        QTRY_COMPARE_WITH_TIMEOUT(questions, 1, 10000);
        QCOMPARE(userStopped.size(), 0);
        if (save)
            QVERIFY(page && page->isBusy());
        else
            QTRY_VERIFY_WITH_TIMEOUT(!page, 10000);
        job::testing::release();
        QTRY_VERIFY_WITH_TIMEOUT(!page, 10000);
        manager.engine()->shutdown();
        QVERIFY(background::settle(10000));
        QVERIFY(appfiles::flush(10000));
        QVERIFY(recoveryEntries().isEmpty());
        if (save) {
            QVERIFY(bytes(path) != original);
            RenderEngine check;
            auto document = check.openDocument(path);
            QVERIFY(document);
            QCOMPARE(AnnotModel(*document).pageAnnots(0).size(), size_t(1));
            QCOMPARE(AnnotModel(*document).pageAnnots(0).front().contents,
                     QStringLiteral("Retained edit"));
        } else {
            QCOMPARE(bytes(path), original);
        }
    }

    /// Delivers a failed checkpoint through the surviving owner after a tab move.
    void movedTabReportsProfileFailureInItsCurrentOwner()
    {
        QTemporaryDir files;
        const QString path = copy(files, QStringLiteral("edited.pdf"));
        QVERIFY(!path.isEmpty());
        WindowManager manager;
        const QPointer<MainWindow> first = manager.createWindow();
        MainWindow *second = manager.createWindow();
        first->hide();
        second->hide();
        QVERIFY(testing::openAndWait(*first, path));
        const QPointer<TabPage> page = first->findChild<TabPage *>();
        QVERIFY(page && edit(*page));
        const QString blocker = profile_->filePath(QStringLiteral("not-a-folder"));
        QFile file(blocker);
        QVERIFY(file.open(QIODevice::WriteOnly));
        QVERIFY(file.write("Not a directory") > 0);
        file.close();
        ConfigPaths::setOverrideDir(blocker);
        const QString directory = QDir(blocker).filePath(QStringLiteral("recovery"));
        job::testing::holdWhen([directory](job::Step step, const QString &where) {
            return step == job::Step::Writing && where == directory;
        });
        const auto restore = qScopeGuard([&] {
            job::testing::release();
            ConfigPaths::setOverrideDir(profile_->path());
        });
        first->updateDocumentActivity(0, 1);
        QVERIFY(first->updateDocumentActivity(61000, 1));
        QTRY_COMPARE(job::testing::held(), 1);
        ConfigPaths::setOverrideDir(profile_->path());
        second->adoptTab(first->releaseTab(0));
        delete first.data();
        QVERIFY(!first);
        job::testing::release();
        QTRY_VERIFY_WITH_TIMEOUT(page->isUsable(), 10000);
        QVERIFY(page->hasUnsavedEdits());
        QTRY_VERIFY(second->statusBar()->currentMessage().startsWith(
            QStringLiteral("edited.pdf remains loaded.")));
        QCOMPARE(page->viewer()->annotModel()->pageAnnots(0).size(), size_t(1));
        QCOMPARE(page->viewer()->annotModel()->pageAnnots(0).front().contents,
                 QStringLiteral("Retained edit"));
    }

    /// Once every document has unloaded, trims the entries in MuPDF's shared store that
    /// nothing else holds and keeps the held ones. Unpatched MuPDF reads a store entry it
    /// has found after it releases the store lock, so a trim that freed held entries gave
    /// a render worker freed memory, and the process crashed or corrupted its heap.
    void unloadingEveryDocumentTrimsOnlyUnheldStoreEntries()
    {
        QTemporaryDir files;
        const QString path = copy(files, QStringLiteral("unloaded.pdf"));
        QVERIFY(!path.isEmpty());
        WindowManager manager;
        MainWindow *window = manager.createWindow();
        QVERIFY(testing::openAndWait(*window, path));
        auto *tab = window->findChild<TabPage *>();
        QVERIFY(tab);
        fz_context *base = manager.engine()->baseContext().get();
        static StoreProbe held;
        static StoreProbe unheld;
        storeProbe(base, held, true);
        storeProbe(base, unheld, false);
        const auto release = qScopeGuard([base] { fz_drop_storable(base, &held.storable); });
        manager.checkDocumentActivity();
        QVERIFY(storeHolds(base, unheld));

        window->hide();
        QVERIFY(testing::suspendAndWait(*tab));
        QVERIFY(!window->hasResidentDocuments());
        manager.checkDocumentActivity();
        QVERIFY(storeHolds(base, held));
        QVERIFY(!storeHolds(base, unheld));
    }

    /// A completed close can reclaim images while another document stays open.
    /// Queue/worker retirement prevents late rasters from refilling the just-trimmed store.
    void completedCloseTrimWaitsForWorkersAndCoalesces()
    {
        RenderEngine engine;
        auto scan = engine.openDocument(QStringLiteral(MERVIN_SCAN_PDF));
        auto survivor = engine.openDocument(QStringLiteral(MERVIN_FIXTURE_PDF));
        QVERIFY(scan && survivor);
        const QImage decoded = engine.renderPageImage(scan.get(), 0, 1, 0);
        QVERIFY(!decoded.isNull());
        fz_context *base = engine.baseContext().get();
        QCOMPARE(storeLists(base, "(image "), std::optional<bool>(true));
        const QImage expected = engine.renderPageImage(survivor.get(), 0, 1, 0);
        QVERIFY(!expected.isNull());
        const quint64 before = Document::completedCloses();
        {
            testing::Stall closeStall(scan->lifetime()->mutex);
            Document::closeInBackground(std::move(scan));
            QCOMPARE(Document::completedCloses(), before);
        }
        QTRY_COMPARE_WITH_TIMEOUT(Document::completedCloses(), before + 1, 10000);
        QCOMPARE(storeLists(base, "(image "), std::optional<bool>(true));

        QSignalSpy results(&engine, &RenderEngine::resultReady);
        {
            testing::Stall renderStall(survivor->lifetime()->mutex);
            RenderRequest request;
            request.document = survivor.get();
            request.requester = 1;
            engine.submit(request);
            QTRY_COMPARE_WITH_TIMEOUT(RenderEngine::testing::activeRequests(engine, 1), 1, 5000);
            engine.scheduleCacheTrim();
            engine.scheduleCacheTrim();
            QCoreApplication::processEvents();
            QCOMPARE(RenderEngine::testing::cacheTrims(engine), 0);
            QCOMPARE(storeLists(base, "(image "), std::optional<bool>(true));
        }
        QTRY_COMPARE_WITH_TIMEOUT(results.size(), 1, 10000);
        QTRY_COMPARE_WITH_TIMEOUT(RenderEngine::testing::cacheTrims(engine), 1, 10000);
        QCOMPARE(storeLists(base, "(image "), std::optional<bool>(false));
        QVERIFY(results.front().front().value<RenderResult>().ok);
        QCOMPARE(results.front().front().value<RenderResult>().image, expected);
        // Warm resources of the open document can be recreated without changing pixels.
        QCOMPARE(engine.renderPageImage(survivor.get(), 0, 1, 0), expected);
        QCoreApplication::processEvents();
        QCOMPARE(RenderEngine::testing::cacheTrims(engine), 1);
    }

    /// Shrinking the resource store alone does not purge MuPDF's separate glyph cache.
    void cacheTrimPurgesPopulatedGlyphs()
    {
        RenderEngine engine;
        auto document = engine.openDocument(QStringLiteral(MERVIN_FIXTURE_PDF));
        QVERIFY(document);
        fz_context *base = engine.baseContext().get();
        fz_page *page = nullptr;
        fz_display_list *list = nullptr;
        fz_pixmap *pixmap = nullptr;
        fz_var(page);
        fz_var(list);
        fz_var(pixmap);
        const auto cleanup = qScopeGuard([&] {
            fz_drop_pixmap(base, pixmap);
            fz_drop_display_list(base, list);
        });
        fz_try(base) {
            page = fz_load_page(base, document->handle(), 0);
            list = fz_new_display_list_from_page(base, page);
        }
        fz_always(base) {
            fz_drop_page(base, page);
        }
        fz_catch(base) {}
        QVERIFY(list);
        // MuPDF purges glyphs during PDF close. A worker's independent display list
        // can rasterize afterwards and repopulate them. Reproduce that exact order.
        document.reset();
        fz_try(base) {
            pixmap = fz_new_pixmap_from_display_list(base, list, fz_identity, fz_device_rgb(base), 0);
        }
        fz_catch(base) {}
        QVERIFY(pixmap);
        fz_drop_pixmap(base, pixmap);
        pixmap = nullptr;
        fz_drop_display_list(base, list);
        list = nullptr;
        const auto populated = glyphBytes(base);
        QVERIFY(populated.has_value());
        QVERIFY(*populated > 0);
        // The negative control pins why the glyph purge is needed in addition to shrink.
        fz_shrink_store(base, 0);
        QCOMPARE(glyphBytes(base), populated);
        engine.trimCache();
        QCOMPARE(glyphBytes(base), std::optional<quint64>(0));
    }

    /// Frees decoded images from the shared store after every document unloads.
    /// Closing a PDF alone does not release these cached page images.
    void unloadingEveryDocumentFreesItsDecodedImages()
    {
        QTemporaryDir files;
        const QString path =
            copy(files, QStringLiteral("scan.pdf"), QStringLiteral(MERVIN_SCAN_PDF));
        QVERIFY(!path.isEmpty());
        WindowManager manager;
        QSignalSpy rendered(manager.engine(), &RenderEngine::resultReady);
        MainWindow *window = manager.createWindow();
        QVERIFY(testing::openAndWait(*window, path));
        auto *tab = window->findChild<TabPage *>();
        QVERIFY(tab);
        fz_context *base = manager.engine()->baseContext().get();
        // Unload only after the render has finished. MuPDF frees the image itself when a
        // display list outlives the close, which would hide a missing trim, and the
        // worker drops its list before it delivers the result.
        QTRY_VERIFY(!rendered.isEmpty());
        const std::optional<bool> decoded = storeLists(base, "(image ");
        QVERIFY2(decoded.has_value(), "MuPDF could not dump its store");
        QVERIFY(*decoded);

        window->hide();
        QVERIFY(testing::suspendAndWait(*tab));
        QVERIFY(!window->hasResidentDocuments());
        QTRY_COMPARE(Document::closing(), 0);
        // The fixture's one render was delivered before the unload, so no worker holds a
        // display list, and one check after the close frees the images.
        manager.checkDocumentActivity();
        const std::optional<bool> unloaded = storeLists(base, "(image ");
        QVERIFY2(unloaded.has_value(), "MuPDF could not dump its store");
        QVERIFY(!*unloaded);
    }
};

QTEST_MAIN(TstBackgroundUnload)
#include "tst_background_unload.moc"
