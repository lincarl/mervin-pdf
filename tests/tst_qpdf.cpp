#include "i18n/UiLanguage.h"
#include "io/DestinationConsent.h"
#include "render/Document.h"
#include "render/RenderEngine.h"
#include "security/PageOps.h"
#include "security/QpdfService.h"

#include <qpdf/QPDF.hh>
#include <qpdf/QPDFObjectHandle.hh>
#include <qpdf/QPDFPageDocumentHelper.hh>
#include <qpdf/QPDFPageObjectHelper.hh>
#include <qpdf/QPDFWriter.hh>

#include <QBuffer>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QTemporaryDir>
#include <QTest>

#include <cctype>
#include <functional>
#include <memory>

using mervin::PageOps;
using mervin::QpdfService;

Q_DECLARE_METATYPE(QpdfService::Algorithm)
Q_DECLARE_METATYPE(qpdf_r3_print_e)

namespace {

// Build a valid PDF whose pages have the given MediaBox widths (all 792 tall).
// Distinct widths make each page identifiable after a copy, which is the only
// way to assert that pages came out in the right ORDER - makePdf's pages are all
// identical, so a reversed or sorted implementation passes a count-only check.
void makePdfSized(const QString &path, const QList<int> &widths)
{
    QPDF q;
    q.emptyPDF();
    QPDFPageDocumentHelper dh(q);
    for (int w : widths) {
        QPDFObjectHandle box = QPDFObjectHandle::newArray();
        box.appendItem(QPDFObjectHandle::newInteger(0));
        box.appendItem(QPDFObjectHandle::newInteger(0));
        box.appendItem(QPDFObjectHandle::newInteger(w));
        box.appendItem(QPDFObjectHandle::newInteger(792));
        QPDFObjectHandle page = QPDFObjectHandle::newDictionary();
        page.replaceKey("/Type", QPDFObjectHandle::newName("/Page"));
        page.replaceKey("/MediaBox", box);
        dh.addPage(QPDFPageObjectHelper(q.makeIndirectObject(page)), false);
    }
    const QByteArray outputName = path.toUtf8();
    QPDFWriter writer(q, outputName.constData());
    writer.write();
}

// Build a valid PDF with `n` blank Letter pages.
void makePdf(const QString &path, int n)
{
    makePdfSized(path, QList<int>(n, 612));
}

// The MediaBox widths of `path`, in page order - the fingerprint makePdfSized
// leaves behind.
QList<int> widthsOf(const QString &path)
{
    mervin::RenderEngine engine;
    QString err;
    auto doc = engine.openDocument(path, QString(), &err, nullptr);
    if (!doc)
        return {};
    QList<int> out;
    for (int i = 0; i < doc->pageCount(); ++i)
        out << qRound(doc->pageSize(i).width());
    return out;
}

// Removes /Length from the Encrypt dictionary of a file qpdf encrypted, as some
// writers do. The PDF specification makes it optional unless V is 2 or 3. Blanking
// the entry with spaces keeps every cross-reference offset valid.
bool omitKeyLength(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadWrite))
        return false;
    QByteArray data = file.readAll();
    const QByteArray key("/Standard /Length ");
    const qsizetype at = data.indexOf(key);
    if (at < 0)
        return false;
    qsizetype end = at + key.size();
    while (end < data.size() && std::isdigit(static_cast<unsigned char>(data.at(end))))
        ++end;
    const qsizetype from = at + qsizetype(sizeof("/Standard ") - 1);
    data.replace(from, end - from, QByteArray(end - from, ' '));
    return file.seek(0) && file.write(data) == data.size() && file.flush();
}

// Writes a one-page PDF whose encryption `setUp` chooses on the writer, as another
// program would write it. Password bytes go to qpdf unchanged.
void makeEncrypted(const QString &path, const std::function<void(QPDFWriter &)> &setUp)
{
    QPDF q;
    q.emptyPDF();
    QPDFPageDocumentHelper(q).addPage(QPDFPageObjectHelper(q.makeIndirectObject(
        QPDFObjectHandle::parse("<< /Type /Page /MediaBox [0 0 612 792] /Resources << >> >>"))), false);
    const QByteArray outputName = path.toUtf8();
    QPDFWriter writer(q, outputName.constData());
    setUp(writer);
    writer.write();
}

// Opens `path` with qpdf and the exact password bytes, for checks of what a writer stored.
std::unique_ptr<QPDF> openWithBytes(const QString &path, const QByteArray &password)
{
    auto q = std::make_unique<QPDF>();
    q->processFile(path.toUtf8().constData(), password.constData());
    return q;
}

// Replaces the crypt filter names in a qpdf-written V4 or V5 file with `methods`, as other
// programs choose them. Key derivation ignores these names, so the passwords still open
// the file. The optional /AuthEvent /DocOpen entry makes room, so no offset moves.
void retargetCryptFilters(const QString &path, const QByteArray &methods)
{
    QFile file(path);
    QVERIFY(file.open(QIODevice::ReadOnly));
    QByteArray bytes = file.readAll();
    file.close();
    const QByteArray authEvent(" /AuthEvent /DocOpen");
    const QByteArray filters(" /StmF /StdCF /StrF /StdCF");
    QCOMPARE(bytes.count(authEvent), 1);
    QCOMPARE(bytes.count(filters), 1);
    const qsizetype room = authEvent.size() + filters.size();
    QVERIFY(methods.size() + 1 <= room);
    bytes.replace(authEvent, QByteArray());
    bytes.replace(filters, ' ' + methods + QByteArray(room - methods.size() - 1, ' '));
    QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
    QCOMPARE(file.write(bytes), bytes.size());
}

} // namespace

class TstQpdf : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void init();

    void pageCountRoundTrip();
    void jpegDecodersCoexist();
    void encryptAes256ThenReadInfo();
    void decryptRemovesEncryption();
    void userPasswordRequiredToOpen();
    void permissionsRoundTrip();
    void legacyEncryptionStoresPdfDocPasswords_data();
    void legacyEncryptionStoresPdfDocPasswords();
    void legacyEncryptionRefusesOtherCharacters_data();
    void legacyEncryptionRefusesOtherCharacters();
    void aes256KeepsUnicodePasswords();
    void pdfDocKeyedSourcesOpenWithTheirPassword();
    void utf8KeyedSourcesStillOpen();
    void emptyOwnerPasswordKeepsRestrictions_data();
    void emptyOwnerPasswordKeepsRestrictions();
    void limitedPermissionsAreReported_data();
    void limitedPermissionsAreReported();
    void rc4RevisionsAreReportedAsRc4_data();
    void rc4RevisionsAreReportedAsRc4();
    void cryptFilterNamesTheAlgorithm_data();
    void cryptFilterNamesTheAlgorithm();
    void deletePagesReducesCount();
    void mergeConcatenates();
    void mergeHonoursPerFileRanges();
    void mergeRepeatsAPage();
    void mergeEmptyPagesMeansAll();
    void mergeUsesPerFilePassword();
    void mergeReportsFailedIndex();
    void mergeRejectsOutOfRangeIndex();
    void destinationConsentDistinguishesContentsAndErrors();
    void delayedMergeRequiresFreshConsent_data();
    void delayedMergeRequiresFreshConsent();
    void delayedSecurityRequiresFreshConsent_data();
    void delayedSecurityRequiresFreshConsent();
    void probeDistinguishesLockedFromUnreadable();
    void singleSourceCopiesKeepItsEncryption_data();
    void singleSourceCopiesKeepItsEncryption();
    void singleSourceCopiesKeepOwnerRestrictions();
    void mergeOfSeveralFilesIsNotEncrypted();
    void splitWritesOnePerPage();
    void rotateKeepsCount();
    void rotateAndDeleteKeepEncryptionWithoutKeyLength_data();
    void rotateAndDeleteKeepEncryptionWithoutKeyLength();
    void openEncryptedNeedsPassword();

