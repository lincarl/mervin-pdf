#include "render/Document.h"
#include "render/RenderEngine.h"
#include "render/TextIndex.h"

#include <QByteArray>
#include <QFile>
#include <QTemporaryDir>
#include <QTest>

#include <memory>

using namespace mervin;

class TstTextIndex : public QObject
{
    Q_OBJECT

private slots:
    void searchRespectsOptions_data();
    void searchRespectsOptions();
    void plainWebTextHitTestNormalizesWww();
    void selectionGeometryUsesPageOriginAndUtf16();
    void pdfLinkAnnotationHitTestReturnsUri();
    void pdfInternalLinkAnnotationResolvesTargetPage();
    void pdfJavaScriptPropertiesHitTestReturnsValues();
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
    pdf += "trailer\n<< /Size " + QByteArray::number(n)
           + " /Root 1 0 R >>\nstartxref\n" + QByteArray::number(xrefOff) + "\n%%EOF\n";
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

QByteArray makeWebTextPdf()
{
    const QByteArray content = "BT /F1 18 Tf 50 250 Td (Visit www.inet.se now.) Tj ET";

    QList<QByteArray> objs;
    objs << "<< /Type /Catalog /Pages 2 0 R >>";
    objs << "<< /Type /Pages /Kids [3 0 R] /Count 1 >>";
    objs << "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 300 300] "
            "/Resources << /Font << /F1 5 0 R >> >> /Contents 4 0 R >>";
    objs << "<< /Length " + QByteArray::number(content.size()) + " >>\nstream\n" + content
            + "\nendstream";
    objs << "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>";
    return assemblePdf(objs);
}

// Two text pages distinguish word boundaries, case, page order and overlapping matches.
QByteArray makeSearchPdf()
{
    const QList<QByteArray> texts{
        "Cat cat scatter cat_cat cat2 2cat cat-cat.", "cat CAT cat banana"};
    QList<QByteArray> objs;
    objs << "<< /Type /Catalog /Pages 2 0 R >>";
    objs << "<< /Type /Pages /Kids [4 0 R 6 0 R] /Count 2 >>";
    objs << "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>";
    for (int i = 0; i < texts.size(); ++i) {
        const QByteArray content = "BT /F1 12 Tf 20 160 Td (" + texts.at(i) + ") Tj ET";
        objs << "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 500 200] "
                "/Resources << /Font << /F1 3 0 R >> >> /Contents "
                    + QByteArray::number(5 + 2 * i) + " 0 R >>";
        objs << "<< /Length " + QByteArray::number(content.size()) + " >>\nstream\n"
                    + content + "\nendstream";
    }
    return assemblePdf(objs);
}

QByteArray makeLinkAnnotationPdf()
{
    QList<QByteArray> objs;
    objs << "<< /Type /Catalog /Pages 2 0 R >>";
    objs << "<< /Type /Pages /Kids [3 0 R] /Count 1 >>";
    objs << "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 300 300] "
            "/Resources << >> /Annots [4 0 R] >>";
    objs << "<< /Type /Annot /Subtype /Link /Rect [100 250 180 280] /Border [0 0 0] "
            "/A << /S /URI /URI (https://example.com/path) >> >>";
    return assemblePdf(objs);
}

QByteArray makeInternalLinkAnnotationPdf()
{
    QList<QByteArray> objs;
    objs << "<< /Type /Catalog /Pages 2 0 R >>";
    objs << "<< /Type /Pages /Kids [3 0 R 4 0 R] /Count 2 >>";
    objs << "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 300 300] "
            "/Resources << >> /Annots [5 0 R] >>";
    objs << "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 300 300] /Resources << >> >>";
    objs << "<< /Type /Annot /Subtype /Link /Rect [100 250 180 280] /Border [0 0 0] "
            "/Dest [4 0 R /FitB] >>";
    return assemblePdf(objs);
}

QByteArray makePropertyAnnotationPdf()
{
    const QByteArray js = "ShM([\n"
                          "[\"Reference = EXAMPLE-42\"],\n"
                          "[\"Price \\u00281k\\u0029 = 1.23USD\"],\n"
                          "[\"https://example.com/components/42\", "
                          "\"https://example.com/components/42\"],\n"
                          "]);";

    QList<QByteArray> objs;
    objs << "<< /Type /Catalog /Pages 2 0 R >>";
    objs << "<< /Type /Pages /Kids [3 0 R] /Count 1 >>";
    objs << "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 300 300] "
            "/Resources << >> /Annots [4 0 R] >>";
    objs << "<< /Type /Annot /Subtype /Link /Rect [100 250 180 280] /Border [0 0 0] "
            "/A << /S /JavaScript /JS <" + js.toHex() + "> >> >>";
    return assemblePdf(objs);
}

std::unique_ptr<Document> openTempPdf(RenderEngine &engine, const QTemporaryDir &dir,
                                      const QString &name, const QByteArray &bytes)
{
    const QString path = writeTemp(dir, name, bytes);
    if (path.isEmpty())
        return nullptr;

    QString err;
    std::unique_ptr<Document> doc = engine.openDocument(path, QString(), &err);
    if (!doc)
        qWarning("openDocument failed: %s", qPrintable(err));
    return doc;
}

} // namespace

