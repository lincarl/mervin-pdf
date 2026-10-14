#include "session/SessionEnd.h"

#include <QElapsedTimer>
#include <QScopeGuard>
#include <QTimer>
#include <QtTest>

#include <atomic>
#include <thread>

using namespace mervin::sessionend;

// Checks admission, bounded waits and recovery disposal without file calls.
class TstSessionEnd : public QObject
{
    Q_OBJECT

private slots:
    // Resets the OS outcome gate between isolated controller cases.
    void cleanup()
    {
        finishRequest(false);
    }

    // Checks immediate readiness closes admission when no recovery is pending.
    void readyReturnsWithoutWaitingAndClosesAdmission()
    {
        QElapsedTimer time;
        time.start();
        QCOMPARE(waitForRecovery(), Result::Settled);
        QVERIFY(time.elapsed() < 100);
        QVERIFY(active());
        QVERIFY(!RecoveryWrite::begin());
        finishRequest(true);
        QVERIFY(!RecoveryWrite::begin());
        finishRequest(false);
        auto admitted = RecoveryWrite::begin();
        QVERIFY(admitted);
        QVERIFY(admitted->pending());
    }

    // Checks that worker completion releases grace without running queued GUI events.
    void workerCompletionReleasesGraceWithoutGuiEvents()
    {
        auto write = RecoveryWrite::begin();
        QVERIFY(write);
        bool guiDelivered = false;
        QTimer::singleShot(0, this, [&] { guiDelivered = true; });
        std::jthread worker([copy = *write] {
            QThread::msleep(70);
            copy.finish();
        });
        QElapsedTimer time;
        time.start();
        QCOMPARE(waitForRecovery(500), Result::Settled);
        QVERIFY(time.elapsed() >= 60);
        QVERIFY(time.elapsed() < 500);
        QVERIFY(!write->pending());
        QVERIFY(!guiDelivered);
        QCoreApplication::processEvents();
        QVERIFY(guiDelivered);
    }

    // Checks that unfinished work reaches its deadline and repeated requests reuse the answer.
    void unfinishedRecoveryTimesOutAndRepeatDoesNotWait()
    {
        auto write = RecoveryWrite::begin();
        QVERIFY(write);
        QElapsedTimer time;
        time.start();
        QCOMPARE(waitForRecovery(80), Result::TimedOut);
        QVERIFY(time.elapsed() >= 70);
        QVERIFY(time.elapsed() < 300);
        QVERIFY(write->pending());
        time.restart();
        QCOMPARE(waitForRecovery(), Result::TimedOut);
        QVERIFY(time.elapsed() < 100);
        QVERIFY(!RecoveryWrite::begin());
        write->finish();
        write->finish();
        QVERIFY(!write->pending());
    }

    // Checks that a worker reservation stays counted after its GUI copy is dropped.
    void retainedWorkerCountsAfterGuiCopyIsDestroyed()
    {
        auto guiWrite = RecoveryWrite::begin();
        QVERIFY(guiWrite);
        auto workerWrite = *guiWrite;
        guiWrite.reset();
        QCOMPARE(waitForRecovery(0), Result::TimedOut);
        finishRequest(false);
        workerWrite.finish();
        QCOMPARE(waitForRecovery(0), Result::Settled);
    }

    // Checks that destruction of the final shared reservation releases pending protection.
    void lastDestroyedCopyReleasesRecovery()
    {
        {
            auto write = RecoveryWrite::begin();
            QVERIFY(write);
            auto another = *write;
            write.reset();
            QVERIFY(another.pending());
        }
        QCOMPARE(waitForRecovery(0), Result::Settled);
    }

    // Checks that the configured grace clamps longer requests below four seconds.
    void maximumGraceIsBelowFourSeconds()
    {
        auto write = RecoveryWrite::begin();
        QVERIFY(write);
        QElapsedTimer time;
        time.start();
        QCOMPARE(waitForRecovery(10000), Result::TimedOut);
        QVERIFY(time.elapsed() >= kGraceMs - 20);
        QVERIFY(time.elapsed() < 4000);
        QVERIFY(write->pending());
    }

    // Races writer admission against observation to detect accepted work missing from grace.
    void admissionAndObservationCannotMissConcurrentWrites()
    {
        std::atomic<bool> go{false};
        std::optional<RecoveryWrite> write;
        std::jthread worker([&] {
            while (!go.load())
                std::this_thread::yield();
            write = RecoveryWrite::begin();
        });
        go = true;
        const auto answer = waitForRecovery(20);
        worker.join();
        if (write) {
            QCOMPARE(answer, Result::TimedOut);
            QVERIFY(write->pending());
        } else {
            QCOMPARE(answer, Result::Settled);
        }
    }

    // Checks that discard waits for the OS outcome and successful end denies removal.
    void committedRecoverySurvivesDiscardDuringSuccessfulSessionEnd()
    {
        auto write = RecoveryWrite::begin();
        QVERIFY(write);
        write->finish();
        QCOMPARE(waitForRecovery(0), Result::Settled);
        std::atomic<bool> discardStarted{false}, removed{false}, returned{false};
        std::jthread worker([copy = *write, &discardStarted, &removed, &returned] {
            discardStarted = true;
            if (copy.beginDiscard())
                removed = true;
            copy.finish();
            returned = true;
        });
        const auto release = qScopeGuard([] { finishRequest(true); });
        QTRY_VERIFY(discardStarted.load());
        QTest::qWait(30);
        QVERIFY(!returned.load());
        finishRequest(true);
        worker.join();
        QVERIFY(returned.load());
        QVERIFY(!removed.load());
        QVERIFY(!write->pending());
    }

    // Checks that cancellation rearms protection before a worker begins disposal.
    void canceledSessionRearmsProtectionBeforeDiscard()
    {
        auto write = RecoveryWrite::begin();
        QVERIFY(write);
        write->finish();
        QCOMPARE(waitForRecovery(0), Result::Settled);
        std::atomic<bool> admitted{false}, release{false};
        std::jthread worker([copy = *write, &admitted, &release] {
            if (copy.beginDiscard()) {
                admitted = true;
                while (!release.load())
                    std::this_thread::yield();
            }
            copy.finish();
        });
        const auto reset = qScopeGuard([&] {
            finishRequest(false);
            release = true;
        });
        finishRequest(false);
        QTRY_VERIFY(admitted.load());
        QVERIFY(write->pending());
        QCOMPARE(waitForRecovery(0), Result::TimedOut);
        finishRequest(false);
        release = true;
        worker.join();
        QVERIFY(!write->pending());
        QCOMPARE(waitForRecovery(0), Result::Settled);
    }

    // Checks that already admitted adopted recovery cleanup adds no snapshot grace.
    void adoptedCleanupDoesNotStartSnapshotGrace()
    {
        std::atomic<bool> admitted{false}, release{false};
        std::jthread worker([&] {
            if (beginRecoveryRemoval()) {
                admitted = true;
                while (!release.load())
                    std::this_thread::yield();
            }
        });
        const auto reset = qScopeGuard([&] {
            finishRequest(false);
            release = true;
        });
        QTRY_VERIFY(admitted.load());
        QCOMPARE(waitForRecovery(0), Result::Settled);
        release = true;
        worker.join();
    }
};

QTEST_GUILESS_MAIN(TstSessionEnd)
#include "tst_session_end.moc"
