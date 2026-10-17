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

#include <algorithm>
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
    void clippedRenderMatchesFullRender_data();
    void clippedRenderMatchesFullRender();
    void renderWithoutAntialiasingKeepsHairlines();
    void renderStaysWithinTheEdgeClamp();
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

// The largest difference between two images in any colour channel, or 256 when
// their sizes differ.
int largestDifference(const QImage &a, const QImage &b)
{
    if (a.size() != b.size())
        return 256;
    int largest = 0;
    for (int y = 0; y < a.height(); ++y) {
        for (int x = 0; x < a.width(); ++x) {
            const QRgb p = a.pixel(x, y);
            const QRgb q = b.pixel(x, y);
            largest = std::max({largest, std::abs(qRed(p) - qRed(q)),
                                std::abs(qGreen(p) - qGreen(q)), std::abs(qBlue(p) - qBlue(q))});
        }
    }
    return largest;
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
    fz_context *ctx = engine.baseContext().get();
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

void TstDocument::clippedRenderMatchesFullRender_data()
{
    QTest::addColumn<int>("rotation");
    QTest::newRow("0") << 0;
    QTest::newRow("90") << 90;
    QTest::newRow("180") << 180;
    QTest::newRow("270") << 270;
}

// Print bands render with a device-pixel clip. Each clip must give the pixels of the
// full raster it selects, also for a turned page whose crop box does not start at the
// origin, and only the part of a clip that lies on the page.
void TstDocument::clippedRenderMatchesFullRender()
{
    QFETCH(int, rotation);
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    // The green square sits at the crop box's centre, so the centred clip below holds
    // its edges and the diagonal at every rotation.
    const QByteArray content = "1 0 0 rg 160 260 100 80 re f 0 0 1 rg 300 500 120 150 re f "
                               "0 0.5 0 rg 320 440 60 70 re f 0 G 2 w 150 250 m 550 700 l S";
    const QByteArray pdf = assemblePdf({
        "<< /Type /Catalog /Pages 2 0 R >>",
        "<< /Type /Pages /Count 1 /Kids [3 0 R] >>",
        "<< /Type /Page /Parent 2 0 R /MediaBox [100 200 712 992] "
        "/CropBox [150 250 550 700] /Rotate 90 /Contents 4 0 R >>",
        "<< /Length " + QByteArray::number(content.size()) + " >>\nstream\n" + content
            + "\nendstream",
    });
    const QString path = writeTemp(dir, QStringLiteral("clip.pdf"), pdf);
    QVERIFY(!path.isEmpty());
    RenderEngine engine;
    auto doc = engine.openDocument(path);
    QVERIFY(doc);

    constexpr double scale = 0.7;
    const QImage full = doc->renderPage(0, scale, rotation);
    QVERIFY(!full.isNull());
    const QRect inside(full.width() / 2 - 60, full.height() / 2 - 45, 120, 90);
    const QImage band = doc->renderPage(0, scale, rotation, inside);
    QCOMPARE(band.format(), QImage::Format_RGB888);
    // MuPDF clips a path's edges to the band, which can move antialiased edge pixels
    // by a few levels (up to 15 here). A clip one row off moves whole edges, which
    // then differ by 255.
    constexpr int tolerance = 32;
    const int inner = largestDifference(band, full.copy(inside));
    QVERIFY2(inner <= tolerance, qPrintable(QString::number(inner)));
    // A clip past the raster's edge returns only its part on the page.
    const QRect edge(full.width() - 30, full.height() - 20, 100, 100);
    const int outer = largestDifference(doc->renderPage(0, scale, rotation, edge),
                                        full.copy(edge & full.rect()));
    QVERIFY2(outer <= tolerance, qPrintable(QString::number(outer)));
    QVERIFY(doc->renderPage(0, scale, rotation, QRect(full.width() + 5, 0, 10, 10)).isNull());
}

// Black and white printing renders without antialiasing, so a stroke thinner than
// a pixel still reaches full black instead of a light gray that its threshold drops.
void TstDocument::renderWithoutAntialiasingKeepsHairlines()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QByteArray content = "0 G 0.1 w 10 50.5 m 90 50.5 l S";
    const QByteArray pdf = assemblePdf({
        "<< /Type /Catalog /Pages 2 0 R >>",
        "<< /Type /Pages /Count 1 /Kids [3 0 R] >>",
        "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 100 100] /Contents 4 0 R >>",
        "<< /Length " + QByteArray::number(content.size()) + " >>\nstream\n" + content
            + "\nendstream",
    });
    const QString path = writeTemp(dir, QStringLiteral("hairline.pdf"), pdf);
    QVERIFY(!path.isEmpty());
    RenderEngine engine;
    auto doc = engine.openDocument(path);
    QVERIFY(doc);

    const auto darkest = [](const QImage &image) {
        int value = 255;
        for (int y = 0; y < image.height(); ++y)
            for (int x = 0; x < image.width(); ++x)
                value = std::min(value, qGray(image.pixel(x, y)));
        return value;
    };
    const QImage smooth = doc->renderPage(0, 1, 0);
    const QImage hard = doc->renderPage(0, 1, 0, {}, false);
    QVERIFY(!smooth.isNull() && !hard.isNull());
    QVERIFY2(darkest(smooth) > 128, qPrintable(QString::number(darkest(smooth))));
    QCOMPARE(darkest(hard), 0);
    // The context's antialiasing returns for later renders.
    QCOMPARE(doc->renderPage(0, 1, 0), smooth);
}

