#include "i18n/UiLanguage.h"
#include "ocr/OcrLanguageMapping.h"
#include "ocr/OcrProvisioner.h"
#include "ocr/TessdataManager.h"
#include "ocr/TessdataFile.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMap>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QPointer>
#include <QSaveFile>
#include <QSignalSpy>
#include <QScopeGuard>
#include <QSemaphore>
#include <QTemporaryDir>
#include <QTest>
#include <QThread>
#include <QtEndian>

#include <atomic>
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
    void queuedInstallationLifetime_data();
    void queuedInstallationLifetime();
    void laterBytesRestartInactivityDeadline();
    void installFailureAfterSuccessfulStaging();
    void installFailureAfterDestinationOpened_data();
    void installFailureAfterDestinationOpened();
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
    // The downloads start once the OCR folder's thread has made their files.
    QTRY_COMPARE(network.requests.size(), 2);
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
    QTRY_VERIFY(!mervin::TessdataManager::busy()); // the files are written on its thread
    QCOMPARE(installed.count(), 1);
    QCOMPARE(installed[0][0].toString(), QStringLiteral("swe"));
    QCOMPARE(finished.count(), 0);
    QCOMPARE(readFile(directory.filePath("swe.traineddata")), data);
    QVERIFY(mervin::TessdataFile::validate(directory.filePath("swe.traineddata")));

    network.replies.at(1)->supply(data);
    network.replies.at(1)->finish();
    QTRY_VERIFY(!mervin::TessdataManager::busy());
    QCOMPARE(installed.count(), 2);
    QCOMPARE(installed[1][0].toString(), QStringLiteral("eng"));
    QCOMPARE(readFile(directory.filePath("eng.traineddata")), data);
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
    QTRY_COMPARE(network.requests.size(), 1);
    network.replies.first()->supply(data);
    QTRY_VERIFY(!mervin::TessdataManager::busy()); // the files are written on its thread
    QCOMPARE(readFile(target), original);
    network.replies.first()->finish(QNetworkReply::NetworkError(error), status);
    QTRY_VERIFY(!mervin::TessdataManager::busy());
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
    QTRY_COMPARE(network.requests.size(), 2);
    network.replies.first()->finish(QNetworkReply::HostNotFoundError, 0);
    network.replies.last()->supply(modelData());
    network.replies.last()->finish();
    QTRY_VERIFY(!mervin::TessdataManager::busy());
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
            QTRY_COMPARE(network.requests.size(), 2);
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
        QTRY_VERIFY(!mervin::TessdataManager::busy()); // the staging files go on its thread
        QVERIFY(files(directory.path()).isEmpty());
    }
}

// Explicit cancel stops queued installation; destruction only abandons live downloads.
void TstOcrProvisioner::queuedInstallationLifetime_data()
{
    QTest::addColumn<bool>("cancelExplicitly");
    QTest::newRow("explicit cancel keeps original") << true;
    QTest::newRow("destruction permits queued installation") << false;
}

void TstOcrProvisioner::queuedInstallationLifetime()
{
    QFETCH(bool, cancelExplicitly);
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString target = directory.filePath("eng.traineddata");
    const QByteArray original = modelData('A');
    const QByteArray replacement = modelData('B');
    QVERIFY(writeFile(target, original));
    ModelNetwork network;
    auto provisioner = std::make_unique<mervin::ocr::OcrProvisioner>(nullptr, &network);
    int notifications = 0;
    connect(provisioner.get(), &mervin::ocr::OcrProvisioner::modelInstalled,
            this, [&] { ++notifications; });
    QSignalSpy finished(provisioner.get(), &mervin::ocr::OcrProvisioner::finished);
    provisioner->start({"eng"}, directory.path());
    QTRY_COMPARE(network.requests.size(), 1);
    network.replies.first()->supply(replacement);
    QTRY_VERIFY(!mervin::TessdataManager::busy());
    auto entered = std::make_shared<QSemaphore>();
    auto release = std::make_shared<QSemaphore>();
    mervin::TessdataManager::runInFolder([entered, release] {
        entered->release();
        release->tryAcquire(1, 10000);
    });
    const auto unblock = qScopeGuard([release] { release->release(); });
    QVERIFY(entered->tryAcquire(1, 5000));
    network.replies.first()->finish();
    if (cancelExplicitly) {
        provisioner->cancel();
        provisioner->cancel();
        QCOMPARE(finished.count(), 1);
    } else {
        provisioner.reset();
        QCOMPARE(finished.count(), 0);
    }
    QCOMPARE(readFile(target), original);
    release->release();
    QTRY_VERIFY(!mervin::TessdataManager::busy());
    QCOMPARE(notifications, 0);
    QCOMPARE(readFile(target), cancelExplicitly ? original : replacement);
    QCOMPARE(files(directory.path()), QStringList({"eng.traineddata"}));
}