// Literal offsets come from the authored page text, independently of the matcher.
void TstTextIndex::searchRespectsOptions_data()
{
    QTest::addColumn<QString>("query");
    QTest::addColumn<bool>("caseSensitive");
    QTest::addColumn<bool>("wholeWord");
    QTest::addColumn<QList<int>>("pages");
    QTest::addColumn<QList<int>>("starts");

    QTest::newRow("insensitive substring") << "cat" << false << false
        << QList<int>({0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1})
        << QList<int>({0, 4, 9, 16, 20, 24, 30, 34, 38, 0, 4, 8});
    QTest::newRow("sensitive substring") << "cat" << true << false
        << QList<int>({0, 0, 0, 0, 0, 0, 0, 0, 1, 1})
        << QList<int>({4, 9, 16, 20, 24, 30, 34, 38, 0, 8});
    QTest::newRow("insensitive whole word") << "cat" << false << true
        << QList<int>({0, 0, 0, 0, 1, 1, 1}) << QList<int>({0, 4, 34, 38, 0, 4, 8});
    QTest::newRow("sensitive whole word") << "cat" << true << true
        << QList<int>({0, 0, 0, 1, 1}) << QList<int>({4, 34, 38, 0, 8});
    QTest::newRow("non-overlapping substring") << "ana" << true << false
        << QList<int>({1}) << QList<int>({13});
    QTest::newRow("absent") << "dog" << false << false << QList<int>() << QList<int>();
    QTest::newRow("empty") << "" << false << false << QList<int>() << QList<int>();
}

// Searches real extracted text and checks every match, including rejected boundary cases.
void TstTextIndex::searchRespectsOptions()
{
    QFETCH(QString, query);
    QFETCH(bool, caseSensitive);
    QFETCH(bool, wholeWord);
    QFETCH(QList<int>, pages);
    QFETCH(QList<int>, starts);
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    RenderEngine engine;
    const auto doc = openTempPdf(engine, dir, QStringLiteral("search.pdf"), makeSearchPdf());
    QVERIFY(doc);
    TextIndex index(engine.baseContext(), doc.get());

    const auto matches = index.search(query, caseSensitive, wholeWord);
    QCOMPARE(matches.size(), size_t(pages.size()));
    for (size_t i = 0; i < matches.size(); ++i) {
        QCOMPARE(matches[i].page, pages.at(qsizetype(i)));
        QCOMPARE(matches[i].start, starts.at(qsizetype(i)));
        QCOMPARE(matches[i].length, query.size());
    }
}

