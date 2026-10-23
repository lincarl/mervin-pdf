#include "config/ConfigPaths.h"
#include "i18n/UiLanguage.h"
#include "ocr/TessdataFile.h"
#include "ocr/TessdataManager.h"

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QScopeGuard>
#include <QString>
#include <QTemporaryDir>
#include <QTest>
#include <QtEndian>

namespace TessdataFile = mervin::TessdataFile;

// Validate the container and character tables before damaged language models can
// reach Tesseract. Generated fixtures keep these checks independent of downloads.
class TstTessdata : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void acceptsValidContainer();
    void containerMatrix_data();
    void containerMatrix();
    void validatesEveryRequestedLanguage();
    void rejectsEmDashPurgedModel();
    void rejectsTruncatedModel();
    void rejectsFileThatIsNotTessdata();
    void missingLanguageIsNotOurError();
    void repositoryOffersOnlyBestModels();
    void languageNamesAreFriendly();
    void modelsAreListedSavedAndRemovedInOrder();
};

namespace {

// A minimal container with a real LSTM unicharset component. The validator does
// not parse neural network data, so this is deliberately not an OCR model.
QByteArray validContainer()
{
    constexpr quint32 count = 22;
    constexpr qsizetype headerSize = sizeof(quint32) + count * sizeof(qint64);
    QByteArray bytes(headerSize, '\0');
    qToLittleEndian<quint32>(count, bytes.data());
    for (quint32 slot = 0; slot < count; ++slot)
        qToLittleEndian<qint64>(slot == 21 ? headerSize : -1,
                               bytes.data() + sizeof(quint32) + slot * sizeof(qint64));
    bytes += "3\n- 0 Common 0\n\xE2\x80\x94 0 Common 1\nA 3 Latin 2\n";
    return bytes;
}

// Build independent component tables in either byte order, including opaque slots.
QByteArray container(const QList<QByteArray> &components, bool bigEndian = false)
{
    QByteArray bytes(4 + components.size() * 8, '\0');
    if (bigEndian)
        qToBigEndian<quint32>(components.size(), bytes.data());
    else
        qToLittleEndian<quint32>(components.size(), bytes.data());
    for (qsizetype i = 0; i < components.size(); ++i) {
        const qint64 offset = components[i].isNull() ? -1 : bytes.size();
        if (bigEndian)
            qToBigEndian<qint64>(offset, bytes.data() + 4 + i * 8);
        else
            qToLittleEndian<qint64>(offset, bytes.data() + 4 + i * 8);
        bytes += components[i];
    }
    return bytes;
}

// Write `bytes` into `dir` as eng.traineddata and return the path.
QString writeModel(const QString &dir, const QByteArray &bytes)
{
    const QString path = QDir(dir).filePath(QStringLiteral("eng.traineddata"));
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly))
        return {};
    f.write(bytes);
    f.close();
    return path;
}

} // namespace

// The checks below read English messages and names.
void TstTessdata::initTestCase()
{
    mervin::i18n::apply(QStringLiteral("en"));
}

void TstTessdata::languageNamesAreFriendly()
{
    // Mervin's own table names every tessdata_best model, in the UI language.
    QCOMPARE(mervin::TessdataManager::languageName(QStringLiteral("eng")),
             QStringLiteral("English"));
    QCOMPARE(mervin::TessdataManager::languageName(QStringLiteral("swe")),
             QStringLiteral("Swedish"));
    QCOMPARE(mervin::TessdataManager::languageName(QStringLiteral("osd")),
             QStringLiteral("Orientation and script detection"));
    // A code missing from the table is returned unchanged unless QLocale knows it.
    QCOMPARE(mervin::TessdataManager::languageName(QStringLiteral("unknown_model")),
             QStringLiteral("unknown_model"));

    // In Swedish the names are translated, and the English name stays available
    // for the search.
    mervin::i18n::apply(QStringLiteral("sv"));
    QCOMPARE(mervin::TessdataManager::languageName(QStringLiteral("swe")),
             QStringLiteral("Svenska"));
    QCOMPARE(mervin::TessdataManager::englishLanguageName(QStringLiteral("swe")),
             QStringLiteral("Swedish"));
    mervin::i18n::apply(QStringLiteral("en"));
}

