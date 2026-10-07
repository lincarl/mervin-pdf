#include "i18n/UiLanguage.h"
#include "ocr/TessdataFile.h"
#include "ocr/TessdataManager.h"

#include <QByteArray>
#include <QDir>
#include <QFile>
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
    void rejectsEmDashPurgedModel();
    void rejectsTruncatedModel();
    void rejectsFileThatIsNotTessdata();
    void missingLanguageIsNotOurError();
    void repositoryOffersOnlyBestModels();
    void languageNamesAreFriendly();
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
    QVERIFY(url.contains(QStringLiteral("/tessdata_best")));
    QVERIFY(!url.contains(QStringLiteral("tessdata_fast")));
}

QTEST_GUILESS_MAIN(TstTessdata)
#include "tst_tessdata.moc"
