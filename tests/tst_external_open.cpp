#include "platform/ExternalOpen.h"

#include <QCoreApplication>
#include <QDesktopServices>
#include <QScopeGuard>
#include <QSemaphore>
#include <QTest>
#include <QThread>

#include <atomic>
#include <memory>

class TestExternalOpen : public QObject
{
    Q_OBJECT

private slots:
    void slowHandlerKeepsUiResponsive();
};

void TestExternalOpen::slowHandlerKeepsUiResponsive()
{
    struct State
    {
        QSemaphore release;
        std::atomic<int> started{0};
        std::atomic<int> finished{0};
        std::atomic<bool> onUiThread{false};
        std::atomic<bool> changedUrl{false};
    };
    const auto state = std::make_shared<State>();
    const QUrl url(QStringLiteral("https://example.invalid/a%20b?value=%26#section"));
    const auto release = qScopeGuard([state] { state->release.release(2); });
    auto owner = std::make_unique<QObject>();
    mervin::externalopen::install(owner.get(), [state, url](const QUrl &received) {
        if (QThread::currentThread() == QCoreApplication::instance()->thread())
            state->onUiThread = true;
        if (received != url)
            state->changedUrl = true;
        ++state->started;
        state->release.tryAcquire(1, 5000);
        ++state->finished;
    });
    QVERIFY(QDesktopServices::openUrl(url));
    QVERIFY(QDesktopServices::openUrl(url));
    QTRY_COMPARE(state->started.load(), 2);
    QCOMPARE(state->finished.load(), 0);
    QVERIFY(!state->onUiThread.load());
    QVERIFY(!state->changedUrl.load());
    // Closing the owner must not wait for the system handler or leave the
    // workers referring to a destroyed QObject.
    owner.reset();
    QCOMPARE(state->finished.load(), 0);
    state->release.release(2);
    QTRY_COMPARE(state->finished.load(), 2);
}

QTEST_MAIN(TestExternalOpen)
#include "tst_external_open.moc"