private:
    QTemporaryDir dir_;
    QString in(const QString &name) const { return dir_.filePath(name); }
};

void TstQpdf::initTestCase()
{
    // The page-count error uses a %n plural, whose English forms come from the
    // English catalog.
    mervin::i18n::apply(QStringLiteral("en"));
}

void TstQpdf::init()
{
    QVERIFY(dir_.isValid());
}

/// Distinguishes absent output, exact contents and failed checks without trusting metadata.
void TstQpdf::destinationConsentDistinguishesContentsAndErrors()
{
    QTemporaryDir folder;
    const QString path = folder.filePath("output.pdf");
    QString error;
    const auto absent = mervin::DestinationConsent::read(path, &error);
    QVERIFY(absent);
    QVERIFY(!absent->exists);
    QFile output(path);
    QVERIFY(output.open(QIODevice::WriteOnly));
    QCOMPARE(output.write("first contents"), qint64(14));
    output.close();
    const auto first = mervin::DestinationConsent::read(path, &error);
    QVERIFY(first && first->exists);
    QVERIFY(*first != *absent);
    QVERIFY(output.open(QIODevice::WriteOnly));
    QCOMPARE(output.write("other contents"), qint64(14));
    output.close();
    const auto other = mervin::DestinationConsent::read(path, &error);
    QVERIFY(other);
    QVERIFY(*other != *first);
    QVERIFY(output.open(QIODevice::WriteOnly));
    QCOMPARE(output.write("first contents"), qint64(14));
    output.close();
    QCOMPARE(mervin::DestinationConsent::read(path, &error), first);
    QVERIFY(!mervin::DestinationConsent::read(folder.path(), &error));
    QVERIFY(!error.isEmpty());
#ifdef Q_OS_UNIX
    const QString dangling = folder.filePath("dangling.pdf");
    QVERIFY(QFile::link(folder.filePath("missing-target.pdf"), dangling));
    error.clear();
    QVERIFY(!mervin::DestinationConsent::read(dangling, &error));
    QVERIFY(!error.isEmpty());
#endif
}

/// Covers an output initially absent and an existing output already approved for replacement.
void TstQpdf::delayedMergeRequiresFreshConsent_data()
{
    QTest::addColumn<bool>("existing");
    QTest::newRow("absent output") << false;
    QTest::newRow("confirmed output") << true;
}

/// Preserves a completed winner when an older accepted request starts writing later.
void TstQpdf::delayedMergeRequiresFreshConsent()
{
    QFETCH(bool, existing);
    QTemporaryDir folder;
    const QString source = folder.filePath("source.pdf");
    const QString output = folder.filePath("output.pdf");
    makePdf(source, 3);
    if (existing)
        makePdf(output, 3);
    const auto approved = mervin::DestinationConsent::read(output);
    QVERIFY(approved);
    QCOMPARE(approved->exists, existing);
    QCOMPARE(PageOps::merge(QList<PageOps::MergeInput>{{source, {0}, {}}}, output),
             PageOps::Status::Ok);
    QFile winnerFile(output);
    QVERIFY(winnerFile.open(QIODevice::ReadOnly));
    const QByteArray winner = winnerFile.readAll();
    winnerFile.close();
    int validations = 0;
    const QList<PageOps::MergeInput> later{{source, {0, 1}, {}}};
    QCOMPARE(PageOps::merge(later, output, nullptr, nullptr, [&] {
        ++validations;
        return mervin::DestinationConsent::read(output) == approved;
    }), PageOps::Status::Failed);
    QCOMPARE(validations, 1);
    QVERIFY(winnerFile.open(QIODevice::ReadOnly));
    QCOMPARE(winnerFile.readAll(), winner);
    winnerFile.close();
    const auto confirmed = mervin::DestinationConsent::read(output);
    QVERIFY(confirmed);
    QCOMPARE(PageOps::merge(later, output, nullptr, nullptr, [&] {
        ++validations;
        return mervin::DestinationConsent::read(output) == confirmed;
    }), PageOps::Status::Ok);
    QCOMPARE(validations, 2);
    QCOMPARE(PageOps::pageCount(output), 2);
}

/// Covers both Security writers with absent and previously approved destinations.
void TstQpdf::delayedSecurityRequiresFreshConsent_data()
{
    QTest::addColumn<bool>("encrypt");
    QTest::addColumn<bool>("existing");
    QTest::newRow("encrypt absent") << true << false;
    QTest::newRow("encrypt existing") << true << true;
    QTest::newRow("decrypt absent") << false << false;
    QTest::newRow("decrypt existing") << false << true;
}

/// Both Qpdf writers preserve the winner until a new request approves its exact bytes.
void TstQpdf::delayedSecurityRequiresFreshConsent()
{
    QFETCH(bool, encrypt);
    QFETCH(bool, existing);
    QTemporaryDir folder;
    const QString source = folder.filePath("source.pdf");
    const QString encrypted = folder.filePath("encrypted.pdf");
    const QString output = folder.filePath("output.pdf");
    const QString password = QStringLiteral("source password");
    makePdf(source, 3);
    QpdfService service;
    QCOMPARE(service.encrypt(source, encrypted, {}, password, {}, QpdfService::Algorithm::AES256, {}),
             QpdfService::Status::Ok);
    if (existing)
        makePdf(output, 3);
    const auto approved = mervin::DestinationConsent::read(output);
    QVERIFY(approved);
    QCOMPARE(approved->exists, existing);
    QCOMPARE(PageOps::merge(QList<PageOps::MergeInput>{{source, {0}, {}}}, output),
             PageOps::Status::Ok);
    QFile winnerFile(output);
    QVERIFY(winnerFile.open(QIODevice::ReadOnly));
    const QByteArray winner = winnerFile.readAll();
    winnerFile.close();
    int validations = 0;
    auto expected = approved;
    const auto validate = [&] {
        ++validations;
        return mervin::DestinationConsent::read(output) == expected;
    };
    const auto write = [&] {
        return encrypt
            ? service.encrypt(source, output, {}, password, {}, QpdfService::Algorithm::AES256,
                              {}, nullptr, validate)
            : service.decrypt(encrypted, output, password, nullptr, validate);
    };
    QCOMPARE(write(), QpdfService::Status::Failed);
    QCOMPARE(validations, 1);
    QVERIFY(winnerFile.open(QIODevice::ReadOnly));
    QCOMPARE(winnerFile.readAll(), winner);
    winnerFile.close();
    expected = mervin::DestinationConsent::read(output);
    QVERIFY(expected);
    QCOMPARE(write(), QpdfService::Status::Ok);
    QCOMPARE(validations, 2);
    QpdfService::Info info;
    QCOMPARE(service.readInfo(output, password, info), QpdfService::Status::Ok);
    QCOMPARE(info.encrypted, encrypt);
    QCOMPARE(PageOps::pageCount(output, password), 3);
}

