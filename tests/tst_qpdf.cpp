#include "render/Document.h"
#include "render/RenderEngine.h"
#include "security/PageOps.h"
#include "security/QpdfService.h"

#include <qpdf/QPDF.hh>
#include <qpdf/QPDFObjectHandle.hh>
#include <qpdf/QPDFPageDocumentHelper.hh>
#include <qpdf/QPDFPageObjectHelper.hh>
#include <qpdf/QPDFWriter.hh>

#include <QTemporaryDir>
#include <QTest>

using mervin::PageOps;
using mervin::QpdfService;

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
    QPDFWriter writer(q, path.toUtf8().constData());
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

} // namespace

class TstQpdf : public QObject
{
    Q_OBJECT

private slots:
    void init();

    void pageCountRoundTrip();
    void encryptAes256ThenReadInfo();
    void decryptRemovesEncryption();
    void userPasswordRequiredToOpen();
    void permissionsRoundTrip();
    void deletePagesReducesCount();
    void extractPagesInOrder();
    void mergeConcatenates();
    void mergeHonoursPerFileRanges();
    void mergeRepeatsAPage();
    void mergeEmptyPagesMeansAll();
    void mergeUsesPerFilePassword();
    void mergeReportsFailedIndex();
    void mergeRejectsOutOfRangeIndex();
    void probeDistinguishesLockedFromUnreadable();
    void splitWritesOnePerPage();
    void rotateKeepsCount();
    void openEncryptedNeedsPassword();

private:
    QTemporaryDir dir_;
    QString in(const QString &name) const { return dir_.filePath(name); }
};

void TstQpdf::init()
{
    QVERIFY(dir_.isValid());
}

void TstQpdf::pageCountRoundTrip()
{
    const QString p = in(QStringLiteral("count.pdf"));
    makePdf(p, 4);
    QCOMPARE(PageOps::pageCount(p), 4);
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

    QpdfService svc;
    QVERIFY(svc.encrypt(src, enc, QString(), QString(), QStringLiteral("owner"),
                        QpdfService::Algorithm::AES256, perms, nullptr)
            == QpdfService::Status::Ok);

    QpdfService::Info info;
    QCOMPARE(svc.readInfo(enc, QString(), info, nullptr), QpdfService::Status::Ok);
    QVERIFY(info.encrypted);
    QVERIFY(!info.permissions.canPrint);
    QVERIFY(!info.permissions.canCopy);
}

void TstQpdf::deletePagesReducesCount()
{
    const QString src = in(QStringLiteral("del_src.pdf"));
    const QString out = in(QStringLiteral("del_out.pdf"));
    makePdf(src, 5);
    QCOMPARE(PageOps::deletePages(src, out, {1, 3}), PageOps::Status::Ok);
    QCOMPARE(PageOps::pageCount(out), 3);
}

void TstQpdf::extractPagesInOrder()
{
    const QString src = in(QStringLiteral("ex_src.pdf"));
    const QString out = in(QStringLiteral("ex_out.pdf"));
    // Identifiable pages: the count alone would pass on a reversed or sorted
    // implementation, which is what this case is named for.
    makePdfSized(src, {100, 200, 300, 400, 500});
    QCOMPARE(PageOps::extractPages(src, out, {4, 0, 2}), PageOps::Status::Ok);
    QCOMPARE(widthsOf(out), QList<int>({500, 100, 300}));
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
    // extractPages skips an out-of-range index; merge must not. A page count that
    // went stale between the dialog's probe and this write would otherwise write a
    // shorter document than the dialog promised and still report success.
    const QString a = in(QStringLiteral("mo_a.pdf"));
    const QString out = in(QStringLiteral("mo_out.pdf"));
    makePdf(a, 2);
    QString err;
    int failed = -1;
    QCOMPARE(PageOps::merge({{a, {0, 9}, {}}}, out, &err, &failed), PageOps::Status::Failed);
    QCOMPARE(failed, 0);
    QVERIFY2(err.contains(QStringLiteral("10")), qPrintable(err)); // names the 1-based page
    QVERIFY2(err.contains(QStringLiteral("2 pages")), qPrintable(err));
    // No Qt %n here: without a translator loaded it leaves the "(s)" verbatim,
    // and this string reaches the user through MainWindow's failure box.
    QVERIFY2(!err.contains(QStringLiteral("(s)")), qPrintable(err));
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