// MuPDF clamps edges to 2^20 device pixels from the device origin. A band of a larger
// raster would come back blank or with displaced shapes, so it must fail instead,
// while a raster just inside the limit keeps every shape in place.
void TstDocument::renderStaysWithinTheEdgeClamp()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    // A 144000 pt square page (UserUnit 10) with a small square right of its centre
    // and a long line of slope 2 through the centre.
    const QByteArray content = "0 g 7220 7190 40 20 re f 0 G 0.5 w 3600 0 m 10800 14400 l S";
    const QByteArray pdf = assemblePdf({
        "<< /Type /Catalog /Pages 2 0 R >>",
        "<< /Type /Pages /Count 1 /Kids [3 0 R] >>",
        "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 14400 14400] /UserUnit 10 "
        "/Contents 4 0 R >>",
        "<< /Length " + QByteArray::number(content.size()) + " >>\nstream\n" + content
            + "\nendstream",
    });
    const QString path = writeTemp(dir, QStringLiteral("huge.pdf"), pdf);
    QVERIFY(!path.isEmpty());
    RenderEngine engine;
    auto doc = engine.openDocument(path);
    QVERIFY(doc);
    QCOMPARE(doc->pageSize(0), QSizeF(144000, 144000));

    // At 7.2 pixels a point the raster is 1036800 pixels square. The page centre is
    // pixel 518400, where the line crosses the band's middle row at band column 2000.
    // The line is 36 pixels wide, so its row spans about 40 columns. The square
    // starts at column 3440.
    const QImage band = doc->renderPage(0, 7.2, 0, QRect(516400, 518350, 4000, 100));
    QVERIFY(!band.isNull());
    int first = -1;
    int last = -1;
    for (int x = 0; x < 3000; ++x) {
        if (qGray(band.pixel(x, 50)) < 128) {
            first = first < 0 ? x : first;
            last = x;
        }
    }
    QVERIFY2(std::abs((first + last) / 2 - 2000) <= 3 && last - first >= 30 && last - first <= 50,
             qPrintable(QStringLiteral("line at columns %1 to %2").arg(first).arg(last)));
    for (int x = 3460; x < 4000; x += 20)
        QVERIFY2(qGray(band.pixel(x, 50)) < 128, qPrintable(QString::number(x)));

    // At 16 pixels a point the raster is 2304000 pixels square, and the same place is
    // beyond the clamp.
    QVERIFY(doc->renderPage(0, 16, 0, QRect(1148000, 1151900, 8000, 200)).isNull());
    QVERIFY(doc->renderPage(0, 16, 0, QRect(0, 0, 100, 100)).isNull());
}

QTEST_GUILESS_MAIN(TstDocument)
#include "tst_document.moc"