void TstQpdf::pageCountRoundTrip()
{
    const QString p = in(QStringLiteral("count.pdf"));
    makePdf(p, 4);
    QCOMPARE(PageOps::pageCount(p), 4);
}

void TstQpdf::jpegDecodersCoexist()
{
    // qpdf's libjpeg and MuPDF's bundled JPEG 10 have different ABIs. Windows
    // links both into the executable; on Linux the shared libqpdf and Qt's JPEG
    // plugin bind to MuPDF's copy if the executable exports it. Encode with Qt,
    // then decode with qpdf and MuPDF in one process to detect symbol sharing.
    QImage source(16, 16, QImage::Format_RGB888);
    const QColor left(200, 32, 24);
    const QColor right(24, 64, 200);
    for (int y = 0; y < source.height(); ++y) {
        for (int x = 0; x < source.width(); ++x)
            source.setPixelColor(x, y, x < 8 ? left : right);
    }
    QByteArray jpeg;
    QBuffer buffer(&jpeg);
    QVERIFY(buffer.open(QIODevice::WriteOnly));
    QVERIFY(source.save(&buffer, "JPEG", 100));

    QPDF pdf;
    pdf.emptyPDF();
    auto image = pdf.newStream(std::string(jpeg.constData(), jpeg.size()));
    auto dictionary = image.getDict();
    dictionary.replaceKey("/Type", QPDFObjectHandle::newName("/XObject"));
    dictionary.replaceKey("/Subtype", QPDFObjectHandle::newName("/Image"));
    dictionary.replaceKey("/Width", QPDFObjectHandle::newInteger(16));
    dictionary.replaceKey("/Height", QPDFObjectHandle::newInteger(16));
    dictionary.replaceKey("/BitsPerComponent", QPDFObjectHandle::newInteger(8));
    dictionary.replaceKey("/ColorSpace", QPDFObjectHandle::newName("/DeviceRGB"));
    dictionary.replaceKey("/Filter", QPDFObjectHandle::newName("/DCTDecode"));

    const auto decoded = image.getStreamData(qpdf_dl_all);
    QCOMPARE(decoded->getSize(), size_t(16 * 16 * 3));
    const QImage qpdfPixels(decoded->getBuffer(), 16, 16, 16 * 3, QImage::Format_RGB888);
    for (const int x : {4, 12}) {
        const QColor expected = x < 8 ? left : right;
        const QColor actual = qpdfPixels.pixelColor(x, 8);
        QVERIFY(qAbs(actual.red() - expected.red()) <= 3);
        QVERIFY(qAbs(actual.green() - expected.green()) <= 3);
        QVERIFY(qAbs(actual.blue() - expected.blue()) <= 3);
    }

    auto images = QPDFObjectHandle::newDictionary();
    images.replaceKey("/Image", image);
    auto resources = QPDFObjectHandle::newDictionary();
    resources.replaceKey("/XObject", images);
    auto page = QPDFObjectHandle::parse("<< /Type /Page /MediaBox [0 0 16 16] >>");
    page.replaceKey("/Resources", resources);
    page.replaceKey("/Contents", pdf.newStream("q 16 0 0 16 0 0 cm /Image Do Q"));
    QPDFPageDocumentHelper(pdf).addPage(QPDFPageObjectHelper(pdf.makeIndirectObject(page)), false);
    const QString path = in(QStringLiteral("jpeg-codecs.pdf"));
    const QByteArray name = path.toUtf8();
    QPDFWriter writer(pdf, name.constData());
    writer.setStreamDataMode(qpdf_s_preserve);
    writer.setDecodeLevel(qpdf_dl_none);
    writer.write();

    mervin::RenderEngine engine;
    QString error;
    auto document = engine.openDocument(path, {}, &error);
    QVERIFY2(document, qPrintable(error));
    const QImage rendered = engine.renderPageImage(document.get(), 0, 1, 0);
    QCOMPARE(rendered.size(), source.size());
    for (const int x : {4, 12}) {
        const QColor expected = x < 8 ? left : right;
        const QColor actual = rendered.pixelColor(x, 8);
        QVERIFY(qAbs(actual.red() - expected.red()) <= 3);
        QVERIFY(qAbs(actual.green() - expected.green()) <= 3);
        QVERIFY(qAbs(actual.blue() - expected.blue()) <= 3);
    }
}

void TstQpdf::encryptAes256ThenReadInfo()
{
    const QString src = in(QStringLiteral("src.pdf"));
    const QString enc = in(QStringLiteral("enc.pdf"));
    makePdf(src, 2);

    QpdfService svc;
    QString err;
    const auto st = svc.encrypt(src, enc, QString(), QStringLiteral("open"), QStringLiteral("owner"),
                                QpdfService::Algorithm::AES256, {}, &err);
    QCOMPARE(st, QpdfService::Status::Ok);

    QpdfService::Info info;
    QCOMPARE(svc.readInfo(enc, QStringLiteral("open"), info, &err), QpdfService::Status::Ok);
    QVERIFY(info.encrypted);
    QCOMPARE(info.algorithm, QStringLiteral("AES-256"));
    QCOMPARE(info.keyLengthBits, 256);
}

void TstQpdf::decryptRemovesEncryption()
{
    const QString src = in(QStringLiteral("d_src.pdf"));
    const QString enc = in(QStringLiteral("d_enc.pdf"));
    const QString dec = in(QStringLiteral("d_dec.pdf"));
    makePdf(src, 2);

    QpdfService svc;
    QVERIFY(svc.encrypt(src, enc, QString(), QStringLiteral("pw"), QString(),
                        QpdfService::Algorithm::AES256, {}, nullptr)
            == QpdfService::Status::Ok);
    QVERIFY(svc.decrypt(enc, dec, QStringLiteral("pw"), nullptr) == QpdfService::Status::Ok);

    QpdfService::Info info;
    QCOMPARE(svc.readInfo(dec, QString(), info, nullptr), QpdfService::Status::Ok);
    QVERIFY(!info.encrypted);
}

void TstQpdf::userPasswordRequiredToOpen()
{
    const QString src = in(QStringLiteral("u_src.pdf"));
    const QString enc = in(QStringLiteral("u_enc.pdf"));
    makePdf(src, 1);

    QpdfService svc;
    QVERIFY(svc.encrypt(src, enc, QString(), QStringLiteral("secret"), QString(),
                        QpdfService::Algorithm::AES256, {}, nullptr)
            == QpdfService::Status::Ok);

    QpdfService::Info info;
    QCOMPARE(svc.readInfo(enc, QString(), info, nullptr), QpdfService::Status::NeedsPassword);
    QCOMPARE(svc.readInfo(enc, QStringLiteral("secret"), info, nullptr), QpdfService::Status::Ok);
}