// Authored Courier cells give independent hit-test coordinates and widths.
void TstTextIndex::selectionGeometryUsesPageOriginAndUtf16()
{
    const QByteArray content = "BT /F1 20 Tf 120 450 Td (ABCD) Tj 0 -50 Td (www.inet.se) Tj ET";
    const QByteArray cmap = "/CIDInit /ProcSet findresource begin 12 dict begin begincmap "
        "/CIDSystemInfo << /Registry (Adobe) /Ordering (UCS) /Supplement 0 >> def "
        "/CMapName /Test def /CMapType 2 def 1 begincodespacerange <00> <FF> endcodespacerange "
        "1 beginbfchar <41> <D83DDE00> endbfchar endcmap CMapName currentdict /CMap defineresource pop end end";
    const QList<QByteArray> objects{
        "<< /Type /Catalog /Pages 2 0 R >>",
        "<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
        "<< /Type /Page /Parent 2 0 R /MediaBox [100 200 400 500] "
        "/Resources << /Font << /F1 5 0 R >> >> /Contents 4 0 R >>",
        "<< /Length " + QByteArray::number(content.size()) + " >>\nstream\n" + content + "\nendstream",
        "<< /Type /Font /Subtype /Type1 /BaseFont /Courier /ToUnicode 6 0 R >>",
        "<< /Length " + QByteArray::number(cmap.size()) + " >>\nstream\n" + cmap + "\nendstream"};
    QTemporaryDir files;
    RenderEngine engine;
    auto doc = openTempPdf(engine, files, QStringLiteral("geometry.pdf"), assemblePdf(objects));
    QVERIFY(doc);
    TextIndex index(engine.baseContext(), doc.get());
    const QString smile = QString::fromUcs4(U"\U0001f600");
    QCOMPARE(index.pageText(0), smile + QStringLiteral("BCD\nwww.inet.se\n"));
    QCOMPARE(index.textRange(0, 0, 2), smile);
    const auto high = index.rangeRects(0, 0, 1);
    const auto low = index.rangeRects(0, 1, 1);
    const auto next = index.rangeRects(0, 2, 1);
    QCOMPARE(high.size(), size_t(1));
    QCOMPARE(low.size(), size_t(1));
    QCOMPARE(next.size(), size_t(1));
    for (const QRectF &rect : {high[0], low[0]}) {
        QVERIFY(qAbs(rect.left() - 20.0) < 0.1);
        QVERIFY(qAbs(rect.width() - 12.0) < 0.1);
        QVERIFY(rect.top() > 30 && rect.top() < 35);
        QVERIFY(rect.bottom() > 55 && rect.bottom() < 58);
    }
    QVERIFY(qAbs(next[0].left() - 32.0) < 0.1);
    QVERIFY(qAbs(next[0].width() - 12.0) < 0.1);
    QCOMPARE(index.offsetAt(0, {21, 45}), 0);
    QCOMPARE(index.offsetAt(0, {33, 45}), 2);
    QCOMPARE(index.offsetAt(0, {100, 45}), 5); // before the first newline
    int start = -1;
    int end = -1;
    index.wordBoundsAt(0, 3, &start, &end);
    QCOMPARE(start, 2);
    QCOMPARE(end, 5);
    const auto lines = index.rangeRects(0, 0, 18);
    QCOMPARE(lines.size(), size_t(2));
    QVERIFY(qAbs(lines[0].left() - 20.0) < 0.1);
    QVERIFY(qAbs(lines[0].width() - 48.0) < 0.1);
    QVERIFY(qAbs(lines[1].left() - 20.0) < 0.1);
    QVERIFY(qAbs(lines[1].width() - 132.0) < 0.1);
    QVERIFY(qAbs(lines[1].top() - lines[0].top() - 50.0) < 0.1);
    QVERIFY(index.rangeRects(0, 5, 1).empty()); // newline carries no selection box
    const auto link = index.linkAt(0, {33, 95});
    QVERIFY(link);
    QCOMPARE(link->start, 6);
    QCOMPARE(link->length, 11);
    QCOMPARE(link->url, QStringLiteral("https://www.inet.se"));
    QVERIFY(!index.linkAt(0, {160, 95}));
    const auto match = index.search(smile, true, false);
    QCOMPARE(match.size(), size_t(1));
    QCOMPARE(match[0].start, 0);
    QCOMPARE(match[0].length, 2);
}