// Exercise the inactivity deadline itself, including a later chunk extending it.
void TstOcrProvisioner::laterBytesRestartInactivityDeadline()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    ModelNetwork network;
    mervin::ocr::OcrProvisioner provisioner(nullptr, &network, 1000);
    QSignalSpy finished(&provisioner, &mervin::ocr::OcrProvisioner::finished);
    QSignalSpy installed(&provisioner, &mervin::ocr::OcrProvisioner::modelInstalled);
    provisioner.start({"eng"}, directory.path());
    QTRY_COMPARE(network.requests.size(), 1);
    const QByteArray data = modelData();
    network.replies.first()->supply(data.first(50));
    QTest::qWait(600);
    QCOMPARE(finished.count(), 0);
    QVERIFY(network.replies.first());
    network.replies.first()->supply(data.mid(50));
    QTest::qWait(600); // beyond the first deadline, within the restarted deadline
    QCOMPARE(finished.count(), 0);
    QCOMPARE(network.aborts, 0);
    QVERIFY(network.replies.first());
    network.replies.first()->finish();
    QTRY_VERIFY(!mervin::TessdataManager::busy());
    QCOMPARE(finished.count(), 1);
    QCOMPARE(installed.count(), 1);
    QCOMPARE(readFile(directory.filePath("eng.traineddata")), data);
}

// A destination open failure after staging must not report success.
void TstOcrProvisioner::installFailureAfterSuccessfulStaging()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    ModelNetwork network;
    mervin::ocr::OcrProvisioner provisioner(nullptr, &network);
    QSignalSpy installed(&provisioner, &mervin::ocr::OcrProvisioner::modelInstalled);
    QSignalSpy finished(&provisioner, &mervin::ocr::OcrProvisioner::finished);
    provisioner.start({"eng"}, directory.path());
    QTRY_COMPARE(network.requests.size(), 1);
    network.replies.first()->supply(modelData('B'));
    QTRY_VERIFY(!mervin::TessdataManager::busy());
    const QString target = directory.filePath("eng.traineddata");
    QVERIFY(QDir().mkdir(target)); // works even under root, unlike permission tricks
    const QString sentinel = QDir(target).filePath("original");
    QVERIFY(writeFile(sentinel, modelData('A')));
    network.replies.first()->finish();
    QTRY_VERIFY(!mervin::TessdataManager::busy());
    QCOMPARE(installed.count(), 0);
    QCOMPARE(finished.count(), 1);
    QCOMPARE(readFile(sentinel), modelData('A'));
    QCOMPARE(QDir(directory.path()).entryList(QDir::Files | QDir::Hidden).size(), 0);
    // The same valid bytes install once the destination conflict is removed.
    QVERIFY(QFile::remove(sentinel));
    QVERIFY(QDir().rmdir(target));
    mervin::ocr::OcrProvisioner retry(nullptr, &network);
    QSignalSpy retried(&retry, &mervin::ocr::OcrProvisioner::modelInstalled);
    retry.start({"eng"}, directory.path());
    QTRY_COMPARE(network.requests.size(), 2);
    network.replies.last()->supply(modelData('B'));
    network.replies.last()->finish();
    QTRY_VERIFY(!mervin::TessdataManager::busy());
    QCOMPARE(retried.count(), 1);
    QCOMPARE(readFile(target), modelData('B'));
}

/// Selects failures during copying and after the complete copy reaches commit.
void TstOcrProvisioner::installFailureAfterDestinationOpened_data()
{
    QTest::addColumn<bool>("failDuringWrite");
    QTest::newRow("during-write") << true;
    QTest::newRow("during-commit") << false;
}