void TstTessdata::acceptsValidContainer()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = writeModel(dir.path(), validContainer());
    QVERIFY(!path.isEmpty());

    QString err;
    QVERIFY2(TessdataFile::validate(path, &err), qPrintable(err));
    QVERIFY2(TessdataFile::validateLanguages(dir.path(), {QStringLiteral("eng")}, &err),
             qPrintable(err));
}

// Pin accepted byte orders and every structural rejection without a native OCR call.
void TstTessdata::containerMatrix_data()
{
    QTest::addColumn<QByteArray>("bytes");
    QTest::addColumn<QString>("detail");
    const QByteArray unichars("2\nNULL 0\nA 1\n");
    for (bool big : {false, true}) {
        QList<QByteArray> components(22);
        components[1] = unichars;
        components[3] = QByteArrayLiteral("opaque");
        components[21] = unichars;
        QTest::newRow(big ? "big-endian offsets" : "little-endian offsets")
            << container(components, big) << QString();
    }
    QTest::newRow("legacy slot") << container({{}, unichars}) << QString();
    QTest::newRow("opaque component") << container({QByteArrayLiteral("opaque")}) << QString();
    QTest::newRow("zero count") << QByteArray(4, '\0') << "implausible component count";
    QByteArray excessive(4, char(0xff));
    QTest::newRow("excessive count") << excessive << "implausible component count";
    QTest::newRow("short header") << QByteArray(3, '\0') << "too small";
    QTest::newRow("short table") << container({{}, unichars}).first(12) << "offset table needs";
    const auto badOffset = [&](const char *name, int slot, qint64 offset, const char *detail) {
        QByteArray bytes = container({QByteArrayLiteral("opaque"), unichars});
        qToLittleEndian<qint64>(offset, bytes.data() + 4 + slot * 8);
        QTest::newRow(name) << bytes << QString::fromLatin1(detail);
    };
    badOffset("inside header", 0, 19, "outside");
    badOffset("beyond end", 1, 1000, "outside");
    QByteArray descending = container({QByteArrayLiteral("opaque"), unichars});
    qToLittleEndian<qint64>(27, descending.data() + 4);
    QTest::newRow("strictly descending offsets") << descending << "before the previous";
    badOffset("equal offsets", 1, 20, "before the previous");
    QByteArray eof = container({QByteArrayLiteral("opaque")});
    qToLittleEndian<qint64>(eof.size(), eof.data() + 4);
    QTest::newRow("empty component at EOF") << eof << "non-positive length";
    QTest::newRow("invalid unichar count") << container({{}, QByteArrayLiteral("no count\n")})
        << "does not start with a unichar count";
    QTest::newRow("zero unichar count") << container({{}, QByteArrayLiteral("0\n")})
        << "does not start with a unichar count";
    QTest::newRow("unichar count shortfall") << container({{}, QByteArrayLiteral("3\nA 1\n\nB 1\n")})
        << "declares 3 unichars but holds 2";
    QTest::newRow("empty unichar token") << container({{}, QByteArrayLiteral("1\n A 1\n")})
        << "empty unichar entry";
    QTest::newRow("legacy duplicate") << container({{}, QByteArrayLiteral("2\nA 1\nA 2\n")})
        << "twice";
}

void TstTessdata::containerMatrix()
{
    QFETCH(QByteArray, bytes);
    QFETCH(QString, detail);
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = writeModel(dir.path(), bytes);
    QVERIFY(!path.isEmpty());
    QString error;
    QCOMPARE(TessdataFile::validate(path, &error), detail.isEmpty());
    if (detail.isEmpty())
        QVERIFY(error.isEmpty());
    else {
        QVERIFY2(error.contains(detail), qPrintable(error));
        QVERIFY(error.contains(QStringLiteral("eng.traineddata")));
    }
}

