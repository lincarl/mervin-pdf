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
    void plainWebTextHitTestNormalizesWww();
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