void TstQpdf::permissionsRoundTrip()
{
    const QString src = in(QStringLiteral("p_src.pdf"));
    const QString enc = in(QStringLiteral("p_enc.pdf"));
    makePdf(src, 1);

    QpdfService::Permissions perms; // owner-only encryption: no user password
    perms.canPrint = false;
    perms.canCopy = false;
    perms.canModify = false;

    QpdfService svc;
    QVERIFY(svc.encrypt(src, enc, QString(), QString(), QStringLiteral("owner"),
                        QpdfService::Algorithm::AES256, perms, nullptr)
            == QpdfService::Status::Ok);

    QpdfService::Info info;
    QCOMPARE(svc.readInfo(enc, QString(), info, nullptr), QpdfService::Status::Ok);
    QVERIFY(info.encrypted);
    QVERIFY(!info.permissions.canPrint);
    QVERIFY(!info.permissions.canCopy);
    QVERIFY(!info.permissions.canModify);
    QVERIFY(info.permissions.canAnnotate);
    // Mervin's writer allows printing fully or not at all, and assembly with the rest.
    QVERIFY(!info.lowResolutionPrintOnly);
    QVERIFY(!info.pageAssemblyOnly);
}

void TstQpdf::legacyEncryptionStoresPdfDocPasswords_data()
{
    QTest::addColumn<QpdfService::Algorithm>("algorithm");
    QTest::newRow("AES-128") << QpdfService::Algorithm::AES128;
    QTest::newRow("RC4-128") << QpdfService::Algorithm::RC4_128;
}

/// R4 keys use PDFDocEncoding, so MuPDF and qpdf both open the result with the typed text.
void TstQpdf::legacyEncryptionStoresPdfDocPasswords()
{
    QFETCH(QpdfService::Algorithm, algorithm);
    const QString src = in(QStringLiteral("l_src.pdf"));
    const QString enc = in(QStringLiteral("l_enc.pdf"));
    makePdf(src, 2);
    const QString password = QStringLiteral("Müller");
    QString err;
    QCOMPARE(QpdfService().encrypt(src, enc, {}, password, {}, algorithm, {}, &err),
             QpdfService::Status::Ok);

    mervin::RenderEngine engine;
    bool needsPassword = false;
    auto doc = engine.openDocument(enc, password, &err, &needsPassword);
    QVERIFY2(doc, qPrintable(err));
    QCOMPARE(doc->pageCount(), 2);
    int count = 0;
    QCOMPARE(PageOps::probe(enc, &count, password, &err), PageOps::Status::Ok);
    QCOMPARE(count, 2);
    // The stored key is the PDFDocEncoding of the text, as other readers expect.
    QVERIFY(openWithBytes(enc, QByteArray("M\xFCller"))->userPasswordMatched());
}

void TstQpdf::legacyEncryptionRefusesOtherCharacters_data()
{
    QTest::addColumn<QpdfService::Algorithm>("algorithm");
    QTest::addColumn<QString>("user");
    QTest::addColumn<QString>("owner");
    QTest::newRow("AES-128 open password")
        << QpdfService::Algorithm::AES128 << QStringLiteral("密码") << QString();
    QTest::newRow("RC4-128 owner password")
        << QpdfService::Algorithm::RC4_128 << QStringLiteral("open") << QStringLiteral("密码");
}

/// PDFDocEncoding has no Chinese characters, so legacy encryption writes nothing.
void TstQpdf::legacyEncryptionRefusesOtherCharacters()
{
    QFETCH(QpdfService::Algorithm, algorithm);
    QFETCH(QString, user);
    QFETCH(QString, owner);
    const QString src = in(QStringLiteral("x_src.pdf"));
    const QString enc = in(QStringLiteral("x_enc.pdf"));
    makePdf(src, 1);
    QString err;
    QCOMPARE(QpdfService().encrypt(src, enc, {}, user, owner, algorithm, {}, &err),
             QpdfService::Status::Failed);
    QCOMPARE(err, QpdfService::tr("A password contains characters that AES-128 and RC4-128 "
                                  "cannot use. Choose AES-256."));
    // Not even a temporary file.
    QCOMPARE(QDir(dir_.path()).entryList({QStringLiteral("x_enc*")}, QDir::Files | QDir::Hidden),
             QStringList());
}

/// AES-256 stores UTF-8, which MuPDF and qpdf both accept for any character.
void TstQpdf::aes256KeepsUnicodePasswords()
{
    const QString src = in(QStringLiteral("u8_src.pdf"));
    const QString enc = in(QStringLiteral("u8_enc.pdf"));
    makePdf(src, 1);
    const QString password = QStringLiteral("密码");
    QString err;
    QCOMPARE(QpdfService().encrypt(src, enc, {}, password, {}, QpdfService::Algorithm::AES256, {},
                                   &err),
             QpdfService::Status::Ok);
    mervin::RenderEngine engine;
    auto doc = engine.openDocument(enc, password, &err, nullptr);
    QVERIFY2(doc, qPrintable(err));
    QCOMPARE(PageOps::probe(enc, nullptr, password, &err), PageOps::Status::Ok);
    QVERIFY(openWithBytes(enc, password.toUtf8())->userPasswordMatched());
}

/// A third-party R4 file keyed with PDFDocEncoding opens with the typed text in every qpdf
/// reader, and its one-file copy keeps that key.
void TstQpdf::pdfDocKeyedSourcesOpenWithTheirPassword()
{
    const QString enc = in(QStringLiteral("pd_enc.pdf"));
    const QString out = in(QStringLiteral("pd_out.pdf"));
    makeEncrypted(enc, [](QPDFWriter &writer) {
        writer.setR4EncryptionParametersInsecure("caf\xE9", "caf\xE9", true, true, true, true, true,
                                                 true, qpdf_r3p_full, true, true);
    });
    const QString password = QStringLiteral("café");
    QString err;
    int count = 0;
    QCOMPARE(PageOps::probe(enc, &count, password, &err), PageOps::Status::Ok);
    QCOMPARE(count, 1);
    QpdfService::Info info;
    QCOMPARE(QpdfService().readInfo(enc, password, info, &err), QpdfService::Status::Ok);
    QCOMPARE(info.algorithm, QStringLiteral("AES-128"));
    QCOMPARE(PageOps::merge(QList<PageOps::MergeInput>{{enc, {}, password}}, out, &err),
             PageOps::Status::Ok);
    // The copy of one file keeps its encryption, keyed with the same PDFDocEncoding bytes.
    QCOMPARE(PageOps::probe(out, &count, {}, &err), PageOps::Status::NeedsPassword);
    QCOMPARE(PageOps::probe(out, &count, password, &err), PageOps::Status::Ok);
    QCOMPARE(count, 1);
    QVERIFY(openWithBytes(out, QByteArray("caf\xE9"))->userPasswordMatched());
    // A wrong password still asks for the right one.
    QCOMPARE(PageOps::probe(enc, &count, QStringLiteral("cafe"), &err),
             PageOps::Status::NeedsPassword);
}

/// R4 files keyed with UTF-8, as earlier versions of Mervin wrote them, stay readable.
void TstQpdf::utf8KeyedSourcesStillOpen()
{
    const QString enc = in(QStringLiteral("u8k_enc.pdf"));
    const QByteArray utf8 = QStringLiteral("Müller").toUtf8();
    makeEncrypted(enc, [&utf8](QPDFWriter &writer) {
        writer.setR4EncryptionParametersInsecure(utf8.constData(), utf8.constData(), true, true,
                                                 true, true, true, true, qpdf_r3p_full, true, true);
    });
    QString err;
    int count = 0;
    QCOMPARE(PageOps::probe(enc, &count, QStringLiteral("Müller"), &err), PageOps::Status::Ok);
    QCOMPARE(count, 1);
}