// Missing/empty languages defer to Tesseract, but a later damaged model must fail.
void TstTessdata::validatesEveryRequestedLanguage()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    QVERIFY(!writeModel(dir.path(), validContainer()).isEmpty());
    QFile bad(dir.filePath(QStringLiteral("swe.traineddata")));
    QVERIFY(bad.open(QIODevice::WriteOnly));
    QCOMPARE(bad.write("bad"), qint64(3));
    bad.close();
    QString error;
    QVERIFY(TessdataFile::validateLanguages(dir.path(), {"", "missing", "eng", ""}, &error));
    QVERIFY(error.isEmpty());
    QVERIFY(!TessdataFile::validateLanguages(dir.path(), {"", "missing", "eng", "swe"}, &error));
    QVERIFY(error.contains(QStringLiteral("swe.traineddata")));
    QVERIFY(error.contains(QStringLiteral("too small")));
}

// Replacing an em dash with a hyphen once damaged a shipped model. Duplicate
// unichars can make Tesseract abort even when the container offsets look valid.
void TstTessdata::rejectsEmDashPurgedModel()
{
    const QByteArray good = validContainer();
    const QByteArray purged = QByteArray(good).replace("\xE2\x80\x94", "-");
    QCOMPARE(purged.size(), good.size() - 2);

    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = writeModel(dir.path(), purged);
    QVERIFY(!path.isEmpty());

    QString err;
    QVERIFY2(!TessdataFile::validate(path, &err), "duplicate unichar was accepted");
    QVERIFY2(err.contains(QStringLiteral("twice")), qPrintable(err));

    QString langErr;
    QVERIFY(!TessdataFile::validateLanguages(dir.path(), {QStringLiteral("eng")}, &langErr));
    QVERIFY(!langErr.isEmpty());
}

void TstTessdata::rejectsTruncatedModel()
{
    const QByteArray good = validContainer();
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = writeModel(dir.path(), good.left(good.size() / 2));
    QVERIFY(!path.isEmpty());

    QString err;
    QVERIFY2(!TessdataFile::validate(path, &err), "truncated model was accepted");
    QVERIFY(!err.isEmpty());
}

// Whatever a failed download leaves behind (an error page, an empty file) must
// not reach Tesseract either.
void TstTessdata::rejectsFileThatIsNotTessdata()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    const QString html =
        writeModel(dir.path(), QByteArray("<!DOCTYPE html><html>404 Not Found</html>").repeated(40));
    QVERIFY(!html.isEmpty());
    QString err;
    QVERIFY2(!TessdataFile::validate(html, &err), "an HTML error page was accepted");

    QTemporaryDir empty;
    QVERIFY(empty.isValid());
    const QString none = writeModel(empty.path(), QByteArray());
    QVERIFY(!none.isEmpty());
    QVERIFY2(!TessdataFile::validate(none, &err), "an empty file was accepted");

    QVERIFY2(!TessdataFile::validate(QDir(dir.path()).filePath(QStringLiteral("nope.traineddata")),
                                     &err),
             "a nonexistent file was accepted");
}

// A language with no file at all is Tesseract's business - it reports that
// cleanly through its return value, so the validator must not pre-empt it and
// must not claim the data is damaged.
void TstTessdata::missingLanguageIsNotOurError()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    QString err;
    QVERIFY(TessdataFile::validateLanguages(dir.path(), {QStringLiteral("swe")}, &err));
    QVERIFY(err.isEmpty());
}

void TstTessdata::repositoryOffersOnlyBestModels()
{
    const QString url = mervin::TessdataManager::repositoryUrl();
    QCOMPARE(url, QStringLiteral("https://github.com/tesseract-ocr/tessdata_best"));
}

