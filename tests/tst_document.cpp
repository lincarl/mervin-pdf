#include "render/Document.h"
#include "render/RenderEngine.h"

#include <mupdf/fitz.h>
#include <mupdf/pdf.h>

#include <QByteArray>
#include <QFile>
#include <QFileInfo>
#include <QSizeF>
#include <QStringList>
#include <QTemporaryDir>
#include <QTest>

#include <memory>
#include <mutex>

using namespace mervin;

// Document's per-page size table used to come from fz_load_page + fz_bound_page.
// fz_load_page builds a pdf_page and eagerly resolves every annotation and link on
// the page, so it now reads the page object directly instead
// (pdf_lookup_page_obj + pdf_page_obj_transform). fz_bound_page requests the crop
// box, and so does pdf_page_obj_transform, which is what makes the two equivalent.
//
// These tests are that equivalence, page by page: the sizes Document reports must
// match what a fully loaded page reports, or the cheaper walk silently resized
// documents (wrong fit-to-width scale, wrong scroll extent, wrong measurements).
class TstDocument : public QObject
{
    Q_OBJECT

private slots:
    void pageSizesMatchLoadedPageBounds();
    void pageSizesMatchOnShippedExamples();
    void pageWithoutAnyBoxFallsBackLikeLoadedPage();
};

namespace {

QByteArray assemblePdf(const QList<QByteArray> &bodies)
{
    QByteArray pdf = "%PDF-1.7\n";
    QList<int> offsets;
    for (int i = 0; i < bodies.size(); ++i) {
        offsets << pdf.size();
        pdf += QByteArray::number(i + 1) + " 0 obj\n" + bodies[i] + "\nendobj\n";
    }
    const int xrefOff = pdf.size();
    const int n = bodies.size() + 1;
    pdf += "xref\n0 " + QByteArray::number(n) + "\n";
    pdf += "0000000000 65535 f \n";
    for (int off : offsets) {
        QByteArray rec = QByteArray::number(off);
        while (rec.size() < 10)
            rec.prepend('0');
        pdf += rec + " 00000 n \n";
    }
    pdf += "trailer\n<< /Size " + QByteArray::number(n) + " /Root 1 0 R >>\nstartxref\n"
           + QByteArray::number(xrefOff) + "\n%%EOF\n";
    return pdf;
}

QString writeTemp(const QTemporaryDir &dir, const QString &name, const QByteArray &bytes)
{
    const QString path = dir.filePath(name);
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly))
        return QString();
    f.write(bytes);
    f.close();
    return path;
}

// The size fz_bound_page reports for a fully loaded page - the reference the
// cheap page-object walk has to reproduce. Returns an invalid size on failure.
QSizeF loadedPageSize(fz_context *ctx, fz_document *doc, int pageNo)
{
    double w = 0.0;
    double h = 0.0;
    fz_page *page = nullptr;
    fz_var(page);
    fz_var(w);
    fz_var(h);
    fz_try(ctx) {
        page = fz_load_page(ctx, doc, pageNo);
        const fz_rect r = fz_bound_page(ctx, page);
        w = r.x1 - r.x0;
        h = r.y1 - r.y0;
    }
    fz_always(ctx) {
        if (page)
            fz_drop_page(ctx, page);
    }
    fz_catch(ctx) {
        w = h = 0.0;
    }
    return (w > 0.0 && h > 0.0) ? QSizeF(w, h) : QSizeF();
}

// Compare every page of an open document against the loaded-page reference.
// Reported through QTest so a mismatch names the file and the page.
void verifyEveryPage(RenderEngine &engine, const Document &doc, const QString &label)
{
    fz_context *ctx = engine.baseContext();
    std::lock_guard<std::mutex> lk(doc.accessMutex()); // same contract the workers honour
    for (int i = 0; i < doc.pageCount(); ++i) {
        const QSizeF ref = loadedPageSize(ctx, doc.handle(), i);
        QVERIFY2(ref.isValid(),
                 qPrintable(QStringLiteral("%1: page %2 would not load").arg(label).arg(i)));
        const QSizeF got = doc.pageSize(i);
        QVERIFY2(qFuzzyCompare(got.width(), ref.width()) && qFuzzyCompare(got.height(), ref.height()),
                 qPrintable(QStringLiteral("%1: page %2 is %3x%4, loaded page says %5x%6")
                                .arg(label)
                                .arg(i)
                                .arg(got.width())
                                .arg(got.height())
                                .arg(ref.width())
                                .arg(ref.height())));
    }
}

} // namespace

