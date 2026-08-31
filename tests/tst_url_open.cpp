#include "net/UrlOpen.h"
#include "net/UrlDownloadLocation.h"

#include <QEventLoop>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QRegularExpression>
#include <QTest>
#include <QTemporaryDir>
#include <QTimer>

using namespace mervin;

class TstUrlOpen : public QObject
{
    Q_OBJECT

private slots:
    void parsesInternetUrl_data();
    void parsesInternetUrl();
    void rejectsNonInternetInput_data();
    void rejectsNonInternetInput();
    void buildsCompatibleRequest();
    void makesSafeCacheName();
    void choosesDownloadDirectory();
    void downloadsConfiguredUrl();
};

void TstUrlOpen::parsesInternetUrl_data()
{
    QTest::addColumn<QString>("input");
    QTest::addColumn<QString>("expected");

    const QString expected = QStringLiteral(
        "https://www.onsemi.com/pdf/datasheet/esd5z2.5t1-d.pdf");
    QTest::newRow("ordinary URL") << expected << expected;
    QTest::newRow("Windows picker separators")
        << QStringLiteral("https:\\www.onsemi.com\\pdf\\datasheet\\esd5z2.5t1-d.pdf")
        << expected;
    QTest::newRow("mixed separators")
        << QStringLiteral("https:/\\www.onsemi.com/pdf\\datasheet/esd5z2.5t1-d.pdf")
        << expected;
    QTest::newRow("trimmed and case-insensitive")
        << QStringLiteral("  HTTPS://www.onsemi.com/pdf/datasheet/esd5z2.5t1-d.pdf  ")
        << expected;
}

void TstUrlOpen::parsesInternetUrl()
{
    QFETCH(QString, input);
    QFETCH(QString, expected);
    const auto parsed = urlopen::fromUserInput(input);
    QVERIFY(parsed.has_value());
    QCOMPARE(parsed->toString(), expected);
}

void TstUrlOpen::rejectsNonInternetInput_data()
{
    QTest::addColumn<QString>("input");
    QTest::newRow("empty") << QString();
    QTest::newRow("local Windows path") << QStringLiteral("C:\\Documents\\drawing.pdf");
    QTest::newRow("local Unix path") << QStringLiteral("/tmp/drawing.pdf");
    QTest::newRow("unsupported scheme") << QStringLiteral("ftp://example.com/drawing.pdf");
    QTest::newRow("missing host") << QStringLiteral("https://");
}

void TstUrlOpen::rejectsNonInternetInput()
{
    QFETCH(QString, input);
    QVERIFY(!urlopen::fromUserInput(input).has_value());
}

void TstUrlOpen::buildsCompatibleRequest()
{
    const QUrl url(QStringLiteral("https://www.onsemi.com/example.pdf"));
    const QNetworkRequest request = urlopen::makeRequest(url);
    QCOMPARE(request.url(), url);
    QVERIFY(request.rawHeader("User-Agent").contains("HeadlessChrome/151"));
    QCOMPARE(request.rawHeader("Accept-Language"), QByteArray("en-US,en;q=0.9"));
    QCOMPARE(request.rawHeader("Upgrade-Insecure-Requests"), QByteArray("1"));
    QVERIFY(request.rawHeader("sec-ch-ua").contains("Chromium"));
    QCOMPARE(request.rawHeader("sec-ch-ua-mobile"), QByteArray("?0"));
    QCOMPARE(request.rawHeader("sec-ch-ua-platform"), QByteArray("\"Linux\""));
    QVERIFY(request.rawHeader("Accept").contains("application/pdf"));
    QCOMPARE(request.attribute(QNetworkRequest::RedirectPolicyAttribute).toInt(),
             static_cast<int>(QNetworkRequest::NoLessSafeRedirectPolicy));
    QCOMPARE(request.transferTimeout(), 30000);

    const QUrl legacyAnalog(QStringLiteral(
        "http://www.analog.com/static/imported-files/data_sheets/AD5628_5648_5668.pdf"));
    QCOMPARE(urlopen::makeRequest(legacyAnalog).url(),
             QUrl(QStringLiteral("https://www.analog.com/media/en/technical-documentation/"
                                 "data-sheets/ad5628_5648_5668.pdf")));
}

void TstUrlOpen::makesSafeCacheName()
{
    const QUrl url(QStringLiteral("https://example.com/a%3Ab%3Fc.pdf?revision=2"));
    const QString name = urlopen::cachedFileName(url);
    QVERIFY(name.endsWith(QStringLiteral(".pdf")));
    QVERIFY(QRegularExpression(QStringLiteral("^[\\p{L}\\p{N}._-]+$"))
                .match(name).hasMatch());
    QVERIFY(!name.contains(u':'));
    QVERIFY(!name.contains(u'?'));
}

void TstUrlOpen::choosesDownloadDirectory()
{
    const QString originalOverride = ConfigPaths::overrideDir();
    ConfigPaths::setOverrideDir(QString());
    const QString standard = QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
    if (!standard.isEmpty())
        QCOMPARE(urlopen::downloadDirectory(), standard);

    QTemporaryDir profile;
    QVERIFY(profile.isValid());
    ConfigPaths::setOverrideDir(profile.path());
    QCOMPARE(urlopen::downloadDirectory(),
             QDir(profile.path()).filePath(QStringLiteral("downloads")));
    ConfigPaths::setOverrideDir(originalOverride);
}

void TstUrlOpen::downloadsConfiguredUrl()
{
    // Normal ctest runs stay hermetic. Set this explicitly for a pre-release
    // compatibility check against a document server that rejected Qt's empty
    // default User-Agent in v1.55.0.
    const QString input = qEnvironmentVariable("MERVIN_URL_OPEN_TEST_URL");
    if (input.isEmpty())
        return;

    const auto url = urlopen::fromUserInput(input);
    QVERIFY(url.has_value());
    QNetworkAccessManager network;
    QNetworkReply *reply = network.get(urlopen::makeRequest(*url));
    QEventLoop loop;
    QTimer timeout;
    timeout.setSingleShot(true);
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    QObject::connect(&timeout, &QTimer::timeout, reply, &QNetworkReply::abort);
    timeout.start(30000);
    loop.exec();

    QCOMPARE(reply->error(), QNetworkReply::NoError);
    QVERIFY(reply->readAll().startsWith("%PDF-"));
    reply->deleteLater();
}

QTEST_GUILESS_MAIN(TstUrlOpen)
#include "tst_url_open.moc"
