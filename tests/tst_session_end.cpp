#include "session/SessionEnd.h"

#include <QElapsedTimer>
#include <QScopeGuard>
#include <QTimer>
#include <QtTest>

#include <atomic>
#include <thread>

using namespace mervin::sessionend;

class TstSessionEnd : public QObject
{
    Q_OBJECT

private slots:
    void cleanup()
    {
        finishRequest(false);
    }

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