void TstQpdf::emptyOwnerPasswordKeepsRestrictions_data()
{
    QTest::addColumn<QpdfService::Algorithm>("algorithm");
    QTest::addColumn<QString>("user");
    QTest::addColumn<bool>("restricted");
    QTest::newRow("AES-128 restricted") << QpdfService::Algorithm::AES128 << QStringLiteral("x") << true;
    QTest::newRow("AES-256 restricted") << QpdfService::Algorithm::AES256 << QStringLiteral("x") << true;
    QTest::newRow("no open password, restricted")
        << QpdfService::Algorithm::AES256 << QString() << true;
    QTest::newRow("everything allowed") << QpdfService::Algorithm::AES128 << QStringLiteral("x") << false;
}

/// Restrictions get an owner password that the open password does not match.
void TstQpdf::emptyOwnerPasswordKeepsRestrictions()
{
    QFETCH(QpdfService::Algorithm, algorithm);
    QFETCH(QString, user);
    QFETCH(bool, restricted);
    const QString src = in(QStringLiteral("o_src.pdf"));
    const QString enc = in(QStringLiteral("o_enc.pdf"));
    const QString dec = in(QStringLiteral("o_dec.pdf"));
    makePdf(src, 1);
    QpdfService::Permissions perms;
    perms.canPrint = !restricted;
    QpdfService svc;
    QString err;
    QCOMPARE(svc.encrypt(src, enc, {}, user, {}, algorithm, perms, &err), QpdfService::Status::Ok);

    const auto q = openWithBytes(enc, user.toUtf8());
    QVERIFY(q->userPasswordMatched());
    // Owner rights ignore the flags, so restricted files must not grant them.
    // With everything allowed the open password stays the owner password, so other
    // programs still let its holder change the security settings.
    QCOMPARE(q->ownerPasswordMatched(), !restricted);
    QCOMPARE(q->allowPrintLowRes(), !restricted);

    // Strip Owner Restrictions needs only the open password.
    QCOMPARE(svc.decrypt(enc, dec, user, &err), QpdfService::Status::Ok);
    QpdfService::Info info;
    QCOMPARE(svc.readInfo(dec, {}, info, &err), QpdfService::Status::Ok);
    QVERIFY(!info.encrypted);
}

void TstQpdf::limitedPermissionsAreReported_data()
{
    QTest::addColumn<qpdf_r3_print_e>("print");
    QTest::addColumn<bool>("assemble");
    QTest::addColumn<bool>("modifyOther");
    QTest::addColumn<bool>("canPrint");
    QTest::addColumn<bool>("canModify");
    QTest::addColumn<bool>("lowResolution");
    QTest::addColumn<bool>("pageAssembly");
    QTest::newRow("low resolution and assembly only")
        << qpdf_r3p_low << true << false << false << false << true << true;
    QTest::newRow("everything") << qpdf_r3p_full << true << true << true << true << false << false;
    QTest::newRow("nothing") << qpdf_r3p_none << false << false << false << false << false << false;
    // Bit 4 alone already permits assembly in the PDF format.
    QTest::newRow("modify without bit 11")
        << qpdf_r3p_none << false << true << false << true << false << false;
}

/// Printing and modifying read as allowed only when every kind is allowed.
void TstQpdf::limitedPermissionsAreReported()
{
    QFETCH(qpdf_r3_print_e, print);
    QFETCH(bool, assemble);
    QFETCH(bool, modifyOther);
    QFETCH(bool, canPrint);
    QFETCH(bool, canModify);
    QFETCH(bool, lowResolution);
    QFETCH(bool, pageAssembly);
    const QString enc = in(QStringLiteral("lp_enc.pdf"));
    makeEncrypted(enc, [&](QPDFWriter &writer) {
        writer.setR4EncryptionParametersInsecure("", "owner", true, false, assemble, false, false,
                                                 modifyOther, print, true, true);
    });
    QpdfService::Info info;
    QString err;
    QCOMPARE(QpdfService().readInfo(enc, {}, info, &err), QpdfService::Status::Ok);
    QCOMPARE(info.permissions.canPrint, canPrint);
    QCOMPARE(info.permissions.canModify, canModify);
    QCOMPARE(info.lowResolutionPrintOnly, lowResolution);
    QCOMPARE(info.pageAssemblyOnly, pageAssembly);
}

void TstQpdf::rc4RevisionsAreReportedAsRc4_data()
{
    QTest::addColumn<int>("revision");
    QTest::addColumn<int>("bits");
    QTest::newRow("R2") << 2 << 40;
    QTest::newRow("R3") << 3 << 128;
}

/// qpdf reports no method for V1 and V2 files, which always use RC4.
void TstQpdf::rc4RevisionsAreReportedAsRc4()
{
    QFETCH(int, revision);
    QFETCH(int, bits);
    const QString enc = in(QStringLiteral("rc4_enc.pdf"));
    makeEncrypted(enc, [revision](QPDFWriter &writer) {
        if (revision == 2)
            writer.setR2EncryptionParametersInsecure("open", "owner", true, true, true, true);
        else
            writer.setR3EncryptionParametersInsecure("open", "owner", true, true, true, true, true,
                                                     true, qpdf_r3p_full);
    });
    QpdfService::Info info;
    QString err;
    QCOMPARE(QpdfService().readInfo(enc, QStringLiteral("open"), info, &err),
             QpdfService::Status::Ok);
    QVERIFY(info.encrypted);
    QCOMPARE(info.revision, revision);
    QCOMPARE(info.algorithm, QStringLiteral("RC4"));
    QCOMPARE(info.keyLengthBits, bits);
}

void TstQpdf::cryptFilterNamesTheAlgorithm_data()
{
    QTest::addColumn<bool>("aes256");
    QTest::addColumn<QByteArray>("methods");
    QTest::addColumn<QString>("algorithm");
    QTest::addColumn<int>("bits");
    // Acrobat encrypts only attachments this way. Streams and strings stay plain.
    QTest::newRow("attachments only")
        << true << QByteArray("/StmF /Identity /StrF /Identity /EFF /StdCF")
        << QStringLiteral("AES-256") << 256;
    // The stream filter names the algorithm when attachments use another.
    QTest::newRow("streams before attachments")
        << false << QByteArray("/StmF /StdCF /StrF /StdCF /EFF /Identity")
        << QStringLiteral("AES-128") << 128;
}

/// V4 and V5 files name the first crypt filter in use, in stream, string, attachment order.
void TstQpdf::cryptFilterNamesTheAlgorithm()
{
    QFETCH(bool, aes256);
    QFETCH(QByteArray, methods);
    QFETCH(QString, algorithm);
    QFETCH(int, bits);
    const QString enc = in(QStringLiteral("cf_enc.pdf"));
    makeEncrypted(enc, [aes256](QPDFWriter &writer) {
        writer.setObjectStreamMode(qpdf_o_disable);
        if (aes256) {
            writer.setR6EncryptionParameters("open", "owner", true, true, true, true, true, true,
                                             qpdf_r3p_full, true);
        } else {
            writer.setR4EncryptionParametersInsecure("open", "owner", true, true, true, true, true,
                                                     true, qpdf_r3p_full, true, true);
        }
    });
    retargetCryptFilters(enc, methods);
    QpdfService::Info info;
    QString err;
    QCOMPARE(QpdfService().readInfo(enc, QStringLiteral("open"), info, &err),
             QpdfService::Status::Ok);
    QVERIFY(info.encrypted);
    QCOMPARE(info.algorithm, algorithm);
    QCOMPARE(info.keyLengthBits, bits);
}