// The folder is in the profile, so the UI reads and changes it through calls that
// run on one background thread, in the order asked.
void TstTessdata::modelsAreListedSavedAndRemovedInOrder()
{
    namespace Models = mervin::TessdataManager;
    QTemporaryDir profile;
    QVERIFY(profile.isValid());
    mervin::ConfigPaths::setOverrideDir(profile.path());
    auto restore = qScopeGuard([] { mervin::ConfigPaths::setOverrideDir({}); });
    QObject context;

    QList<Models::Model> listed{{QStringLiteral("none yet"), 1}};
    Models::listModels(&context, [&listed](const QList<Models::Model> &models) { listed = models; });
    QTRY_VERIFY(!Models::busy());
    QVERIFY(listed.isEmpty()); // no folder yet

    // Asked one after another without waiting.
    Models::Saved saved = Models::Saved::Failed;
    QList<Models::Model> afterSave;
    Models::saveModel(QStringLiteral("eng"), validContainer(), &context,
                      [&](Models::Saved result, const QString &, const QList<Models::Model> &models) {
                          saved = result;
                          afterSave = models;
                      });
    Models::Saved invalid = Models::Saved::Ok;
    QString detail;
    Models::saveModel(QStringLiteral("swe"), QByteArrayLiteral("not a model"), &context,
                      [&](Models::Saved result, const QString &why, const QList<Models::Model> &) {
                          invalid = result;
                          detail = why;
                      });
    Models::listModels(&context, [&listed](const QList<Models::Model> &models) { listed = models; });
    QTRY_VERIFY(!Models::busy());
    QCOMPARE(saved, Models::Saved::Ok);
    QCOMPARE(afterSave.size(), 1);
    QCOMPARE(afterSave.first().code, QStringLiteral("eng"));
    QCOMPARE(afterSave.first().bytes, validContainer().size());
    QCOMPARE(invalid, Models::Saved::Invalid);
    QVERIFY(!detail.isEmpty());
    QVERIFY(!QFile::exists(QDir(Models::directory()).filePath(QStringLiteral("swe.traineddata"))));
    QCOMPARE(listed.size(), 1);
    QFile installed(QDir(Models::directory()).filePath(QStringLiteral("eng.traineddata")));
    QVERIFY(installed.open(QIODevice::ReadOnly));
    QCOMPARE(installed.readAll(), validContainer());
    installed.close();

    // An invalid file leaves the installed model as it was.
    invalid = Models::Saved::Ok;
    Models::saveModel(QStringLiteral("eng"), QByteArrayLiteral("not a model either"), &context,
                      [&](Models::Saved result, const QString &, const QList<Models::Model> &models) {
                          invalid = result;
                          listed = models;
                      });
    QTRY_VERIFY(!Models::busy());
    QCOMPARE(invalid, Models::Saved::Invalid);
    QCOMPARE(listed.size(), 1);
    QCOMPARE(listed.first().bytes, validContainer().size());
    QVERIFY(installed.open(QIODevice::ReadOnly));
    QCOMPARE(installed.readAll(), validContainer());
    installed.close();
    QCOMPARE(QDir(Models::directory()).entryList(QDir::Files | QDir::Hidden),
             QStringList({QStringLiteral("eng.traineddata")}));

    bool removed = false;
    Models::removeModel(QStringLiteral("eng"), &context,
                        [&](bool result, const QList<Models::Model> &models) {
                            removed = result;
                            listed = models;
                        });
    QTRY_VERIFY(!Models::busy());
    QVERIFY(removed);
    QVERIFY(listed.isEmpty());

    // Insert out of order and check the actual sorted contents, not only their sizes.
    for (const QString &code : {QStringLiteral("swe"), QStringLiteral("ara"), QStringLiteral("eng")})
        Models::saveModel(code, validContainer(), &context,
                          [](Models::Saved, const QString &, const QList<Models::Model> &) {});
    Models::listModels(&context, [&listed](const QList<Models::Model> &models) { listed = models; });
    QTRY_VERIFY(!Models::busy());
    QCOMPARE(listed.size(), 3);
    QCOMPARE(listed[0].code, QStringLiteral("ara"));
    QCOMPARE(listed[1].code, QStringLiteral("eng"));
    QCOMPARE(listed[2].code, QStringLiteral("swe"));
    for (const auto &model : listed) {
        QFile file(QDir(Models::directory()).filePath(model.code + QStringLiteral(".traineddata")));
        QVERIFY(file.open(QIODevice::ReadOnly));
        QCOMPARE(file.readAll(), validContainer());
    }

    // An answer for a window that has closed is dropped.
    bool called = false;
    {
        QObject gone;
        Models::listModels(&gone, [&called](const QList<Models::Model> &) { called = true; });
    }
    QTRY_VERIFY(!Models::busy());
    QVERIFY(!called);
}

QTEST_GUILESS_MAIN(TstTessdata)
#include "tst_tessdata.moc"
