#include "render/AnnotModel.h"
#include "render/AnnotTypes.h"
#include "render/Document.h"
#include "render/RenderEngine.h"

#include <QByteArray>
#include <QColor>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QTest>

#include <cmath>
#include <memory>

using namespace mervin;

// AnnotModel is the only owner of markup-annotation mutation (design §6b). These
// tests build a tiny self-contained PDF on disk (so they always run, no fixture
// needed), then exercise enumeration, creation (highlight + sticky note),
// mutation (comment text, colour, delete) and the create -> save -> reopen
// round-trip that backs Save edits.
class TstAnnotModel : public QObject
{
    Q_OBJECT

private slots:
    void plainDocumentHasNoAnnots();
    void addHighlightEnumeratesAndPlacesRect();
    void highlightSaveReopenRoundTrip();
    void stickyNoteRoundTrip();
    void editContentsColorAndDelete();
    void nonLatinCommentSurvivesSave();
};

namespace {

// Same minimal-PDF assembler as tst_form_model: object bodies in, valid xref out.
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

// A one-page 300x300 PDF with no annotations.
QByteArray makePlainPdf()
{
    QList<QByteArray> objs;
    objs << "<< /Type /Catalog /Pages 2 0 R >>";
    objs << "<< /Type /Pages /Kids [3 0 R] /Count 1 >>";
    objs << "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 300 300] /Resources << >> >>";
    return assemblePdf(objs);
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

} // namespace

void TstAnnotModel::plainDocumentHasNoAnnots()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = writeTemp(dir, QStringLiteral("plain.pdf"), makePlainPdf());
    QVERIFY(!path.isEmpty());

    RenderEngine engine;
    QString err;
    std::unique_ptr<Document> doc = engine.openDocument(path, QString(), &err);
    QVERIFY2(doc != nullptr, qPrintable(err));

    AnnotModel am(*doc);
    QVERIFY(am.pageAnnots(0).empty());
    QVERIFY(am.allAnnots().empty());
    QVERIFY(!am.isDirty());
}

void TstAnnotModel::addHighlightEnumeratesAndPlacesRect()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = writeTemp(dir, QStringLiteral("plain.pdf"), makePlainPdf());
    QVERIFY(!path.isEmpty());

    RenderEngine engine;
    QString err;
    std::unique_ptr<Document> doc = engine.openDocument(path, QString(), &err);
    QVERIFY2(doc != nullptr, qPrintable(err));

    AnnotModel am(*doc);
    // A line rect in app page-point space (top-left origin, y-down).
    const std::vector<QRectF> lines{QRectF(20, 30, 100, 12)};
    const int id = am.addTextMarkup(0, AnnotType::Highlight, lines, QColor(255, 242, 0),
                                    QStringLiteral("tester"), QStringLiteral("hello"));
    QVERIFY(id >= 0);
    QVERIFY(am.isDirty());

    const std::vector<Annotation> &as = am.pageAnnots(0);
    QCOMPARE(as.size(), size_t(1));
    QCOMPARE(as[0].type, AnnotType::Highlight);
    QCOMPARE(as[0].id, id);
    QCOMPARE(as[0].contents, QStringLiteral("hello"));
    QCOMPARE(as[0].author, QStringLiteral("tester"));
    // The highlight rect should round-trip back to ~the source line rect:
    // page-point (20,30,100,12) -> PDF [20 258 120 270] -> back to app space.
    // MuPDF's highlight appearance inflates the bounding /Rect a few points
    // beyond the text quad (rounded corners + stroke), so allow ~4pt of padding
    // on each side rather than demanding an exact match.
    QVERIFY2(std::abs(as[0].rect.x() - 20.0) < 4.0, qPrintable(QString::number(as[0].rect.x())));
    QVERIFY2(std::abs(as[0].rect.y() - 30.0) < 4.0, qPrintable(QString::number(as[0].rect.y())));
    QVERIFY(as[0].rect.width() >= 99.0 && as[0].rect.width() < 110.0);
    QVERIFY(as[0].rect.height() >= 11.0 && as[0].rect.height() < 22.0);
    // Colour read back ~ yellow.
    QVERIFY(as[0].color.red() > 240 && as[0].color.green() > 230 && as[0].color.blue() < 20);
}

