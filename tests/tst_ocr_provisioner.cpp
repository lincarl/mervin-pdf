#include "i18n/UiLanguage.h"
#include "ocr/OcrLanguageMapping.h"
#include "ocr/OcrProvisioner.h"
#include "ocr/TessdataFile.h"

#include <QDir>
#include <QFile>
#include <QMap>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QPointer>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QtEndian>

#include <cstring>

namespace {

// A generated traineddata container with a valid unicharset. These tests check
// download and container handling; the real OCR engine has its own model test.
QByteArray modelData(char character = 'A')
{
    constexpr quint32 count = 22;
    QByteArray result(sizeof(quint32) + count * sizeof(qint64), '\0');
    qToLittleEndian<quint32>(count, result.data());
    for (quint32 slot = 0; slot < count; ++slot)
        qToLittleEndian<qint64>(slot == 21 ? result.size() : -1,
                               result.data() + sizeof(quint32) + slot * sizeof(qint64));
    result += QByteArrayLiteral("2\nNULL 0\n") + character + QByteArrayLiteral(" 1\n");
    return result;
}

bool writeFile(const QString &path, const QByteArray &data)
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(data) == data.size();
}

QByteArray readFile(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    return file.readAll();
}

QStringList files(const QString &directory)
{
    return QDir(directory).entryList(QDir::Files | QDir::Hidden);
}

class ModelReply final : public QNetworkReply
{
public:
    ModelReply(QObject *parent, const QNetworkRequest &request, int &aborts)
        : QNetworkReply(parent), aborts_(aborts)
    {
        setRequest(request);
        setUrl(request.url());
        open(QIODevice::ReadOnly | QIODevice::Unbuffered);
    }

    void supply(const QByteArray &data)
    {
        data_.append(data);
        emit readyRead();
    }

    void finish(NetworkError error = NoError, int status = 200)
    {
        setAttribute(QNetworkRequest::HttpStatusCodeAttribute, status);
        if (error != NoError)
            setError(error, QStringLiteral("Download fixture failure"));
        setFinished(true);
        emit finished();
    }

    void abort() override
    {
        ++aborts_;
        finish(OperationCanceledError);
    }

    qint64 bytesAvailable() const override
    {
        return data_.size() - offset_ + QNetworkReply::bytesAvailable();
    }

protected:
    qint64 readData(char *destination, qint64 maximum) override
    {
        const qint64 size = qMin(maximum, data_.size() - offset_);
        if (size == 0)
            return -1;
        std::memcpy(destination, data_.constData() + offset_, size_t(size));
        offset_ += size;
        return size;
    }

private:
    int &aborts_;
    QByteArray data_;
    qint64 offset_ = 0;
};

class ModelNetwork final : public QNetworkAccessManager
{
public:
    QList<QNetworkRequest> requests;
    QList<QPointer<ModelReply>> replies;
    int aborts = 0;

protected:
    QNetworkReply *createRequest(Operation, const QNetworkRequest &request, QIODevice *) override
    {
        requests.append(request);
        auto *reply = new ModelReply(this, request, aborts);
        replies.append(reply);
        return reply;
    }
};

const QMap<QString, QString> kUiModels = {
    {"en", "eng"}, {"ar", "ara"}, {"az", "aze"}, {"be", "bel"}, {"bg", "bul"},
    {"bs", "bos"}, {"ca", "cat"}, {"cnr", ""}, {"cs", "ces"}, {"da", "dan"},
    {"de", "deu"}, {"el", "ell"}, {"es", "spa"}, {"et", "est"}, {"fi", "fin"},
    {"fr", "fra"}, {"ga", "gle"}, {"hi", "hin"}, {"hr", "hrv"}, {"hu", "hun"},
    {"hy", "hye"}, {"id", "ind"}, {"is", "isl"}, {"it", "ita"}, {"ja", "jpn"},
    {"ka", "kat"}, {"kk", "kaz"}, {"ko", "kor"}, {"lb", "ltz"}, {"lt", "lit"},
    {"lv", "lav"}, {"mk", "mkd"}, {"mt", "mlt"}, {"nb", "nor"}, {"nl", "nld"},
    {"nn", "nor"}, {"pl", "pol"}, {"pt_BR", "por"}, {"pt_PT", "por"}, {"rm", ""},
    {"ro", "ron"}, {"ru", "rus"}, {"sk", "slk"}, {"sl", "slv"}, {"sq", "sqi"},
    {"sr", "srp"}, {"sv", "swe"}, {"th", "tha"}, {"tr", "tur"}, {"uk", "ukr"},
    {"vi", "vie"}, {"zh_CN", "chi_sim"}, {"zh_TW", "chi_tra"},
};

} // namespace

