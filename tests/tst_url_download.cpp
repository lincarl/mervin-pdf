#include "io/Background.h"
#include "net/UrlDownload.h"
#include "net/UrlOpen.h"

#include <QDir>
#include <QFile>
#include <QSaveFile>
#include <QScopeGuard>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#include <QtTest>

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>

using mervin::urlopen::UrlDownload;

namespace {

/// Serves real HTTP responses, including redirects, failures and stalled replies.
class HttpServer final : public QTcpServer
{
public:
    explicit HttpServer(QByteArray body) : body_(std::move(body))
    {
        connect(this, &QTcpServer::newConnection, this, [this] {
            while (hasPendingConnections()) {
                QTcpSocket *socket = nextPendingConnection();
                const auto input = std::make_shared<QByteArray>();
                connect(socket, &QTcpSocket::readyRead, this, [this, socket, input] {
                    input->append(socket->readAll());
                    if (!input->contains("\r\n\r\n"))
                        return;
                    socket->disconnect(this);
                    requests.append(*input);
                    if (hold)
                        return;
                    if (input->startsWith("GET /redirect ")) {
                        socket->write("HTTP/1.1 302 Found\r\nLocation: /document\r\n"
                                      "Content-Length: 0\r\nConnection: close\r\n\r\n");
                    } else {
                        const QByteArray status = fail ? "500 Internal Server Error" : "200 OK";
                        const qint64 length = body_.size() + (truncate ? 100 : 0);
                        socket->write("HTTP/1.1 " + status + "\r\nContent-Length: "
                                      + QByteArray::number(length)
                                      + "\r\nConnection: close\r\n\r\n" + body_);
                    }
                    socket->disconnectFromHost();
                });
            }
        });
        listen(QHostAddress::LocalHost);
    }

    /// Returns this server's ephemeral local address for the requested route.
    QUrl url(const QString &path = QStringLiteral("/document")) const
    {
        return QUrl(QStringLiteral("http://127.0.0.1:%1%2").arg(serverPort()).arg(path));
    }

    QList<QByteArray> requests;
    bool fail = false;
    bool truncate = false;
    bool hold = false;

private:
    QByteArray body_;
};

/// Holds a worker until released while leaving the test event loop available.
class Gate
{
public:
    void wait()
    {
        std::unique_lock lock(mutex_);
        reached_.store(true);
        changed_.wait(lock, [this] { return released_; });
    }

    void release()
    {
        std::lock_guard lock(mutex_);
        released_ = true;
        changed_.notify_all();
    }

    bool reached() const { return reached_.load(); }

private:
    std::atomic<bool> reached_{false};
    std::mutex mutex_;
    std::condition_variable changed_;
    bool released_ = false;
};

/// Writes an existing destination for replacement and preservation checks.
bool writeFile(const QString &path, const QByteArray &data)
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(data) == data.size();
}

/// Reads the committed destination after a worker has finished.
QByteArray readFile(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    return file.readAll();
}

} // namespace

class TstUrlDownload : public QObject
{
    Q_OBJECT

private slots:
    /// Leaves no worker from one case running during the next case.
    void cleanup()
    {
        QTRY_COMPARE_WITH_TIMEOUT(mervin::background::running(), 0, 10000);
    }