void TstAnnotModel::highlightSaveReopenRoundTrip()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString src = writeTemp(dir, QStringLiteral("plain.pdf"), makePlainPdf());
    QVERIFY(!src.isEmpty());

    RenderEngine engine;
    QString err;
    std::unique_ptr<Document> doc = engine.openDocument(src, QString(), &err);
    QVERIFY2(doc != nullptr, qPrintable(err));

    AnnotModel am(*doc);
    const int id = am.addTextMarkup(0, AnnotType::Underline, {QRectF(20, 30, 100, 12)},
                                    QColor(0, 120, 255), QStringLiteral("tester"),
                                    QStringLiteral("note"));
    QVERIFY(id >= 0);

    const QString out = dir.filePath(QStringLiteral("annotated.pdf"));
    QString serr;
    QVERIFY2(doc->savePdfTo(out, &serr), qPrintable(serr));
    QVERIFY(QFileInfo::exists(out));

    // Reopen: the annotation persists as a standard /Annot (no private blob).
    std::unique_ptr<Document> doc2 = engine.openDocument(out, QString(), &err);
    QVERIFY2(doc2 != nullptr, qPrintable(err));
    AnnotModel am2(*doc2);
    const std::vector<Annotation> &as = am2.pageAnnots(0);
    QCOMPARE(as.size(), size_t(1));
    QCOMPARE(as[0].type, AnnotType::Underline);
    QCOMPARE(as[0].contents, QStringLiteral("note"));
}

void TstAnnotModel::stickyNoteRoundTrip()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString src = writeTemp(dir, QStringLiteral("plain.pdf"), makePlainPdf());
    QVERIFY(!src.isEmpty());

    RenderEngine engine;
    QString err;
    std::unique_ptr<Document> doc = engine.openDocument(src, QString(), &err);
    QVERIFY2(doc != nullptr, qPrintable(err));

    AnnotModel am(*doc);
    const int id = am.addTextNote(0, QPointF(50, 50), QColor(255, 242, 0),
                                  QStringLiteral("tester"), QStringLiteral("a sticky comment"));
    QVERIFY(id >= 0);

    const QString out = dir.filePath(QStringLiteral("noted.pdf"));
    QString serr;
    QVERIFY2(doc->savePdfTo(out, &serr), qPrintable(serr));

    std::unique_ptr<Document> doc2 = engine.openDocument(out, QString(), &err);
    QVERIFY2(doc2 != nullptr, qPrintable(err));
    AnnotModel am2(*doc2);
    const std::vector<Annotation> &as = am2.pageAnnots(0);
    QCOMPARE(as.size(), size_t(1));
    QCOMPARE(as[0].type, AnnotType::Text);
    QCOMPARE(as[0].contents, QStringLiteral("a sticky comment"));
}

void TstAnnotModel::editContentsColorAndDelete()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString src = writeTemp(dir, QStringLiteral("plain.pdf"), makePlainPdf());
    QVERIFY(!src.isEmpty());

    RenderEngine engine;
    QString err;
    std::unique_ptr<Document> doc = engine.openDocument(src, QString(), &err);
    QVERIFY2(doc != nullptr, qPrintable(err));

    AnnotModel am(*doc);
    const int id = am.addTextMarkup(0, AnnotType::Highlight, {QRectF(20, 30, 100, 12)},
                                    QColor(255, 242, 0), QStringLiteral("tester"));
    QVERIFY(id >= 0);

    QVERIFY(am.setContents(0, id, QStringLiteral("added later")));
    QVERIFY(!am.setContents(0, id, QStringLiteral("added later"))); // no-op second time
    QCOMPARE(am.annot(0, id)->contents, QStringLiteral("added later"));

    QVERIFY(am.setColor(0, id, QColor(0, 200, 0)));
    QVERIFY(am.annot(0, id)->color.green() > 150);

    QVERIFY(am.remove(0, id));
    QVERIFY(am.pageAnnots(0).empty());
    QVERIFY(!am.annot(0, id).has_value());
}

void TstAnnotModel::nonLatinCommentSurvivesSave()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString src = writeTemp(dir, QStringLiteral("plain.pdf"), makePlainPdf());
    QVERIFY(!src.isEmpty());

    RenderEngine engine;
    QString err;
    std::unique_ptr<Document> doc = engine.openDocument(src, QString(), &err);
    QVERIFY2(doc != nullptr, qPrintable(err));

    const QString comment = QString::fromUtf8("\xC3\xA5\xC3\xA4\xC3\xB6 \xE2\x82\xAC"); // "åäö €"
    AnnotModel am(*doc);
    const int id = am.addTextMarkup(0, AnnotType::Highlight, {QRectF(20, 30, 100, 12)},
                                    QColor(255, 242, 0), QString::fromUtf8("\xC3\x85sa"), comment);
    QVERIFY(id >= 0);

    const QString out = dir.filePath(QStringLiteral("annotated.pdf"));
    QString serr;
    QVERIFY2(doc->savePdfTo(out, &serr), qPrintable(serr));

    std::unique_ptr<Document> doc2 = engine.openDocument(out, QString(), &err);
    QVERIFY2(doc2 != nullptr, qPrintable(err));
    AnnotModel am2(*doc2);
    QCOMPARE(am2.pageAnnots(0).at(0).contents, comment);
}

QTEST_GUILESS_MAIN(TstAnnotModel)
#include "tst_annot_model.moc"