class TstOcrProvisioner : public QObject
{
    Q_OBJECT

private slots:
    void uiLanguageMappings();
    void osLanguageMappings_data();
    void osLanguageMappings();
    void selectedAndOsModels();
    void downloadsBestModelsOnce();
    void replacesExistingModelOnlyOnSuccess_data();
    void replacesExistingModelOnlyOnSuccess();
    void failureDoesNotStopOtherLanguage();
    void cancelAndDestructionAbandonDownloads();
    void inactivityAbandonsDownload();
    void unwritableDestinationDoesNotRequest();
};

void TstOcrProvisioner::uiLanguageMappings()
{
    for (const QString &language : mervin::i18n::availableLanguages())
        QVERIFY2(kUiModels.contains(language), qPrintable(language));
    for (auto it = kUiModels.cbegin(); it != kUiModels.cend(); ++it)
        QCOMPARE(mervin::ocr::modelForLanguage(it.key()), it.value());
}

void TstOcrProvisioner::osLanguageMappings_data()
{
    QTest::addColumn<QString>("locale");
    QTest::addColumn<QString>("model");
    const QMap<QString, QString> matches = {
        {"sv-SE", "swe"}, {"SWE-se", "swe"}, {"en-US", "eng"}, {"de_AT", "deu"},
        {"no-NO", "nor"}, {"nb-NO", "nor"}, {"nn-NO", "nor"}, {"pt-BR", "por"},
        {"pt-PT", "por"}, {"zh-CN", "chi_sim"}, {"zh-SG", "chi_sim"},
        {"zh-TW", "chi_tra"}, {"zh-HK", "chi_tra"}, {"zh-MO", "chi_tra"},
        {"zh-Hant-CN", "chi_tra"}, {"zh-Hans-TW", "chi_sim"},
        {"sr-Latn-RS", "srp_latn"}, {"sr-Cyrl-RS", "srp"},
        {"az-Cyrl-AZ", "aze_cyrl"}, {"az-Latn-AZ", "aze"},
        {"uz-Cyrl-UZ", "uzb_cyrl"}, {"uz-Latn-UZ", "uzb"},
        {"fa-IR", "fas"}, {"he-IL", "heb"}, {"pa-IN", "pan"},
        {"fil-PH", "fil"}, {"ja-JP", "jpn"}, {"ko-KR", "kor"},
        {"cnr-ME", ""}, {"rm-CH", ""}, {"zz-ZZ", ""}, {"C", ""}, {"", ""},
        {"nb-Cyrl-NO", ""}, {"zh-Latn-CN", ""}, {"az-Arab-IR", ""},
        {"az-IR", ""}, {"pa-PK", ""}, {"mn-Mong-CN", ""},
    };
    for (auto it = matches.cbegin(); it != matches.cend(); ++it)
        QTest::newRow(it.key().isEmpty() ? "empty" : qPrintable(it.key())) << it.key() << it.value();
}

void TstOcrProvisioner::osLanguageMappings()
{
    QFETCH(QString, locale);
    QFETCH(QString, model);
    QCOMPARE(mervin::ocr::modelForLanguage(locale), model);
}