void TstQpdf::deletePagesReducesCount()
{
    const QString src = in(QStringLiteral("del_src.pdf"));
    const QString out = in(QStringLiteral("del_out.pdf"));
    makePdf(src, 5);
    QCOMPARE(PageOps::deletePages(src, out, {1, 3}), PageOps::Status::Ok);
    QCOMPARE(PageOps::pageCount(out), 3);
}

void TstQpdf::mergeConcatenates()
{
    const QString a = in(QStringLiteral("m_a.pdf"));
    const QString b = in(QStringLiteral("m_b.pdf"));
    const QString out = in(QStringLiteral("m_out.pdf"));
    makePdfSized(a, {100, 200});
    makePdfSized(b, {300, 400, 500});
    QCOMPARE(PageOps::merge({a, b}, out), PageOps::Status::Ok);
    QCOMPARE(widthsOf(out), QList<int>({100, 200, 300, 400, 500}));
}

void TstQpdf::mergeHonoursPerFileRanges()
{
    const QString a = in(QStringLiteral("mr_a.pdf"));
    const QString b = in(QStringLiteral("mr_b.pdf"));
    const QString out = in(QStringLiteral("mr_out.pdf"));
    makePdfSized(a, {100, 200, 300});
    makePdfSized(b, {400, 500, 600});
    // b's pages 3 and 1, then a's page 2 - an order no sort produces by accident.
    QCOMPARE(PageOps::merge({{b, {2, 0}, {}}, {a, {1}, {}}}, out), PageOps::Status::Ok);
    QCOMPARE(widthsOf(out), QList<int>({600, 400, 200}));
}

void TstQpdf::mergeRepeatsAPage()
{
    const QString a = in(QStringLiteral("mrep_a.pdf"));
    const QString out = in(QStringLiteral("mrep_out.pdf"));
    makePdfSized(a, {100, 200, 300});
    // The same file as two inputs, and the same page twice within one input:
    // "cover page repeated at the back" is the case the dialog's Duplicate exists
    // for, and it must not be silently deduplicated.
    QCOMPARE(PageOps::merge({{a, {0, 0}, {}}, {a, {2}, {}}}, out), PageOps::Status::Ok);
    QCOMPARE(widthsOf(out), QList<int>({100, 100, 300}));
}

void TstQpdf::mergeEmptyPagesMeansAll()
{
    const QString a = in(QStringLiteral("me_a.pdf"));
    const QString b = in(QStringLiteral("me_b.pdf"));
    const QString out = in(QStringLiteral("me_out.pdf"));
    makePdfSized(a, {100, 200});
    makePdfSized(b, {300});
    QCOMPARE(PageOps::merge({{a, {}, {}}, {b, {}, {}}}, out), PageOps::Status::Ok);
    QCOMPARE(widthsOf(out), QList<int>({100, 200, 300}));
}

void TstQpdf::mergeUsesPerFilePassword()
{
    const QString src = in(QStringLiteral("mp_src.pdf"));
    const QString enc = in(QStringLiteral("mp_enc.pdf"));
    const QString plain = in(QStringLiteral("mp_plain.pdf"));
    const QString out = in(QStringLiteral("mp_out.pdf"));
    makePdfSized(src, {100, 200});
    makePdfSized(plain, {300});

    QpdfService svc;
    QVERIFY(svc.encrypt(src, enc, QString(), QStringLiteral("secret"), QString(),
                        QpdfService::Algorithm::AES256, {}, nullptr)
            == QpdfService::Status::Ok);

    // No password: refused, and the offending input is named by index.
    QString err;
    int failed = -1;
    QCOMPARE(PageOps::merge({{plain, {}, {}}, {enc, {}, {}}}, out, &err, &failed),
             PageOps::Status::NeedsPassword);
    QCOMPARE(failed, 1);

    // With it: merges, in order.
    QCOMPARE(PageOps::merge({{plain, {}, {}}, {enc, {1}, QStringLiteral("secret")}}, out, &err,
                            &failed),
             PageOps::Status::Ok);
    QCOMPARE(widthsOf(out), QList<int>({300, 200}));
    QCOMPARE(failed, -1);
}

void TstQpdf::mergeReportsFailedIndex()
{
    const QString a = in(QStringLiteral("mf_a.pdf"));
    const QString out = in(QStringLiteral("mf_out.pdf"));
    makePdf(a, 1);
    QString err;
    int failed = -1;
    QCOMPARE(PageOps::merge({{a, {}, {}}, {in(QStringLiteral("nope.pdf")), {}, {}}, {a, {}, {}}},
                            out, &err, &failed),
             PageOps::Status::Failed);
    QCOMPARE(failed, 1);
    QVERIFY(!err.isEmpty());
}

void TstQpdf::mergeRejectsOutOfRangeIndex()
{
    // An out-of-range index fails rather than being skipped. A page count that
    // went stale between the dialog's probe and this write would otherwise write a
    // shorter document than the dialog promised and still report success.
    const QString a = in(QStringLiteral("mo_a.pdf"));
    const QString out = in(QStringLiteral("mo_out.pdf"));
    makePdf(a, 2);
    QString err;
    int failed = -1;
    QCOMPARE(PageOps::merge({{a, {0, 9}, {}}}, out, &err, &failed), PageOps::Status::Failed);
    QCOMPARE(failed, 0);
    // Names the 1-based page, and the count reads as English, not "2 page(s)".
    QCOMPARE(err, QStringLiteral("It has 2 pages; page 10 does not exist."));

    const QString single = in(QStringLiteral("mo_single.pdf"));
    makePdf(single, 1);
    QCOMPARE(PageOps::merge({{single, {2}, {}}}, out, &err, &failed), PageOps::Status::Failed);
    QCOMPARE(err, QStringLiteral("It has 1 page; page 3 does not exist."));
}

void TstQpdf::probeDistinguishesLockedFromUnreadable()
{
    const QString src = in(QStringLiteral("pr_src.pdf"));
    const QString enc = in(QStringLiteral("pr_enc.pdf"));
    makePdf(src, 4);
    QpdfService svc;
    QVERIFY(svc.encrypt(src, enc, QString(), QStringLiteral("secret"), QString(),
                        QpdfService::Algorithm::AES256, {}, nullptr)
            == QpdfService::Status::Ok);

    int n = -1;
    QCOMPARE(PageOps::probe(src, &n), PageOps::Status::Ok);
    QCOMPARE(n, 4);
    QCOMPARE(PageOps::probe(enc, &n), PageOps::Status::NeedsPassword);
    QCOMPARE(PageOps::probe(enc, &n, QStringLiteral("secret")), PageOps::Status::Ok);
    QCOMPARE(n, 4);
    QCOMPARE(PageOps::probe(in(QStringLiteral("absent.pdf")), &n), PageOps::Status::Failed);
}