void TstTextIndex::plainWebTextHitTestNormalizesWww()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    RenderEngine engine;
    std::unique_ptr<Document> doc = openTempPdf(engine, dir, QStringLiteral("web-text.pdf"),
                                                makeWebTextPdf());
    QVERIFY(doc != nullptr);

    TextIndex index(engine.baseContext(), doc.get());
    const QString text = index.pageText(0);
    const QString needle = QStringLiteral("www.inet.se");
    const int start = text.indexOf(needle);
    QVERIFY2(start >= 0, qPrintable(text));

    const std::vector<QRectF> rects = index.rangeRects(0, start, needle.size());
    QVERIFY(!rects.empty());

    const std::optional<TextLink> link = index.linkAt(0, rects.front().center());
    QVERIFY(link.has_value());
    QCOMPARE(link->page, 0);
    QCOMPARE(link->start, start);
    QCOMPARE(link->length, needle.size());
    QCOMPARE(link->url, QStringLiteral("https://www.inet.se"));

    QVERIFY(!index.linkAt(0, QPointF(5, 5)).has_value());
}

void TstTextIndex::pdfLinkAnnotationHitTestReturnsUri()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    RenderEngine engine;
    std::unique_ptr<Document> doc = openTempPdf(engine, dir, QStringLiteral("annot-link.pdf"),
                                                makeLinkAnnotationPdf());
    QVERIFY(doc != nullptr);

    QCOMPARE(doc->linkAt(0, QPointF(140, 35)), QStringLiteral("https://example.com/path"));
    QVERIFY(doc->linkAt(0, QPointF(140, 80)).isEmpty());
}

void TstTextIndex::pdfInternalLinkAnnotationResolvesTargetPage()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    RenderEngine engine;
    std::unique_ptr<Document> doc = openTempPdf(engine, dir, QStringLiteral("internal-link.pdf"),
                                                makeInternalLinkAnnotationPdf());
    QVERIFY(doc != nullptr);

    const std::optional<PdfLinkTarget> target = doc->linkTargetAt(0, QPointF(140, 35));
    QVERIFY(target.has_value());
    QVERIFY(target->isInternal());
    QCOMPARE(target->page, 1);
    QVERIFY(doc->linkTargetAt(0, QPointF(140, 80)) == std::nullopt);
}

void TstTextIndex::pdfJavaScriptPropertiesHitTestReturnsValues()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    RenderEngine engine;
    std::unique_ptr<Document> doc = openTempPdf(engine, dir, QStringLiteral("properties.pdf"),
                                                makePropertyAnnotationPdf());
    QVERIFY(doc != nullptr);

    const std::optional<PdfItemProperties> props = doc->itemPropertiesAt(0, QPointF(140, 35));
    QVERIFY(props.has_value());
    QCOMPARE(props->page, 0);
    QCOMPARE(props->values.size(), 3);
    QCOMPARE(props->values.at(0), QStringLiteral("Reference = EXAMPLE-42"));
    QCOMPARE(props->values.at(1), QStringLiteral("Price (1k) = 1.23USD"));
    QCOMPARE(props->values.at(2), QStringLiteral("https://example.com/components/42"));
    QVERIFY(doc->itemPropertiesAt(0, QPointF(140, 80)) == std::nullopt);
}
QTEST_GUILESS_MAIN(TstTextIndex)
#include "tst_text_index.moc"