void TstOcrProvisioner::selectedAndOsModels()
{
    using mervin::ocr::initialModels;
    QCOMPARE(initialModels("sv", "en-US"), QStringList({"swe", "eng"}));
    QCOMPARE(initialModels("nb", "nn-NO"), QStringList({"nor"}));
    QCOMPARE(initialModels("pt_BR", "pt-PT"), QStringList({"por"}));
    QCOMPARE(initialModels("zh_TW", "zh-HK"), QStringList({"chi_tra"}));
    QCOMPARE(initialModels("zh_TW", "zh-CN"), QStringList({"chi_tra", "chi_sim"}));
    QCOMPARE(initialModels("zh_CN", "zh-TW"), QStringList({"chi_sim", "chi_tra"}));
    QCOMPARE(initialModels("cnr", "sv-SE"), QStringList({"swe"}));
    QCOMPARE(initialModels("sv", "zz-ZZ"), QStringList({"swe"}));
    QCOMPARE(initialModels("cnr", "rm-CH"), QStringList());
}

void TstOcrProvisioner::downloadsBestModelsOnce()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    ModelNetwork network;
    mervin::ocr::OcrProvisioner provisioner(nullptr, &network);
    QSignalSpy installed(&provisioner, &mervin::ocr::OcrProvisioner::modelInstalled);
    QSignalSpy finished(&provisioner, &mervin::ocr::OcrProvisioner::finished);
    const QStringList models = {"swe", "eng", "swe", "../fra", "", "en"};
    provisioner.start(models, directory.path());
    provisioner.start(models, directory.path());
    QCOMPARE(network.requests.size(), 2);
    for (int i = 0; i < 2; ++i) {
        QCOMPARE(network.requests.at(i).url(), QUrl(QStringLiteral(
            "https://raw.githubusercontent.com/tesseract-ocr/tessdata_best/main/%1.traineddata")
                                                       .arg(models.at(i))));
        QCOMPARE(network.requests.at(i).transferTimeout(), 30000);
    }

    const QByteArray data = modelData();
    network.replies.at(0)->supply(data.first(data.size() / 2));
    QVERIFY(!QFile::exists(directory.filePath("swe.traineddata")));
    network.replies.at(0)->supply(data.mid(data.size() / 2));
    network.replies.at(0)->finish();
    QCOMPARE(installed.count(), 1);
    QCOMPARE(finished.count(), 0);
    QCOMPARE(readFile(directory.filePath("swe.traineddata")), data);
    QVERIFY(mervin::TessdataFile::validate(directory.filePath("swe.traineddata")));

    network.replies.at(1)->supply(data);
    network.replies.at(1)->finish();
    QCOMPARE(installed.count(), 2);
    QCOMPARE(finished.count(), 1);
    QCOMPARE(files(directory.path()), QStringList({"eng.traineddata", "swe.traineddata"}));
    provisioner.start(models, directory.path());
    QCOMPARE(network.requests.size(), 2);
    QCOMPARE(network.aborts, 0);
}

void TstOcrProvisioner::replacesExistingModelOnlyOnSuccess_data()
{
    QTest::addColumn<QByteArray>("data");
    QTest::addColumn<int>("error");
    QTest::addColumn<int>("status");
    QTest::addColumn<bool>("success");
    QTest::newRow("best replaces existing") << modelData('B') << int(QNetworkReply::NoError)
                                           << 200 << true;
    QTest::newRow("invalid body") << QByteArray("<html>not a model</html>")
                                  << int(QNetworkReply::NoError) << 200 << false;
    QTest::newRow("truncated model") << modelData('B').first(50)
                                     << int(QNetworkReply::NoError) << 200 << false;
    QTest::newRow("unreachable") << QByteArray() << int(QNetworkReply::HostNotFoundError)
                                 << 0 << false;
    QTest::newRow("connection interrupted") << modelData('B')
                                            << int(QNetworkReply::RemoteHostClosedError)
                                            << 200 << false;
    QTest::newRow("http failure") << modelData('B') << int(QNetworkReply::NoError)
                                  << 404 << false;
    QTest::newRow("redirect") << modelData('B') << int(QNetworkReply::NoError) << 302 << false;
}