/// Covers each algorithm Mervin writes, V4 and V5 files without the optional /Length,
/// and the revision 3 RC4 files of older writers.
void TstQpdf::singleSourceCopiesKeepItsEncryption_data()
{
    QTest::addColumn<QpdfService::Algorithm>("algorithm");
    QTest::addColumn<bool>("withoutLength");
    QTest::addColumn<QString>("label");
    QTest::addColumn<int>("bits");
    QTest::addColumn<int>("revision");
    QTest::addColumn<QString>("password");
    // The PDFDocEncoding bytes the copies must be keyed with, when they differ from UTF-8.
    QTest::addColumn<QByteArray>("pdfDocKey");
    const QString secret = QStringLiteral("secret");
    QTest::newRow("AES-256")
        << QpdfService::Algorithm::AES256 << false << QStringLiteral("AES-256") << 256 << 6
        << secret << QByteArray();
    QTest::newRow("AES-128")
        << QpdfService::Algorithm::AES128 << false << QStringLiteral("AES-128") << 128 << 4
        << secret << QByteArray();
    // qpdf writes every V4 copy with AES, as Acrobat does.
    QTest::newRow("RC4-128")
        << QpdfService::Algorithm::RC4_128 << false << QStringLiteral("AES-128") << 128 << 4
        << secret << QByteArray();
    QTest::newRow("AES-128 without /Length")
        << QpdfService::Algorithm::AES128 << true << QStringLiteral("AES-128") << 128 << 4
        << secret << QByteArray();
    QTest::newRow("AES-256 without /Length")
        << QpdfService::Algorithm::AES256 << true << QStringLiteral("AES-256") << 256 << 6
        << secret << QByteArray();
    // Revision 3 keeps RC4. An empty label expects the source's own label.
    QTest::newRow("RC4-128 revision 3")
        << QpdfService::Algorithm::RC4_128 << false << QString() << 128 << 3 << secret
        << QByteArray();
    // AES-128 and RC4-128 key the source with the PDFDocEncoding form of the password,
    // and the copies keep that key.
    QTest::newRow("AES-128 with a Western European password")
        << QpdfService::Algorithm::AES128 << false << QStringLiteral("AES-128") << 128 << 4
        << QStringLiteral("Müller") << QByteArray("M\xFCller");
    QTest::newRow("RC4-128 with a Western European password")
        << QpdfService::Algorithm::RC4_128 << false << QStringLiteral("AES-128") << 128 << 4
        << QStringLiteral("Müller") << QByteArray("M\xFCller");
}

/// Extract, a merge of one file's ranges and Split open only with the source's passwords.
void TstQpdf::singleSourceCopiesKeepItsEncryption()
{
    QFETCH(QpdfService::Algorithm, algorithm);
    QFETCH(bool, withoutLength);
    QFETCH(QString, label);
    QFETCH(int, bits);
    QFETCH(int, revision);
    QFETCH(QString, password);
    QFETCH(QByteArray, pdfDocKey);
    QTemporaryDir files;
    QVERIFY(files.isValid());
    const QString src = files.filePath(QStringLiteral("src.pdf"));
    const QString enc = files.filePath(QStringLiteral("enc.pdf"));
    makePdfSized(src, {100, 200, 300});
    QpdfService svc;
    if (revision == 3) {
        // Mervin does not write revision 3, so qpdf builds this source directly.
        QPDF pdf;
        pdf.processFile(src.toUtf8().constData());
        const QByteArray name = enc.toUtf8();
        QPDFWriter writer(pdf, name.constData());
        const QByteArray user = password.toUtf8(); // ASCII, the same in every encoding
        writer.setR3EncryptionParametersInsecure(user.constData(), "owner", true, true, true, true,
                                                 true, true, qpdf_r3p_full);
        writer.write();
    } else {
        QCOMPARE(svc.encrypt(src, enc, QString(), password,
                             QStringLiteral("owner"), algorithm, {}, nullptr),
                 QpdfService::Status::Ok);
    }
    QString err;
    if (label.isEmpty()) {
        QpdfService::Info source;
        QCOMPARE(svc.readInfo(enc, password, source, &err),
                 QpdfService::Status::Ok);
        QCOMPARE(source.revision, revision);
        label = source.algorithm;
    }
    if (withoutLength) {
        QVERIFY(omitKeyLength(enc));
        int pages = 0;
        QCOMPARE(PageOps::probe(enc, &pages, password), PageOps::Status::Ok);
    }

    const QString extracted = files.filePath(QStringLiteral("extracted.pdf"));
    const QString ranges = files.filePath(QStringLiteral("ranges.pdf"));
    QCOMPARE(PageOps::merge({{enc, {1}, password}}, extracted, &err),
             PageOps::Status::Ok);
    QCOMPARE(PageOps::merge({{enc, {2}, password},
                             {enc, {0, 1}, password}},
                            ranges, &err),
             PageOps::Status::Ok);
    QStringList parts;
    QCOMPARE(PageOps::split(enc, files.path(), QStringLiteral("part"), password,
                            &parts, &err),
             PageOps::Status::Ok);
    QCOMPARE(parts.size(), 3);

    // Each output with the source page widths it should hold, in order.
    const QList<std::pair<QString, QList<int>>> outputs{
        {extracted, {200}}, {ranges, {300, 100, 200}}, {parts.at(0), {100}}, {parts.at(2), {300}}};
    for (const auto &[path, widths] : outputs) {
        const int count = int(widths.size());
        int pages = -1;
        QCOMPARE(PageOps::probe(path, &pages), PageOps::Status::NeedsPassword);
        QCOMPARE(PageOps::probe(path, &pages, QStringLiteral("owner")), PageOps::Status::Ok);
        QCOMPARE(PageOps::probe(path, &pages, password), PageOps::Status::Ok);
        QCOMPARE(pages, count);
        QpdfService::Info info;
        QCOMPARE(svc.readInfo(path, password, info, &err), QpdfService::Status::Ok);
        QVERIFY(info.encrypted);
        QCOMPARE(info.algorithm, label);
        QCOMPARE(info.keyLengthBits, bits);
        QCOMPARE(info.revision, revision);
        // MuPDF, which shows the copy, reads it with the same password.
        mervin::RenderEngine engine;
        bool needsPassword = false;
        QVERIFY(!engine.openDocument(path, QString(), &err, &needsPassword));
        QVERIFY(needsPassword);
        const auto doc = engine.openDocument(path, password, &err, &needsPassword);
        QVERIFY2(doc, qPrintable(err));
        QList<int> shown;
        for (int i = 0; i < doc->pageCount(); ++i)
            shown << qRound(doc->pageSize(i).width());
        QCOMPARE(shown, widths);
        if (!pdfDocKey.isEmpty())
            QVERIFY(openWithBytes(path, pdfDocKey)->userPasswordMatched());
    }
}

/// A copy of a file with only an owner password keeps its permission flags.
void TstQpdf::singleSourceCopiesKeepOwnerRestrictions()
{
    const QString src = in(QStringLiteral("or_src.pdf"));
    const QString enc = in(QStringLiteral("or_enc.pdf"));
    const QString out = in(QStringLiteral("or_out.pdf"));
    makePdf(src, 2);
    QpdfService::Permissions perms;
    perms.canPrint = false;
    QpdfService svc;
    QCOMPARE(svc.encrypt(src, enc, QString(), QString(), QStringLiteral("owner"),
                         QpdfService::Algorithm::AES256, perms, nullptr),
             QpdfService::Status::Ok);
    QCOMPARE(PageOps::merge({{enc, {1}, {}}, {enc, {0}, {}}}, out), PageOps::Status::Ok);
    QStringList parts;
    QCOMPARE(PageOps::split(enc, dir_.path(), QStringLiteral("or_part"), QString(), &parts),
             PageOps::Status::Ok);
    for (const QString &path : {out, parts.at(0)}) {
        QpdfService::Info info;
        QCOMPARE(svc.readInfo(path, QString(), info, nullptr), QpdfService::Status::Ok);
        QVERIFY(info.encrypted);
        QVERIFY(!info.permissions.canPrint);
        QVERIFY(info.permissions.canCopy);
    }
}