/// Keeps an existing regular model intact when the opened replacement fails.
void TstOcrProvisioner::installFailureAfterDestinationOpened()
{
    QFETCH(bool, failDuringWrite);
    using Provisioner = mervin::ocr::OcrProvisioner;
    using Phase = Provisioner::InstallPhase;
    struct Observation
    {
        Phase phase;
        qint64 position;
        bool open;
        QString target;
    };
    struct Installation
    {
        QList<Observation> observations;
        bool canceled = false;
        qint64 canceledAt = -1;
    };
    const auto installation = std::make_shared<Installation>();
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString target = directory.filePath("eng.traineddata");
    const QByteArray original = modelData('A');
    QVERIFY(writeFile(target, original));
    QVERIFY(QFileInfo(target).isFile());

    // Add an opaque component before the unicharset so copying needs several
    // writes. The write failure occurs only after some replacement bytes landed.
    QByteArray replacement = modelData('B');
    constexpr qsizetype tableEnd = sizeof(quint32) + 22 * sizeof(qint64);
    const QByteArray opaque(128 * 1024, 'x');
    replacement.insert(tableEnd, opaque);
    qToLittleEndian<qint64>(tableEnd, replacement.data() + sizeof(quint32));
    qToLittleEndian<qint64>(tableEnd + opaque.size(),
                           replacement.data() + sizeof(quint32) + 21 * sizeof(qint64));

    ModelNetwork network;
    Provisioner provisioner(nullptr, &network);
    provisioner.setInstallHookForTesting([installation, failDuringWrite](QSaveFile &output,
                                                                        Phase phase) {
        installation->observations.append(
            {phase, output.pos(), output.isOpen(), output.fileName()});
        const bool failHere = failDuringWrite
            ? phase == Phase::BeforeWrite && output.pos() > 0
            : phase == Phase::BeforeCommit;
        if (failHere && !installation->canceled) {
            installation->canceled = true;
            installation->canceledAt = output.pos();
            output.cancelWriting();
        }
    });
    QSignalSpy installed(&provisioner, &Provisioner::modelInstalled);
    QSignalSpy finished(&provisioner, &Provisioner::finished);
    provisioner.start({"eng"}, directory.path());
    QTRY_COMPARE(network.requests.size(), 1);
    network.replies.first()->supply(replacement);
    QTRY_VERIFY(!mervin::TessdataManager::busy());
    QCOMPARE(readFile(target), original);
    network.replies.first()->finish();
    QTRY_COMPARE(finished.count(), 1);
    QTRY_VERIFY(!mervin::TessdataManager::busy());

    QVERIFY(installation->canceled);
    QVERIFY(installation->observations.size() >= 2);
    QCOMPARE(installation->observations.first().phase, Phase::BeforeWrite);
    QCOMPARE(installation->observations.first().position, 0);
    qsizetype commitPhases = 0;
    for (const Observation &observation : installation->observations) {
        QVERIFY(observation.open);
        QCOMPARE(observation.target, target);
        if (observation.phase == Phase::BeforeCommit)
            ++commitPhases;
    }
    if (failDuringWrite) {
        QVERIFY(installation->canceledAt > 0);
        QVERIFY(installation->canceledAt < replacement.size());
        QCOMPARE(installation->observations.last().phase, Phase::BeforeWrite);
        QCOMPARE(installation->observations.last().position, installation->canceledAt);
        QCOMPARE(commitPhases, 0); // a failed write must stop before commit
    } else {
        QCOMPARE(installation->canceledAt, replacement.size());
        QCOMPARE(installation->observations.last().phase, Phase::BeforeCommit);
        QCOMPARE(commitPhases, 1);
    }
    QCOMPARE(installed.count(), 0);
    QCOMPARE(finished.count(), 1);
    QVERIFY(QFileInfo(target).isFile());
    QVERIFY(!QFileInfo(target).isSymLink());
    QCOMPARE(readFile(target), original);
    QCOMPARE(QDir(directory.path()).entryList(QDir::AllEntries | QDir::Hidden
                                              | QDir::System | QDir::NoDotAndDotDot),
             QStringList({"eng.traineddata"}));

    // Provisioners are single-use. A fresh instance retries the same valid bytes
    // without the fault, while the failed instance's callback remains isolated.
    const qsizetype observedPhases = installation->observations.size();
    Provisioner retry(nullptr, &network);
    QSignalSpy retried(&retry, &Provisioner::modelInstalled);
    QSignalSpy retryFinished(&retry, &Provisioner::finished);
    retry.start({"eng"}, directory.path());
    QTRY_COMPARE(network.requests.size(), 2);
    network.replies.last()->supply(replacement);
    network.replies.last()->finish();
    QTRY_COMPARE(retryFinished.count(), 1);
    QTRY_VERIFY(!mervin::TessdataManager::busy());
    QCOMPARE(retried.count(), 1);
    QCOMPARE(retried.first().first().toString(), QStringLiteral("eng"));
    QCOMPARE(readFile(target), replacement);
    QVERIFY(mervin::TessdataFile::validate(target));
    QCOMPARE(installed.count(), 0);
    QCOMPARE(finished.count(), 1);
    QCOMPARE(installation->observations.size(), observedPhases);
    QCOMPARE(QDir(directory.path()).entryList(QDir::AllEntries | QDir::Hidden
                                              | QDir::System | QDir::NoDotAndDotDot),
             QStringList({"eng.traineddata"}));
}

void TstOcrProvisioner::inactivityAbandonsDownload()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    ModelNetwork network;
    // Long enough to supply data once the download has started, which waits for its
    // staging file on the OCR folder's thread.
    mervin::ocr::OcrProvisioner provisioner(nullptr, &network, 300);
    QSignalSpy finished(&provisioner, &mervin::ocr::OcrProvisioner::finished);
    provisioner.start({"swe"}, directory.path());
    QVERIFY(QTest::qWaitFor([&network] { return network.requests.size() == 1; }, 5000));
    QVERIFY(network.replies.first());
    network.replies.first()->supply(modelData().first(50));
    QTRY_COMPARE(finished.count(), 1);
    QCOMPARE(network.aborts, 1);
    QTRY_VERIFY(!mervin::TessdataManager::busy()); // the staging file goes on its thread
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
    QTRY_COMPARE(finished.count(), 1);
    QVERIFY(network.requests.isEmpty());
    provisioner.start({"eng"}, directory.path());
    QVERIFY(network.requests.isEmpty());
}

QTEST_GUILESS_MAIN(TstOcrProvisioner)
#include "tst_ocr_provisioner.moc"