void TstOcrProvisioner::replacesExistingModelOnlyOnSuccess()
{
    QFETCH(QByteArray, data);
    QFETCH(int, error);
    QFETCH(int, status);
    QFETCH(bool, success);
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString target = directory.filePath("eng.traineddata");
    const QByteArray original = modelData('A');
    QVERIFY(writeFile(target, original));
    ModelNetwork network;
    mervin::ocr::OcrProvisioner provisioner(nullptr, &network);
    QSignalSpy installed(&provisioner, &mervin::ocr::OcrProvisioner::modelInstalled);
    QSignalSpy finished(&provisioner, &mervin::ocr::OcrProvisioner::finished);
    provisioner.start({"eng"}, directory.path());
    QCOMPARE(network.requests.size(), 1);
    network.replies.first()->supply(data);
    QCOMPARE(readFile(target), original);
    network.replies.first()->finish(QNetworkReply::NetworkError(error), status);
    QCOMPARE(installed.count(), success ? 1 : 0);
    QCOMPARE(finished.count(), 1);
    QCOMPARE(readFile(target), success ? data : original);
    QCOMPARE(files(directory.path()), QStringList({"eng.traineddata"}));
    provisioner.start({"eng"}, directory.path());
    QCOMPARE(network.requests.size(), 1);
}

void TstOcrProvisioner::failureDoesNotStopOtherLanguage()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    ModelNetwork network;
    mervin::ocr::OcrProvisioner provisioner(nullptr, &network);
    QSignalSpy installed(&provisioner, &mervin::ocr::OcrProvisioner::modelInstalled);
    QSignalSpy finished(&provisioner, &mervin::ocr::OcrProvisioner::finished);
    provisioner.start({"swe", "eng"}, directory.path());
    network.replies.first()->finish(QNetworkReply::HostNotFoundError, 0);
    network.replies.last()->supply(modelData());
    network.replies.last()->finish();
    QCOMPARE(installed.count(), 1);
    QCOMPARE(installed.first().first().toString(), QStringLiteral("eng"));
    QCOMPARE(finished.count(), 1);
    QCOMPARE(files(directory.path()), QStringList({"eng.traineddata"}));
}

void TstOcrProvisioner::cancelAndDestructionAbandonDownloads()
{
    for (bool cancelExplicitly : {true, false}) {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        ModelNetwork network;
        {
            mervin::ocr::OcrProvisioner provisioner(nullptr, &network);
            QSignalSpy installed(&provisioner, &mervin::ocr::OcrProvisioner::modelInstalled);
            QSignalSpy finished(&provisioner, &mervin::ocr::OcrProvisioner::finished);
            provisioner.start({"swe", "eng"}, directory.path());
            network.replies.first()->supply(modelData().first(50));
            if (cancelExplicitly) {
                provisioner.cancel();
                provisioner.cancel();
                provisioner.start({"swe", "eng"}, directory.path());
                QCOMPARE(finished.count(), 1);
                QCOMPARE(installed.count(), 0);
            }
        }
        QCOMPARE(network.requests.size(), 2);
        QCOMPARE(network.aborts, 2);
        QVERIFY(files(directory.path()).isEmpty());
    }
}

void TstOcrProvisioner::inactivityAbandonsDownload()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    ModelNetwork network;
    mervin::ocr::OcrProvisioner provisioner(nullptr, &network, 20);
    QSignalSpy finished(&provisioner, &mervin::ocr::OcrProvisioner::finished);
    provisioner.start({"swe"}, directory.path());
    network.replies.first()->supply(modelData().first(50));
    QTRY_COMPARE(finished.count(), 1);
    QCOMPARE(network.aborts, 1);
    QVERIFY(files(directory.path()).isEmpty());
    provisioner.start({"swe"}, directory.path());
    QCOMPARE(network.requests.size(), 1);
}

void TstOcrProvisioner::unwritableDestinationDoesNotRequest()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = directory.filePath("file-instead-of-directory");
    QVERIFY(writeFile(path, QByteArray("fixture")));
    ModelNetwork network;
    mervin::ocr::OcrProvisioner provisioner(nullptr, &network);
    QSignalSpy finished(&provisioner, &mervin::ocr::OcrProvisioner::finished);
    provisioner.start({"eng"}, path);
    QCOMPARE(finished.count(), 1);
    QVERIFY(network.requests.isEmpty());
    provisioner.start({"eng"}, directory.path());
    QVERIFY(network.requests.isEmpty());
}

QTEST_GUILESS_MAIN(TstOcrProvisioner)
#include "tst_ocr_provisioner.moc"