    /// Streams a multi-buffer HTTP response and its short tail through an atomic commit.
    void downloadsOnceWithRedirectAndAtomicReplacement()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString destination = directory.filePath(QStringLiteral("document.pdf"));
        const QByteArray old = "old document";
        QVERIFY(writeFile(destination, old));
        QByteArray body(3 * 64 * 1024 + 13, '\0');
        for (qsizetype i = 0; i < body.size(); ++i)
            body[i] = char(i % 251);
        HttpServer server(body);
        QVERIFY(server.isListening());
        UrlDownload download;
        const auto gate = std::make_shared<Gate>();
        const auto release = qScopeGuard([gate, &download] {
            download.cancel();
            gate->release();
            mervin::background::settle(10000);
        });
        const auto workerOnly = std::make_shared<std::atomic<bool>>(true);
        const auto writes = std::make_shared<std::atomic<int>>(0);
        QThread *const uiThread = QThread::currentThread();
        download.setIoHookForTesting([gate, workerOnly, writes, uiThread, size = body.size()]
                                    (QSaveFile &output, UrlDownload::IoPhase phase) {
            if (QThread::currentThread() == uiThread)
                workerOnly->store(false);
            if (phase == UrlDownload::IoPhase::BeforeWrite)
                ++*writes;
            if (phase == UrlDownload::IoPhase::BeforeCommit) {
                if (output.pos() != size)
                    workerOnly->store(false);
                gate->wait();
            }
        });
        bool uiSignals = true;
        connect(&download, &UrlDownload::downloadProgress, this,
                [&](qint64, qint64) { uiSignals &= QThread::currentThread() == uiThread; });
        QSignalSpy finished(&download, &UrlDownload::finished);
        QSignalSpy progress(&download, &UrlDownload::downloadProgress);
        const auto request = mervin::urlopen::makeRequest(server.url(QStringLiteral("/redirect")));
        download.start(request, destination);
        download.start(request, destination);
        QTRY_VERIFY_WITH_TIMEOUT(gate->reached(), 10000);
        QCOMPARE(readFile(destination), old);
        QVERIFY(finished.isEmpty());
        gate->release();
        QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 10000);
        QCOMPARE(finished.first().first().value<UrlDownload::Error>(), UrlDownload::Error::None);
        QCOMPARE(readFile(destination), body);
        QCOMPARE(server.requests.size(), 2);
        QVERIFY(server.requests.first().contains("Sec-Fetch-Dest: document"));
        QVERIFY(workerOnly->load());
        QVERIFY(uiSignals);
        QVERIFY(!progress.isEmpty());
        QVERIFY(writes->load() >= 4);
        QCOMPARE(QDir(directory.path()).entryList(QDir::Files | QDir::Hidden),
                 QStringList{QStringLiteral("document.pdf")});
    }

    /// Distinguishes setup, stream and commit errors without replacing existing bytes.
    void storageFailure_data()
    {
        QTest::addColumn<QString>("failure");
        QTest::newRow("folder") << QStringLiteral("folder");
        QTest::newRow("open") << QStringLiteral("open");
        QTest::newRow("write") << QStringLiteral("write");
        QTest::newRow("commit") << QStringLiteral("commit");
    }

    void storageFailure()
    {
        QFETCH(QString, failure);
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString existing = directory.filePath(QStringLiteral("existing.pdf"));
        QVERIFY(writeFile(existing, "old document"));
        HttpServer server("replacement");
        QVERIFY(server.isListening());
        UrlDownload download;
        if (failure == QStringLiteral("write") || failure == QStringLiteral("commit")) {
            download.setIoHookForTesting([failure](QSaveFile &file, UrlDownload::IoPhase phase) {
                if ((failure == QStringLiteral("write") && phase == UrlDownload::IoPhase::BeforeWrite)
                    || (failure == QStringLiteral("commit") && phase == UrlDownload::IoPhase::BeforeCommit))
                    file.cancelWriting();
            });
        }
        const QString destination = failure == QStringLiteral("folder")
            ? existing + QStringLiteral("/child.pdf")
            : failure == QStringLiteral("open") ? directory.path() : existing;
        QSignalSpy finished(&download, &UrlDownload::finished);
        download.start(mervin::urlopen::makeRequest(server.url()), destination);
        QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 10000);
        const auto expected = failure == QStringLiteral("folder")
            ? UrlDownload::Error::Directory : UrlDownload::Error::File;
        QCOMPARE(finished.first().first().value<UrlDownload::Error>(), expected);
        if (expected == UrlDownload::Error::File)
            QVERIFY(!finished.first().at(1).toString().isEmpty());
        QCOMPARE(readFile(existing), QByteArray("old document"));
        QCOMPARE(server.requests.size(), failure == QStringLiteral("folder")
                     || failure == QStringLiteral("open") ? 0 : 1);
        QCOMPARE(QDir(directory.path()).entryList(QDir::Files | QDir::Hidden),
                 QStringList{QStringLiteral("existing.pdf")});
    }

    /// A failed HTTP status or truncated transfer must not install its response body.
    void networkFailure_data()
    {
        QTest::addColumn<bool>("truncated");
        QTest::newRow("http-error") << false;
        QTest::newRow("truncated-body") << true;
    }

    void networkFailure()
    {
        QFETCH(bool, truncated);
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString destination = directory.filePath(QStringLiteral("document.pdf"));
        QVERIFY(writeFile(destination, "old document"));
        HttpServer server("incomplete document");
        QVERIFY(server.isListening());
        server.fail = !truncated;
        server.truncate = truncated;
        UrlDownload download;
        QSignalSpy finished(&download, &UrlDownload::finished);
        download.start(mervin::urlopen::makeRequest(server.url()), destination);
        QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 10000);
        QCOMPARE(finished.first().first().value<UrlDownload::Error>(), UrlDownload::Error::Network);
        QVERIFY(!finished.first().at(1).toString().isEmpty());
        QCOMPARE(readFile(destination), QByteArray("old document"));
        QCOMPARE(QDir(directory.path()).entryList(QDir::Files | QDir::Hidden),
                 QStringList{QStringLiteral("document.pdf")});
    }

    /// Covers abandonment at every filesystem boundary, including window destruction.
    void stalledStorageDoesNotBlockUiOrCancellation_data()
    {
        QTest::addColumn<UrlDownload::IoPhase>("phase");
        QTest::addColumn<bool>("destroy");
        for (const bool destroy : {false, true}) {
            const QByteArray mode = destroy ? "destroy-" : "cancel-";
            QTest::newRow((mode + "folder").constData()) << UrlDownload::IoPhase::BeforeDirectory << destroy;
            QTest::newRow((mode + "open").constData()) << UrlDownload::IoPhase::BeforeOpen << destroy;
            QTest::newRow((mode + "write").constData()) << UrlDownload::IoPhase::BeforeWrite << destroy;
            QTest::newRow((mode + "commit").constData()) << UrlDownload::IoPhase::BeforeCommit << destroy;
        }
    }

    void stalledStorageDoesNotBlockUiOrCancellation()
    {
        QFETCH(UrlDownload::IoPhase, phase);
        QFETCH(bool, destroy);
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString destination = directory.filePath(QStringLiteral("document.pdf"));
        QVERIFY(writeFile(destination, "old document"));
        HttpServer server(QByteArray(4 * 64 * 1024, 'x'));
        QVERIFY(server.isListening());
        auto download = std::make_unique<UrlDownload>();
        const auto gate = std::make_shared<Gate>();
        const auto release = qScopeGuard([gate, &download] {
            if (download)
                download->cancel();
            gate->release();
            mervin::background::settle(10000);
        });
        download->setIoHookForTesting([gate, phase](QSaveFile &, UrlDownload::IoPhase current) {
            if (phase == current)
                gate->wait();
        });
        QSignalSpy finished(download.get(), &UrlDownload::finished);
        download->start(mervin::urlopen::makeRequest(server.url()), destination);
        QTRY_VERIFY_WITH_TIMEOUT(gate->reached(), 10000);
        bool heartbeat = false;
        QTimer::singleShot(0, this, [&] { heartbeat = true; });
        QTRY_VERIFY(heartbeat);
        if (destroy) {
            download.reset();
            QVERIFY(finished.isEmpty());
        } else {
            download->cancel();
            download->cancel();
            QCOMPARE(finished.size(), 1);
            QCOMPARE(finished.first().first().value<UrlDownload::Error>(), UrlDownload::Error::Canceled);
        }
        QVERIFY(mervin::background::running() > 0);
        QCOMPARE(readFile(destination), QByteArray("old document"));
        gate->release();
        QTRY_COMPARE_WITH_TIMEOUT(mervin::background::running(), 0, 10000);
        QCoreApplication::processEvents();
        QCOMPARE(finished.size(), destroy ? 0 : 1);
        QCOMPARE(readFile(destination), QByteArray("old document"));
        QCOMPARE(QDir(directory.path()).entryList(QDir::Files | QDir::Hidden),
                 QStringList{QStringLiteral("document.pdf")});
    }

    /// Cancellation also wakes a worker whose server has stopped sending data.
    void cancelsAnIdleNetworkReply()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString destination = directory.filePath(QStringLiteral("nested/document.pdf"));
        HttpServer server({});
        QVERIFY(server.isListening());
        server.hold = true;
        UrlDownload download;
        QSignalSpy finished(&download, &UrlDownload::finished);
        download.start(mervin::urlopen::makeRequest(server.url()), destination);
        QTRY_COMPARE_WITH_TIMEOUT(server.requests.size(), 1, 10000);
        download.cancel();
        QCOMPARE(finished.size(), 1);
        QCOMPARE(finished.first().first().value<UrlDownload::Error>(), UrlDownload::Error::Canceled);
        QTRY_COMPARE_WITH_TIMEOUT(mervin::background::running(), 0, 10000);
        QVERIFY(!QFile::exists(destination));
        QVERIFY(QDir(directory.filePath(QStringLiteral("nested"))).entryList(
                    QDir::Files | QDir::Hidden).isEmpty());
    }
};

QTEST_GUILESS_MAIN(TstUrlDownload)
#include "tst_url_download.moc"