void TstDocument::pageSizesMatchLoadedPageBounds()
{
    // Three pages that between them cover everything the page-box lookup has to
    // get right: a crop box smaller than a non-zero-origin media box plus a
    // rotation, an inherited media box with no page-level box at all, and an
    // inherited box with the other rotation.
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QByteArray pdf = assemblePdf({
        "<< /Type /Catalog /Pages 2 0 R >>",
        "<< /Type /Pages /Count 3 /Kids [3 0 R 4 0 R 5 0 R] /MediaBox [0 0 300 400] >>",
        // Crop box 400x450 inside a 612x792 media box at origin (100,200), turned 90.
        "<< /Type /Page /Parent 2 0 R /MediaBox [100 200 712 992] "
        "/CropBox [150 250 550 700] /Rotate 90 >>",
        "<< /Type /Page /Parent 2 0 R >>",              // inherits 300x400
        "<< /Type /Page /Parent 2 0 R /Rotate 270 >>",  // inherits, turned 270
    });
    const QString path = writeTemp(dir, QStringLiteral("boxes.pdf"), pdf);
    QVERIFY(!path.isEmpty());

    RenderEngine engine;
    QString err;
    bool needsPw = false;
    auto doc = engine.openDocument(path, QString(), &err, &needsPw);
    QVERIFY2(doc != nullptr, qPrintable(err));
    QCOMPARE(doc->pageCount(), 3);

    verifyEveryPage(engine, *doc, QStringLiteral("boxes.pdf"));

    // Absolute values too, so the test still fails if BOTH paths start agreeing on
    // something wrong (a rotation dropped, the media box used instead of the crop
    // box, the box origin ignored).
    QCOMPARE(doc->pageSize(0), QSizeF(450.0, 400.0)); // 400x450 crop box, turned 90
    QCOMPARE(doc->pageSize(1), QSizeF(300.0, 400.0)); // inherited, unrotated
    QCOMPARE(doc->pageSize(2), QSizeF(400.0, 300.0)); // inherited, turned 270
}

void TstDocument::pageSizesMatchOnShippedExamples()
{
    // Real files, including the ones the change was made for: schematic.pdf is a
    // link-annotation-per-component export and form_comment.pdf carries widget and
    // markup annotations - exactly the pages whose annotations fz_load_page used to
    // resolve just to hand back a width and a height.
    const QStringList files = {
#ifdef MERVIN_SCHEMATIC_PDF
        QStringLiteral(MERVIN_SCHEMATIC_PDF),
#endif
#ifdef MERVIN_FORM_PDF
        QStringLiteral(MERVIN_FORM_PDF),
#endif
#ifdef MERVIN_HOUSE_PDF
        QStringLiteral(MERVIN_HOUSE_PDF),
#endif
#ifdef MERVIN_IMAGES_PDF
        QStringLiteral(MERVIN_IMAGES_PDF),
#endif
    };
    QCOMPARE(files.size(), 4); // all four are configured by the build

    RenderEngine engine;
    int tested = 0;
    for (const QString &path : files) {
        if (!QFileInfo::exists(path))
            continue;
        QString err;
        bool needsPw = false;
        auto doc = engine.openDocument(path, QString(), &err, &needsPw);
        QVERIFY2(doc != nullptr, qPrintable(QStringLiteral("%1: %2").arg(path, err)));
        QVERIFY(doc->pageCount() > 0);
        verifyEveryPage(engine, *doc, QFileInfo(path).fileName());
        ++tested;
    }
    if (tested == 0)
        QSKIP("optional local PDF fixtures are not present");
}

void TstDocument::pageWithoutAnyBoxFallsBackLikeLoadedPage()
{
    // No media box anywhere in the page's parent chain. MuPDF substitutes US
    // Letter itself, and both paths must land on the same substitute rather than
    // one of them falling through to Document's own fallback with a different size.
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QByteArray pdf = assemblePdf({
        "<< /Type /Catalog /Pages 2 0 R >>",
        "<< /Type /Pages /Count 1 /Kids [3 0 R] >>",
        "<< /Type /Page /Parent 2 0 R >>",
    });
    const QString path = writeTemp(dir, QStringLiteral("noboxes.pdf"), pdf);
    QVERIFY(!path.isEmpty());

    RenderEngine engine;
    QString err;
    bool needsPw = false;
    auto doc = engine.openDocument(path, QString(), &err, &needsPw);
    QVERIFY2(doc != nullptr, qPrintable(err));
    QCOMPARE(doc->pageCount(), 1);
    verifyEveryPage(engine, *doc, QStringLiteral("noboxes.pdf"));
    QCOMPARE(doc->pageSize(0), QSizeF(612.0, 792.0));
}

QTEST_GUILESS_MAIN(TstDocument)
#include "tst_document.moc"
