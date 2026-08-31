#include "ocr/TessdataFile.h"
#include "ocr/TessdataManager.h"

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QString>
#include <QTemporaryDir>
#include <QTest>

namespace TessdataFile = mervin::TessdataFile;

// Guard rail for the language data the installers ship, and for the validator
// that stands between a damaged model and Tesseract.
//
// This exists because of a real shipped defect: a repo-wide em-dash purge
// (commit da2e02d) rewrote two U+2014 bytes inside the binary
// resources/tessdata/eng.traineddata to '-'. That shrank the file by 4 bytes
// without updating the container's offset table and left the LSTM unicharset
// listing "-" twice, which made Tesseract write past the end of a std::vector
// and abort the whole process on the first OCR. Nothing in the suite noticed,
// because the only OCR test skipped itself unless an env var was set.
class TstTessdata : public QObject
{
    Q_OBJECT

private slots:
    void shippedEngModelIsLoadable();
    void rejectsEmDashPurgedModel();
    void rejectsTruncatedModel();
    void rejectsFileThatIsNotTessdata();
    void missingLanguageIsNotOurError();
    void repositoryOffersOnlyBestModels();
    void languageNamesAreFriendly();
};

namespace {

QString shippedEng()
{
#ifdef MERVIN_TESSDATA_ENG
    return QString::fromUtf8(MERVIN_TESSDATA_ENG);
#else
    return {};
#endif
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

void TstTessdata::languageNamesAreFriendly()
{
    QCOMPARE(mervin::TessdataManager::languageName(QStringLiteral("eng")),
             QStringLiteral("English"));
    QCOMPARE(mervin::TessdataManager::languageName(QStringLiteral("swe")),
             QStringLiteral("Swedish"));
    QCOMPARE(mervin::TessdataManager::languageName(QStringLiteral("unknown_model")),
             QStringLiteral("unknown_model"));
}

// The file every installer seeds into the user's tessdata folder must be data
// Tesseract can actually load. This is the check that would have caught the
// corruption on any machine, with no language installed and no OCR run.
void TstTessdata::shippedEngModelIsLoadable()
{
    const QString eng = shippedEng();
    if (eng.isEmpty() || !QFileInfo::exists(eng))
        QSKIP("resources/tessdata/eng.traineddata is not present in this tree");

    QString err;
    QVERIFY2(TessdataFile::validate(eng, &err), qPrintable(err));
}

// The exact damage that shipped: every em-dash rewritten to a hyphen. The
// container stays structurally plausible - only the duplicated unichar gives it
// away - so this is the case a size or offset check alone would wave through.
void TstTessdata::rejectsEmDashPurgedModel()
{
    const QString eng = shippedEng();
    if (eng.isEmpty() || !QFileInfo::exists(eng))
        QSKIP("resources/tessdata/eng.traineddata is not present in this tree");

    QFile src(eng);
    QVERIFY(src.open(QIODevice::ReadOnly));
    const QByteArray good = src.readAll();
    src.close();

    const QByteArray purged = QByteArray(good).replace("\xE2\x80\x94", "-");
    QVERIFY2(purged != good, "the shipped model has no em-dash to purge - fixture is stale");
    QCOMPARE(purged.size(), good.size() - 4); // two 3-byte sequences to 1 byte each

    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = writeModel(dir.path(), purged);
    QVERIFY(!path.isEmpty());

    QString err;
    QVERIFY2(!TessdataFile::validate(path, &err), "em-dash-purged model was accepted");
    QVERIFY2(err.contains(QStringLiteral("twice")), qPrintable(err));

    // And the same file must be rejected through the by-language entry point the
    // OCR service actually calls.
    QString langErr;
    QVERIFY(!TessdataFile::validateLanguages(dir.path(), {QStringLiteral("eng")}, &langErr));
    QVERIFY(!langErr.isEmpty());
}

// A half-finished copy: offsets point past the end of the file.
void TstTessdata::rejectsTruncatedModel()
{
    const QString eng = shippedEng();
    if (eng.isEmpty() || !QFileInfo::exists(eng))
        QSKIP("resources/tessdata/eng.traineddata is not present in this tree");

    QFile src(eng);
    QVERIFY(src.open(QIODevice::ReadOnly));
    const QByteArray good = src.readAll();
    src.close();

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