/// A merge that reads several files writes no encryption, even when one input has it.
void TstQpdf::mergeOfSeveralFilesIsNotEncrypted()
{
    const QString src = in(QStringLiteral("mx_src.pdf"));
    const QString enc = in(QStringLiteral("mx_enc.pdf"));
    const QString plain = in(QStringLiteral("mx_plain.pdf"));
    const QString out = in(QStringLiteral("mx_out.pdf"));
    makePdf(src, 2);
    makePdf(plain, 1);
    QpdfService svc;
    QCOMPARE(svc.encrypt(src, enc, QString(), QStringLiteral("secret"), QString(),
                         QpdfService::Algorithm::AES256, {}, nullptr),
             QpdfService::Status::Ok);
    QCOMPARE(PageOps::merge({{enc, {}, QStringLiteral("secret")}, {plain, {}, {}}}, out),
             PageOps::Status::Ok);
    QpdfService::Info info;
    QCOMPARE(svc.readInfo(out, QString(), info, nullptr), QpdfService::Status::Ok);
    QVERIFY(!info.encrypted);
}

void TstQpdf::splitWritesOnePerPage()
{
    const QString src = in(QStringLiteral("s_src.pdf"));
    makePdf(src, 3);
    QStringList outs;
    QCOMPARE(PageOps::split(src, dir_.path(), QStringLiteral("part"), QString(), &outs),
             PageOps::Status::Ok);
    QCOMPARE(outs.size(), 3);
    for (const QString &f : outs)
        QCOMPARE(PageOps::pageCount(f), 1);
}

void TstQpdf::rotateKeepsCount()
{
    const QString src = in(QStringLiteral("r_src.pdf"));
    const QString out = in(QStringLiteral("r_out.pdf"));
    makePdf(src, 3);
    QCOMPARE(PageOps::rotatePages(src, out, {0, 2}, 90, true), PageOps::Status::Ok);
    QCOMPARE(PageOps::pageCount(out), 3);
}

/// Covers V4 files with AES and with RC4, which Mervin writes with /Length.
void TstQpdf::rotateAndDeleteKeepEncryptionWithoutKeyLength_data()
{
    QTest::addColumn<QpdfService::Algorithm>("algorithm");
    QTest::newRow("AES-128") << QpdfService::Algorithm::AES128;
    QTest::newRow("RC4-128") << QpdfService::Algorithm::RC4_128;
}

/// Rotate and Delete rewrite a V4 file without /Length, still encrypted with its passwords.
void TstQpdf::rotateAndDeleteKeepEncryptionWithoutKeyLength()
{
    QFETCH(QpdfService::Algorithm, algorithm);
    QTemporaryDir files;
    QVERIFY(files.isValid());
    const QString src = files.filePath(QStringLiteral("src.pdf"));
    const QString enc = files.filePath(QStringLiteral("enc.pdf"));
    const QString rotated = files.filePath(QStringLiteral("rotated.pdf"));
    const QString deleted = files.filePath(QStringLiteral("deleted.pdf"));
    makePdfSized(src, {100, 200, 300});
    QpdfService svc;
    QCOMPARE(svc.encrypt(src, enc, QString(), QStringLiteral("secret"), QStringLiteral("owner"),
                         algorithm, {}, nullptr),
             QpdfService::Status::Ok);
    QVERIFY(omitKeyLength(enc));

    QString err;
    QCOMPARE(PageOps::rotatePages(enc, rotated, {1}, 90, true, QStringLiteral("secret"), &err),
             PageOps::Status::Ok);
    QCOMPARE(PageOps::deletePages(enc, deleted, {1}, QStringLiteral("secret"), &err),
             PageOps::Status::Ok);

    // Each output with its page widths, as MuPDF shows them, and rotations, in order.
    struct Output
    {
        QString path;
        QList<int> widths;
        QList<int> rotations;
    };
    const QList<Output> outputs{{rotated, {100, 792, 300}, {0, 90, 0}},
                                {deleted, {100, 300}, {0, 0}}};
    for (const auto &[path, widths, rotations] : outputs) {
        int pages = -1;
        QCOMPARE(PageOps::probe(path, &pages), PageOps::Status::NeedsPassword);
        QCOMPARE(PageOps::probe(path, &pages, QStringLiteral("owner")), PageOps::Status::Ok);
        QCOMPARE(PageOps::probe(path, &pages, QStringLiteral("secret")), PageOps::Status::Ok);
        QCOMPARE(pages, int(widths.size()));
        QpdfService::Info info;
        QCOMPARE(svc.readInfo(path, QStringLiteral("secret"), info, &err), QpdfService::Status::Ok);
        QVERIFY(info.encrypted);
        // qpdf writes every V4 file with AES.
        QCOMPARE(info.algorithm, QStringLiteral("AES-128"));
        QCOMPARE(info.keyLengthBits, 128);
        QCOMPARE(info.revision, 4);
        mervin::RenderEngine engine;
        const auto doc = engine.openDocument(path, QStringLiteral("secret"), &err);
        QVERIFY2(doc, qPrintable(err));
        QList<int> shown;
        for (int i = 0; i < doc->pageCount(); ++i)
            shown << qRound(doc->pageSize(i).width());
        QCOMPARE(shown, widths);
        QPDF pdf;
        pdf.processFile(path.toUtf8().constData(), "secret");
        QList<int> turned;
        for (auto &page : QPDFPageDocumentHelper(pdf).getAllPages()) {
            // Not const: qpdf 11's QPDFObjectHandle accessors are not const.
            auto rotate = page.getAttribute("/Rotate", false);
            turned << (rotate.isInteger() ? int(rotate.getIntValue()) : 0);
        }
        QCOMPARE(turned, rotations);
    }
}

void TstQpdf::openEncryptedNeedsPassword()
{
    const QString src = in(QStringLiteral("e_src.pdf"));
    const QString enc = in(QStringLiteral("e_enc.pdf"));
    makePdf(src, 1);
    QpdfService svc;
    QVERIFY(svc.encrypt(src, enc, QString(), QStringLiteral("secret"), QString(),
                        QpdfService::Algorithm::AES256, {}, nullptr)
            == QpdfService::Status::Ok);

    mervin::RenderEngine engine;
    QString err;
    bool needsPw = false;

    // No password -> refused, flagged as needing a password.
    QVERIFY(engine.openDocument(enc, QString(), &err, &needsPw) == nullptr);
    QVERIFY(needsPw);

    // Wrong password -> still refused.
    QVERIFY(engine.openDocument(enc, QStringLiteral("nope"), &err, &needsPw) == nullptr);
    QVERIFY(needsPw);

    // Correct password -> opens.
    auto doc = engine.openDocument(enc, QStringLiteral("secret"), &err, &needsPw);
    QVERIFY2(doc != nullptr, qPrintable(err));
    QVERIFY(!needsPw);
    QCOMPARE(doc->pageCount(), 1);
}

QTEST_GUILESS_MAIN(TstQpdf)
#include "tst_qpdf.moc"
