#include "render/Document.h"
#include "render/MeasureContent.h"
#include "security/DocumentOutput.h"
#include "security/MeasureExport.h"
#include "security/QpdfService.h"
#include "render/FormModel.h"
#include "render/FormTypes.h"
#include "render/RenderEngine.h"

#include <mupdf/fitz.h>
#include <mupdf/pdf.h>
#include <qpdf/Buffer.hh>
#include <qpdf/QPDF.hh>

#include <QByteArray>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QTest>

#include <cmath>
#include <memory>

using namespace mervin;

// Generated AcroForms cover enumeration, editing, Unicode, and encrypted output.
class TstFormModel : public QObject
{
    Q_OBJECT

private slots:
    void nonFormDocumentReportsNoForm();
    void enumeratesTextField();
    void fillSaveReopenRoundTrip();
    void nonLatinValueSurvivesSave();
    void sharedFieldsRefreshAndPersist_data();
    void sharedFieldsRefreshAndPersist();
    void unrelatedMalformedWidgetDoesNotBlockEdit_data();
    void unrelatedMalformedWidgetDoesNotBlockEdit();
    void listAppearancesInheritOptionsAndTopIndex();
    void savedListSelections_data();
    void savedListSelections();
    void choiceEditsUseExportValuesAndRefreshIndices();
    void choiceIndicesDistinguishDuplicateLabels();
    void editableComboCustomValuesRemainSupported();
    void encryptedSourceStaysEncrypted();
};

namespace {

// Assemble a syntactically valid single-page PDF from object bodies, computing a
// correct cross-reference table. `bodies[i]` is the dictionary/stream text for
// object i+1; object 0 is the standard free head.
QByteArray assemblePdf(const QList<QByteArray> &bodies, const QByteArray &trailerExtra)
{
    QByteArray pdf = "%PDF-1.7\n";
    QList<int> offsets;
    for (int i = 0; i < bodies.size(); ++i) {
        offsets << pdf.size();
        pdf += QByteArray::number(i + 1) + " 0 obj\n" + bodies[i] + "\nendobj\n";
    }
    const int xrefOff = pdf.size();
    const int n = bodies.size() + 1; // include the free object 0
    pdf += "xref\n0 " + QByteArray::number(n) + "\n";
    pdf += "0000000000 65535 f \n";
    for (int off : offsets) {
        QByteArray rec = QByteArray::number(off);
        while (rec.size() < 10)
            rec.prepend('0');
        pdf += rec + " 00000 n \n";
    }
    pdf += "trailer\n<< /Size " + QByteArray::number(n) + " /Root 1 0 R " + trailerExtra
           + ">>\nstartxref\n" + QByteArray::number(xrefOff) + "\n%%EOF\n";
    return pdf;
}

// A one-page PDF carrying a single fillable text field "field1".
QByteArray makeTextFormPdf()
{
    QList<QByteArray> objs;
    objs << "<< /Type /Catalog /Pages 2 0 R /AcroForm << /Fields [4 0 R] /DR << /Font << "
            "/Helv 5 0 R >> >> /DA (/Helv 0 Tf 0 g) >> >>";
    objs << "<< /Type /Pages /Kids [3 0 R] /Count 1 >>";
    objs << "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 300 300] /Annots [4 0 R] "
            "/Resources << /Font << /Helv 5 0 R >> >> >>";
    objs << "<< /Type /Annot /Subtype /Widget /FT /Tx /T (field1) /Rect [20 250 280 270] "
            "/F 4 /DA (/Helv 12 Tf 0 g) /V () /P 3 0 R >>";
    objs << "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>";
    return assemblePdf(objs, QByteArray());
}

// A one-page PDF with no AcroForm.
QByteArray makePlainPdf()
{
    QList<QByteArray> objs;
    objs << "<< /Type /Catalog /Pages 2 0 R >>";
    objs << "<< /Type /Pages /Kids [3 0 R] /Count 1 >>";
    objs << "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 300 300] /Resources << >> >>";
    return assemblePdf(objs, QByteArray());
}

// Shared text, choice, checkbox and radio widgets cross pages. The second page
// deliberately omits /P, and every widget starts with a stored appearance.
QByteArray makeSharedFormPdf(bool listBox, int topIndex = 0, bool inheritChoice = true,
                             bool exportValues = false, bool duplicateLabels = false)
{
    QList<QByteArray> objs{
        "<< /Type /Catalog /Pages 2 0 R /AcroForm << /Fields [6 0 R 9 0 R 12 0 R 15 0 R 18 0 R 19 0 R 20 0 R 21 0 R] /DR << /Font << /Helv 5 0 R >> >> /DA (/Helv 12 Tf 0 g) >> >>",
        "<< /Type /Pages /Kids [3 0 R 4 0 R] /Count 2 >>",
        "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 300 300] /Annots [7 0 R 10 0 R 13 0 R 16 0 R 18 0 R 19 0 R 20 0 R 21 0 R] /Resources << >> >>",
        "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 300 300] /Annots [8 0 R 11 0 R 14 0 R 17 0 R] /Resources << >> >>",
        "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>",
        "<< /FT /Tx /T (shared-text) /V () /Kids [7 0 R 8 0 R] >>",
        "<< /Type /Annot /Subtype /Widget /Parent 6 0 R /P 3 0 R /Rect [20 250 280 270] /F 4 /AP << /N 22 0 R >> >>",
        "<< /Type /Annot /Subtype /Widget /Parent 6 0 R /Rect [20 250 280 270] /F 4 /AP << /N 22 0 R >> >>",
        "<< /FT /Ch /Ff 131072 /T (shared-choice) /V (Red) /Opt [(Red) (Green)] /Kids [10 0 R 11 0 R] >>",
        "<< /Type /Annot /Subtype /Widget /Parent 9 0 R /P 3 0 R /Rect [20 210 280 230] /F 4 /AP << /N 22 0 R >> >>",
        "<< /Type /Annot /Subtype /Widget /Parent 9 0 R /Rect [20 210 280 230] /F 4 /AP << /N 22 0 R >> >>",
        "<< /FT /Btn /T (shared-check) /V /Off /Kids [13 0 R 14 0 R] >>",
        "<< /Type /Annot /Subtype /Widget /Parent 12 0 R /P 3 0 R /Rect [20 160 40 180] /F 4 /AS /Off /AP << /N << /Off 22 0 R /Yes 23 0 R >> >> >>",
        "<< /Type /Annot /Subtype /Widget /Parent 12 0 R /Rect [20 160 40 180] /F 4 /AS /Off /AP << /N << /Off 22 0 R /Yes 23 0 R >> >> >>",
        "<< /FT /Btn /Ff 49152 /T (shared-radio) /V /A /Kids [16 0 R 17 0 R] >>",
        "<< /Type /Annot /Subtype /Widget /Parent 15 0 R /P 3 0 R /Rect [60 160 80 180] /F 4 /AS /A /AP << /N << /Off 22 0 R /A 23 0 R >> >> >>",
        "<< /Type /Annot /Subtype /Widget /Parent 15 0 R /Rect [60 160 80 180] /F 4 /AS /Off /AP << /N << /Off 22 0 R /B 23 0 R >> >> >>",
        "<< /Type /Annot /Subtype /Widget /FT /Btn /T (independent-check) /V /Off /Rect [100 160 120 180] /F 4 /AS /Off /AP << /N << /Off 22 0 R /Yes 23 0 R >> >> >>",
        "<< /Type /Annot /Subtype /Widget /FT /Btn /Ff 49152 /T (independent-radio) /V /C /Rect [140 160 160 180] /F 4 /AS /C /AP << /N << /Off 22 0 R /C 23 0 R >> >> >>",
        "<< /Type /Annot /Subtype /Widget /FT /Tx /Ff 1 /T (read-only) /V (locked) /Rect [20 80 280 100] /F 4 /AP << /N 22 0 R >> >>",
        "<< /Type /Annot /Subtype /Widget /FT /Sig /T (signature) /Rect [20 40 280 60] /F 4 /AP << /N 22 0 R >> >>"};
    if (listBox) {
        objs[8].replace("/Ff 131072", "/Ff 0");
        const QByteArray entries = "/Opt [(Red) (Green)] /TI " + QByteArray::number(topIndex);
        objs[8].replace("/Opt [(Red) (Green)]", inheritChoice ? entries : QByteArray());
        if (!inheritChoice)
            for (int index : {9, 10})
                objs[index].replace("/Parent 9 0 R", "/Parent 9 0 R " + entries);
    }
    if (exportValues) {
        objs[8].replace("/V (Red)", "/V (r) /I [0]");
        objs[8].replace("/Opt [(Red) (Green)]",
                        duplicateLabels
                            ? "/Opt [[(r) (Same)] [(g) (Same)] [(g) (Other green)]]"
                            : "/Opt [[(r) (Red)] [(g) (Green)] [(g) (Other green)]]");
        if (listBox)
            for (int index : {9, 10})
                objs[index].replace("[20 210 280 230]", "[20 150 280 230]");
    }
    for (const QByteArray &content : {QByteArray(""), QByteArray("0 g 2 2 16 16 re f\n")})
        objs << "<< /Type /XObject /Subtype /Form /BBox [0 0 20 20] /Resources << >> /Length "
                    + QByteArray::number(content.size()) + " >>\nstream\n" + content + "endstream";
    return assemblePdf(objs, {});
}

// Two valid widgets share a text field. The unrelated widget has a cyclic
// parent, either on its own page or beside the valid sibling before/after it.
QByteArray makeMalformedWidgetFormPdf(int placement)
{
    QList<QByteArray> objs{
        "<< /Type /Catalog /Pages 2 0 R /AcroForm << /Fields [7 0 R] /DR << /Font << /Helv 6 0 R >> >> /DA (/Helv 12 Tf 0 g) >> >>",
        "<< /Type /Pages /Kids [3 0 R 4 0 R 5 0 R] /Count 3 >>",
        "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 300 300] /Annots [8 0 R] /Resources << >> >>",
        "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 300 300] /Annots [9 0 R] /Resources << >> >>",
        "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 300 300] /Annots [] /Resources << >> >>",
        "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>",
        "<< /FT /Tx /T (shared) /V (Old) /Kids [8 0 R 9 0 R] >>",
        "<< /Type /Annot /Subtype /Widget /Parent 7 0 R /P 3 0 R /Rect [20 250 280 270] /F 4 /AP << /N 11 0 R >> >>",
        "<< /Type /Annot /Subtype /Widget /Parent 7 0 R /Rect [20 250 280 270] /F 4 /AP << /N 11 0 R >> >>",
        "<< /Type /Annot /Subtype /Widget /FT /Tx /Ff 0 /Parent 10 0 R /V () /DA (/Helv 12 Tf 0 g) /Q 0 /Rect [20 210 280 230] /F 4 /AP << /N 11 0 R >> >>"};
    if (placement == 0)
        objs[4].replace("/Annots []", "/Annots [10 0 R]");
    else
        objs[3].replace("/Annots [9 0 R]", placement == 1
            ? "/Annots [10 0 R 9 0 R]" : "/Annots [9 0 R 10 0 R]");
    const QByteArray content = "BT /Helv 12 Tf 2 5 Td (Old) Tj ET\n";
    objs << "<< /Type /XObject /Subtype /Form /BBox [0 0 260 20] /Resources << /Font << /Helv 6 0 R >> >> /Length "
                + QByteArray::number(content.size()) + " >>\nstream\n" + content + "endstream";
    return assemblePdf(objs, {});
}

// Write `bytes` to `dir`/`name` and return the absolute path.
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

void TstFormModel::nonFormDocumentReportsNoForm()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = writeTemp(dir, QStringLiteral("plain.pdf"), makePlainPdf());
    QVERIFY(!path.isEmpty());

    RenderEngine engine;
    QString err;
    std::unique_ptr<Document> doc = engine.openDocument(path, QString(), &err);
    QVERIFY2(doc != nullptr, qPrintable(err));
    QVERIFY(!doc->hasForm());

    FormModel fm(*doc);
    QVERIFY(fm.pageFields(0).empty());
    QVERIFY(!fm.isDirty());
}

void TstFormModel::enumeratesTextField()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = writeTemp(dir, QStringLiteral("form.pdf"), makeTextFormPdf());
    QVERIFY(!path.isEmpty());

    RenderEngine engine;
    QString err;
    std::unique_ptr<Document> doc = engine.openDocument(path, QString(), &err);
    QVERIFY2(doc != nullptr, qPrintable(err));
    QVERIFY(doc->hasForm());

    FormModel fm(*doc);
    const std::vector<FormField> &fields = fm.pageFields(0);
    QCOMPARE(fields.size(), size_t(1));
    QCOMPARE(fields[0].type, FormFieldType::Text);
    QCOMPARE(fields[0].name, QStringLiteral("field1"));
    QVERIFY(fields[0].value.isEmpty());
    QVERIFY(fields[0].editable());
    // The widget /Rect [20 250 280 270] on a 300x300 page maps to app page-point
    // space (top-left origin, y-down): y0 = 300 - 270 = 30, height = 20.
    QVERIFY(std::abs(fields[0].rect.x() - 20.0) < 0.5);
    QVERIFY(std::abs(fields[0].rect.width() - 260.0) < 0.5);
    QVERIFY(std::abs(fields[0].rect.y() - 30.0) < 0.5);
    QVERIFY(std::abs(fields[0].rect.height() - 20.0) < 0.5);
}

void TstFormModel::fillSaveReopenRoundTrip()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString src = writeTemp(dir, QStringLiteral("form.pdf"), makeTextFormPdf());
    QVERIFY(!src.isEmpty());

    RenderEngine engine;
    QString err;
    std::unique_ptr<Document> doc = engine.openDocument(src, QString(), &err);
    QVERIFY2(doc != nullptr, qPrintable(err));

    FormModel fm(*doc);
    QVERIFY(!fm.isDirty());
    QVERIFY(fm.setTextValue(0, 0, QStringLiteral("Hello")));
    QVERIFY(fm.isDirty());
    // Setting the same value again is a no-op (no spurious dirtying / re-render).
    QVERIFY(!fm.setTextValue(0, 0, QStringLiteral("Hello")));

    const QString out = dir.filePath(QStringLiteral("filled.pdf"));
    QString serr;
    QVERIFY2(fm.saveTo(out, &serr), qPrintable(serr));
    QVERIFY(QFileInfo::exists(out));

    // Reopen the saved file: the value persists as standard AcroForm /V, so a
    // fresh enumeration reads it back without any private blob.
    std::unique_ptr<Document> doc2 = engine.openDocument(out, QString(), &err);
    QVERIFY2(doc2 != nullptr, qPrintable(err));
    QVERIFY(doc2->hasForm());
    FormModel fm2(*doc2);
    const std::vector<FormField> &fields = fm2.pageFields(0);
    QCOMPARE(fields.size(), size_t(1));
    QCOMPARE(fields[0].value, QStringLiteral("Hello"));
}

void TstFormModel::nonLatinValueSurvivesSave()
{
    // The reason forms save via MuPDF (not qpdf): non-Latin values must round-trip.
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString src = writeTemp(dir, QStringLiteral("form.pdf"), makeTextFormPdf());
    QVERIFY(!src.isEmpty());

    RenderEngine engine;
    QString err;
    std::unique_ptr<Document> doc = engine.openDocument(src, QString(), &err);
    QVERIFY2(doc != nullptr, qPrintable(err));

    const QString value = QString::fromUtf8("\xC3\xA5\xC3\xA4\xC3\xB6 \xE2\x82\xAC "
                                          "\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E");
    FormModel fm(*doc);
    QVERIFY(fm.setTextValue(0, 0, value));

    const QString out = dir.filePath(QStringLiteral("filled.pdf"));
    QString serr;
    QVERIFY2(fm.saveTo(out, &serr), qPrintable(serr));

    std::unique_ptr<Document> doc2 = engine.openDocument(out, QString(), &err);
    QVERIFY2(doc2 != nullptr, qPrintable(err));
    FormModel fm2(*doc2);
    QCOMPARE(fm2.pageFields(0).at(0).value, value);
}

void TstFormModel::sharedFieldsRefreshAndPersist_data()
{
    QTest::addColumn<bool>("listBox");
    QTest::newRow("combo") << false;
    QTest::newRow("list") << true;
}

void TstFormModel::sharedFieldsRefreshAndPersist()
{
    QFETCH(bool, listBox);
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString source = writeTemp(dir, QStringLiteral("shared.pdf"), makeSharedFormPdf(listBox));
    RenderEngine engine;
    auto doc = engine.openDocument(source);
    QVERIFY(doc);
    FormModel form(*doc);
    QCOMPARE(form.pageFields(0).size(), size_t(8));
    QCOMPARE(form.pageFields(1).size(), size_t(4));
    QVERIFY(!form.pageFields(0)[6].editable());
    QCOMPARE(form.pageFields(0)[7].type, FormFieldType::Signature);
    QVERIFY(!form.pageFields(0)[7].editable());
    QCOMPARE(form.pageFields(0)[1].options, QStringList({"Red", "Green"}));
    QCOMPARE(form.pageFields(0)[1].type, listBox ? FormFieldType::ListBox : FormFieldType::ComboBox);
    const QImage before0 = engine.renderPageImage(doc.get(), 0, 1, 0);
    const QImage before1 = engine.renderPageImage(doc.get(), 1, 1, 0);
    QVERIFY(!before0.isNull() && !before1.isNull());
    QVERIFY(!form.setTextValue(-1, 0, QStringLiteral("Rejected")));
    QVERIFY(!form.toggle(1, 99));
    QVERIFY(!form.isDirty());

    QVERIFY(form.setTextValue(0, 0, QStringLiteral("Shared text")));
    const std::vector<std::pair<int, int>> textWidgets{{0, 0}, {1, 0}};
    QCOMPARE(form.changedFields(), textWidgets);
    QCOMPARE(form.pageFields(1)[0].value, QStringLiteral("Shared text"));
    QVERIFY(form.setChoiceValue(1, 1, QStringLiteral("Green")));
    QCOMPARE(form.pageFields(0)[1].value, QStringLiteral("Green"));
    QVERIFY(form.toggle(0, 2));
    QCOMPARE(form.pageFields(1)[2].value, QStringLiteral("Yes"));
    QVERIFY(form.toggle(1, 3));
    QCOMPARE(form.pageFields(0)[3].value, QStringLiteral("B"));
    QCOMPARE(form.pageFields(0)[4].value, QStringLiteral("Off"));
    QCOMPARE(form.pageFields(0)[5].value, QStringLiteral("C"));
    form.clearDirty();
    QVERIFY(!form.toggle(1, 3)); // the selected no-toggle-to-off radio is a no-op
    QVERIFY(!form.isDirty());
    QVERIFY(form.changedFields().empty());
    QVERIFY(!form.setChoiceValue(0, 1, QStringLiteral("Green")));

    // Save before rendering or enumerating again can repair any stale appearance.
    const QString saved = dir.filePath(QStringLiteral("saved.pdf"));
    QString error;
    QVERIFY2(form.saveTo(saved, &error), qPrintable(error));
    auto reopened = engine.openDocument(saved);
    QVERIFY(reopened);
    FormModel loaded(*reopened);
    for (int page = 0; page < 2; ++page) {
        const auto &fields = loaded.pageFields(page);
        QCOMPARE(fields[0].value, QStringLiteral("Shared text"));
        QCOMPARE(fields[1].value, QStringLiteral("Green"));
        QCOMPARE(fields[2].value, QStringLiteral("Yes"));
        QCOMPARE(fields[3].value, QStringLiteral("B"));
        const QImage rendered = engine.renderPageImage(reopened.get(), page, 1, 0);
        QVERIFY(!rendered.isNull());
        const QImage before = page == 0 ? before0 : before1;
        QVERIFY(rendered.copy(20, 30, 260, 20) != before.copy(20, 30, 260, 20));
        // Options and selection ink must persist in saved appearances.
        QVERIFY(rendered.copy(20, 70, 260, 20) != before.copy(20, 70, 260, 20));
        QVERIFY(rendered.pixelColor(30, 130).lightness() < 50);
        QCOMPARE(rendered.pixelColor(70, 130).lightness() < 50, page == 1);
        QCOMPARE(rendered, engine.renderPageImage(doc.get(), page, 1, 0));
    }
    QVERIFY(loaded.toggle(1, 2));
    QCOMPARE(loaded.pageFields(0)[2].value, QStringLiteral("Off"));
}

void TstFormModel::unrelatedMalformedWidgetDoesNotBlockEdit_data()
{
    QTest::addColumn<int>("placement");
    QTest::newRow("unrelated-page") << 0;
    QTest::newRow("before-sibling") << 1;
    QTest::newRow("after-sibling") << 2;
}

void TstFormModel::unrelatedMalformedWidgetDoesNotBlockEdit()
{
    QFETCH(int, placement);
    QTemporaryDir files;
    QVERIFY(files.isValid());
    const QString source = writeTemp(files, QStringLiteral("malformed-widget.pdf"),
                                     makeMalformedWidgetFormPdf(placement));
    RenderEngine engine;
    auto doc = engine.openDocument(source);
    QVERIFY(doc);
    FormModel form(*doc);
    QCOMPARE(form.pageFields(0).size(), size_t(1));
    QVERIFY(form.pageFields(0)[0].editable());
    QCOMPARE(form.pageFields(0)[0].value, QStringLiteral("Old"));
    if (placement == 0)
        QCOMPARE(form.pageFields(1)[0].value, QStringLiteral("Old"));
    QVERIFY(!form.isDirty());

    QVERIFY(form.setTextValue(0, 0, QStringLiteral("Changed")));
    QVERIFY(form.isDirty());
    const std::vector<std::pair<int, int>> affected{{0, 0}, {1, placement == 1 ? 1 : 0}};
    QCOMPARE(form.changedFields(), affected);
    const QString saved = files.filePath(QStringLiteral("saved.pdf"));
    QString error;
    QVERIFY2(form.saveTo(saved, &error), qPrintable(error));
    QCOMPARE(form.pageFields(0)[0].value, QStringLiteral("Changed"));
    if (placement == 0)
        QCOMPARE(form.pageFields(1)[0].value, QStringLiteral("Changed"));
    auto reopened = engine.openDocument(saved);
    QVERIFY(reopened);
    QCOMPARE(FormModel(*reopened).pageFields(0)[0].value, QStringLiteral("Changed"));

    // A malformed target must fail without clearing an earlier successful edit.
    const int badPage = placement == 0 ? 2 : 1;
    const int badIndex = placement == 2 ? 1 : 0;
    QVERIFY(!form.setTextValue(badPage, badIndex, QStringLiteral("Rejected")));
    QVERIFY(form.isDirty());
    QVERIFY(form.changedFields().empty());
    form.clearDirty();
    QVERIFY(!form.setTextValue(0, 0, QStringLiteral("Changed")));
    QVERIFY(!form.isDirty());
    QVERIFY(form.changedFields().empty());
    // A malformed target must still fail before changing a value or dirty state.
    QVERIFY(!form.setTextValue(badPage, badIndex, QStringLiteral("Rejected")));
    QVERIFY(!form.isDirty());
    QVERIFY(form.changedFields().empty());
    QCOMPARE(form.pageFields(0)[0].value, QStringLiteral("Changed"));
}

void TstFormModel::listAppearancesInheritOptionsAndTopIndex()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    RenderEngine engine;
    QImage firstRows[2];
    for (int top = 0; top < 2; ++top) {
        QImage widgetLocal;
        for (bool inherited : {false, true}) {
            const QString name = QStringLiteral("list-%1-%2.pdf").arg(top).arg(inherited);
            const QString source = writeTemp(dir, name, makeSharedFormPdf(true, top, inherited));
            auto doc = engine.openDocument(source);
            QVERIFY(doc);
            FormModel form(*doc);
            QCOMPARE(form.pageFields(0).size(), size_t(8));
            QCOMPARE(form.pageFields(1).size(), size_t(4));
            QVERIFY(form.setChoiceValue(0, 1, QStringLiteral("Green")));
            const QString saved = dir.filePath(QStringLiteral("saved-") + name);
            QVERIFY(form.saveTo(saved));
            auto reopened = engine.openDocument(saved);
            QVERIFY(reopened);
            FormModel loaded(*reopened);
            for (int page = 0; page < 2; ++page) {
                const auto &field = loaded.pageFields(page)[1];
                QCOMPARE(field.type, FormFieldType::ListBox);
                QCOMPARE(field.value, QStringLiteral("Green"));
                QCOMPARE(field.options, QStringList({"Red", "Green"}));
                const QImage image = engine.renderPageImage(reopened.get(), page, 1, 0);
                QVERIFY(!image.isNull());
                const QImage row = image.copy(20, 70, 260, 20);
                int ink = 0;
                for (int y = 0; y < row.height(); ++y)
                    for (int x = 0; x < row.width(); ++x)
                        ink += row.pixelColor(x, y).lightness() < 150;
                QVERIFY(ink > 10);
                if (!inherited && page == 0)
                    widgetLocal = row;
                else
                    QCOMPARE(row, widgetLocal);
                if (inherited)
                    firstRows[top] = row;
            }
        }
    }
    // /V is Green in both files. Different top rows prove /TI is respected.
    QVERIFY(firstRows[0] != firstRows[1]);
}

void TstFormModel::savedListSelections_data()
{
    QTest::addColumn<QByteArray>("options");
    QTest::addColumn<QByteArray>("selection");
    QTest::addColumn<QList<int>>("selected");
    QTest::addColumn<int>("top");
    const QByteArray plain = "[(one) (two) (three)]";
    const QByteArray pairs = "[[(one) (First)] [(two) (Second)] [(three) (Third)]]";
    QTest::newRow("single") << plain << QByteArray("/V (two)") << QList<int>{1} << 0;
    QTest::newRow("multiple") << plain << QByteArray("/V [(one) (three)]") << QList<int>({0, 2}) << 0;
    QTest::newRow("export-display") << pairs << QByteArray("/V (two)") << QList<int>{1} << 0;
    QTest::newRow("export-display-multiple") << pairs << QByteArray("/V [(one) (three)]")
                                            << QList<int>({0, 2}) << 0;
    QTest::newRow("display-is-not-export") << pairs << QByteArray("/V (Second)") << QList<int>{} << 0;
    QTest::newRow("unset") << plain << QByteArray() << QList<int>{} << 0;
    QTest::newRow("unknown") << plain << QByteArray("/V (missing)") << QList<int>{} << 0;
    QTest::newRow("empty-multiple") << plain << QByteArray("/V [] /I []") << QList<int>{} << 0;
    QTest::newRow("scrolled") << plain << QByteArray("/V [(one) (three)]") << QList<int>({0, 2}) << 1;
    QTest::newRow("negative-top") << plain << QByteArray("/V (two)") << QList<int>{1} << -4;
    QTest::newRow("past-end") << plain << QByteArray("/V (two)") << QList<int>{1} << 9;
    QTest::newRow("duplicate-export-indices")
        << QByteArray("[[(same) (First)] [(same) (Second)] [(other) (Third)]]")
        << QByteArray("/V (same) /I [1]") << QList<int>{1} << 0;
    QTest::newRow("invalid-index") << plain << QByteArray("/V (two) /I [-1 99 (1)]")
                                   << QList<int>{} << 0;
    QTest::newRow("unicode-rows")
        << QByteArray("[[(one) <FEFF65E5672C>] [(two) (Second)] [(three) <FEFF06270644063906310628064A0629>]]")
        << QByteArray("/V [(one) (three)]") << QList<int>({0, 2}) << 0;
}

/// Reads /AP with qpdf before rendering, then compares selected and clear rasters on both pages.
void TstFormModel::savedListSelections()
{
    QFETCH(QByteArray, options);
    QFETCH(QByteArray, selection);
    QFETCH(QList<int>, selected);
    QFETCH(int, top);
    QTemporaryDir files;
    QVERIFY(files.isValid());
    RenderEngine engine;
    QImage local[2];
    for (bool inherited : {false, true}) {
        QImage clear[2];
        for (bool populated : {false, true}) {
            const QByteArray entries = "/Opt " + options + " /TI " + QByteArray::number(top)
                + (populated ? " " + selection : QByteArray());
            const QByteArray inheritedEntries = inherited ? entries : QByteArray();
            const QByteArray widgetEntries = inherited ? QByteArray() : entries;
            const QByteArray bytes = assemblePdf({
                "<< /Type /Catalog /Pages 2 0 R /AcroForm << /Fields [6 0 R] /DR << /Font << /Helv 5 0 R >> >> /DA (/Helv 12 Tf 0 g) >> >>",
                "<< /Type /Pages /Kids [3 0 R 4 0 R] /Count 2 >>",
                "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 300 300] /Annots [7 0 R] /Resources << >> >>",
                "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 300 300] /Annots [8 0 R] /Resources << >> >>",
                "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>",
                "<< /FT /Ch /Ff 2097152 /T (shared) /Kids [7 0 R 8 0 R] " + inheritedEntries + " >>",
                "<< /Type /Annot /Subtype /Widget /Parent 6 0 R /P 3 0 R /Rect [20 150 280 230] /BS << /W 1 >> /MK << /BG [1] /BC [0] >> /F 4 " + widgetEntries + " >>",
                "<< /Type /Annot /Subtype /Widget /Parent 6 0 R /Rect [20 150 280 230] /BS << /W 1 >> /MK << /BG [1] /BC [0] >> /F 4 " + widgetEntries + " >>"
            }, {});
            const QString name = QStringLiteral("%1-%2.pdf").arg(inherited).arg(populated);
            const QString source = writeTemp(files, name, bytes);
            auto doc = engine.openDocument(source);
            QVERIFY(doc);
            // Force persisted appearances without changing existing multi-selection values.
            // Mervin's editor currently offers one choice at a time.
            bool updated = false;
            doc->withPdfDocument([&](fz_context *ctx, pdf_document *pdf) {
                pdf_page *page = nullptr;
                fz_var(page);
                fz_try(ctx) {
                    for (int i = 0; i < 2; ++i) {
                        page = pdf_load_page(ctx, pdf, i);
                        pdf_annot_request_resynthesis(ctx, pdf_first_widget(ctx, page));
                        pdf_update_page(ctx, page);
                        fz_drop_page(ctx, &page->super);
                        page = nullptr;
                    }
                    updated = true;
                }
                fz_always(ctx) { fz_drop_page(ctx, page ? &page->super : nullptr); }
                fz_catch(ctx) { updated = false; }
            });
            QVERIFY(updated);
            const QString saved = files.filePath(QStringLiteral("saved-") + name);
            QVERIFY(FormModel(*doc).saveTo(saved));
            QPDF inspected;
            inspected.processFile(saved.toUtf8().constData());
            int visibleSelections = 0;
            for (int index : selected)
                visibleSelections += index >= std::max(0, top);
            for (auto page : inspected.getAllPages()) {
                auto appearance = page.getKey("/Annots").getArrayItem(0).getKey("/AP").getKey("/N");
                QVERIFY(appearance.isStream());
                const auto stream = appearance.getStreamData();
                const QByteArray drawing(reinterpret_cast<const char *>(stream->getBuffer()),
                                         static_cast<qsizetype>(stream->getSize()));
                QCOMPARE(drawing.count("0.153 0.392 0.769 rg"), populated ? visibleSelections : 0);
            }
            auto reopened = engine.openDocument(saved);
            QVERIFY(reopened);
            for (int page = 0; page < 2; ++page) {
                const QImage image = engine.renderPageImage(reopened.get(), page, 1, 0);
                QVERIFY(!image.isNull());
                if (!populated) {
                    clear[page] = image;
                    continue;
                }
                for (int index = std::max(0, top); index < 3; ++index) {
                    const int y = qRound(72 + (index - std::max(0, top) + 0.5) * 14.4);
                    const QColor color = image.pixelColor(270, y);
                    const bool highlighted = color.blue() > color.red() + 80;
                    QCOMPARE(highlighted, selected.contains(index));
                    const QRect row(22, y - 3, 256, 6);
                    QCOMPARE(image.copy(row) != clear[page].copy(row), selected.contains(index));
                }
                QCOMPARE(image != clear[page], visibleSelections > 0);
                if (!inherited)
                    local[page] = image;
                else
                    QCOMPARE(image, local[page]);
                QCOMPARE(image, engine.renderPageImage(doc.get(), page, 1, 0));
            }
        }
        QCOMPARE(local[0], local[1]);
    }
}

/// Choice labels round-trip through export values and replace stale selection indices.
void TstFormModel::choiceEditsUseExportValuesAndRefreshIndices()
{
    QTemporaryDir files;
    QVERIFY(files.isValid());
    RenderEngine engine;
    for (bool list : {false, true}) {
        const QString source = writeTemp(files, QStringLiteral("pairs-%1.pdf").arg(list),
                                         makeSharedFormPdf(list, 0, true, true));
        auto doc = engine.openDocument(source);
        QVERIFY(doc);
        FormModel form(*doc);
        QCOMPARE(form.pageFields(0)[1].value, QStringLiteral("Red"));
        QCOMPARE(form.pageFields(0)[1].choiceIndex, 0);
        QVERIFY(!form.setChoiceValue(0, 1, QStringLiteral("missing")));
        QVERIFY(!form.setChoiceValue(0, 0, QStringLiteral("Green")));
        QVERIFY(!form.isDirty());
        QVERIFY(form.setChoiceValue(1, 1, QStringLiteral("Green")));
        QCOMPARE(form.pageFields(0)[1].value, QStringLiteral("Green"));
        QVERIFY(!form.setChoiceValue(0, 1, QStringLiteral("Green")));
        // Identical export values still identify different selectable rows.
        QVERIFY(form.setChoiceValue(0, 1, QStringLiteral("Other green")));
        QCOMPARE(form.pageFields(1)[1].value, QStringLiteral("Other green"));
        QCOMPARE(form.pageFields(1)[1].choiceIndex, 2);
        const QString output = files.filePath(QStringLiteral("pairs-saved-%1.pdf").arg(list));
        QVERIFY(form.saveTo(output));
        QPDF inspected;
        inspected.processFile(output.toUtf8().constData());
        for (auto page : inspected.getAllPages()) {
            auto widget = page.getKey("/Annots").getArrayItem(1);
            auto field = widget.getKey("/Parent");
            QCOMPARE(field.getKey("/V").getUTF8Value(), std::string("g"));
            QCOMPARE(field.getKey("/I").getArrayNItems(), 1);
            QCOMPARE(field.getKey("/I").getArrayItem(0).getIntValue(), 2);
            auto appearance = widget.getKey("/AP").getKey("/N");
            if (list)
                appearance = appearance.getDict().getKey("/Resources").getKey("/XObject").getKey("/Opt2");
            QVERIFY(appearance.isStream());
            const auto stream = appearance.getStreamData();
            const QByteArray drawing(reinterpret_cast<const char *>(stream->getBuffer()),
                                     static_cast<qsizetype>(stream->getSize()));
            QVERIFY(drawing.contains("(Other green)"));
        }
        auto reopened = engine.openDocument(output);
        QVERIFY(reopened);
        FormModel loaded(*reopened);
        QCOMPARE(loaded.pageFields(0)[1].value, QStringLiteral("Other green"));
        QCOMPARE(loaded.pageFields(1)[1].value, QStringLiteral("Other green"));
        QCOMPARE(loaded.pageFields(0)[1].choiceIndex, 2);
        QCOMPARE(loaded.pageFields(1)[1].choiceIndex, 2);
        QVERIFY(!loaded.setChoiceValue(1, 1, QStringLiteral("Other green")));
        QVERIFY(!loaded.isDirty());
    }
}

/// Equal display labels remain distinct through shared widgets and saved /V and /I.
void TstFormModel::choiceIndicesDistinguishDuplicateLabels()
{
    QTemporaryDir files;
    QVERIFY(files.isValid());
    RenderEngine engine;
    for (bool list : {false, true}) {
        const QString source = writeTemp(files, QStringLiteral("duplicate-labels-%1.pdf").arg(list),
                                         makeSharedFormPdf(list, 0, true, true, true));
        auto doc = engine.openDocument(source);
        QVERIFY(doc);
        FormModel form(*doc);
        QCOMPARE(form.pageFields(0)[1].value, QStringLiteral("Same"));
        QCOMPARE(form.pageFields(0)[1].choiceIndex, 0);
        QVERIFY(!form.setChoiceValue(0, 1, QStringLiteral("Same"), -2));
        QVERIFY(!form.setChoiceValue(0, 1, QStringLiteral("Same"), 3));
        QVERIFY(!form.isDirty());
        QVERIFY(form.setChoiceValue(1, 1, QStringLiteral("Same"), 1));
        QCOMPARE(form.changedFields().size(), size_t(2));
        for (int page : {0, 1}) {
            QCOMPARE(form.pageFields(page)[1].value, QStringLiteral("Same"));
            QCOMPARE(form.pageFields(page)[1].choiceIndex, 1);
        }
        QVERIFY(!form.setChoiceValue(0, 1, QStringLiteral("Same"), 1));
        const QString output = files.filePath(QStringLiteral("duplicate-labels-saved-%1.pdf").arg(list));
        QVERIFY(form.saveTo(output));
        QPDF inspected;
        inspected.processFile(output.toUtf8().constData());
        for (auto page : inspected.getAllPages()) {
            auto field = page.getKey("/Annots").getArrayItem(1).getKey("/Parent");
            QCOMPARE(field.getKey("/V").getUTF8Value(), std::string("g"));
            QCOMPARE(field.getKey("/I").getArrayNItems(), 1);
            QCOMPARE(field.getKey("/I").getArrayItem(0).getIntValue(), 1);
        }
        auto reopened = engine.openDocument(output);
        QVERIFY(reopened);
        FormModel loaded(*reopened);
        for (int page : {0, 1}) {
            QCOMPARE(loaded.pageFields(page)[1].value, QStringLiteral("Same"));
            QCOMPARE(loaded.pageFields(page)[1].choiceIndex, 1);
        }
        QVERIFY(!loaded.setChoiceValue(1, 1, QStringLiteral("Same"), 1));
        QVERIFY(!loaded.isDirty());
        // Existing label-only callers keep their first matching option behavior.
        QVERIFY(loaded.setChoiceValue(0, 1, QStringLiteral("Same")));
        QCOMPARE(loaded.pageFields(1)[1].choiceIndex, 0);
    }
}

/// Selecting exact options does not remove editable combos' custom-text path.
void TstFormModel::editableComboCustomValuesRemainSupported()
{
    QTemporaryDir files;
    QVERIFY(files.isValid());
    const QList<QByteArray> objects{
        "<< /Type /Catalog /Pages 2 0 R /AcroForm << /Fields [4 0 R] /DR << /Font << /Helv 5 0 R >> >> /DA (/Helv 12 Tf 0 g) >> >>",
        "<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
        "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 300 300] /Annots [4 0 R] /Resources << >> >>",
        "<< /Type /Annot /Subtype /Widget /FT /Ch /Ff 393216 /T (editable) /Rect [20 250 280 270] /F 4 /V (r) /I [0] /Opt [[(r) (Red)] [(g) (Green)]] /P 3 0 R >>",
        "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>"};
    const QString source = writeTemp(files, QStringLiteral("editable-choice.pdf"), assemblePdf(objects, {}));
    RenderEngine engine;
    auto doc = engine.openDocument(source);
    QVERIFY(doc);
    FormModel form(*doc);
    QVERIFY(form.setChoiceValue(0, 0, QStringLiteral("Custom text")));
    QCOMPARE(form.pageFields(0)[0].value, QStringLiteral("Custom text"));
    QCOMPARE(form.pageFields(0)[0].choiceIndex, -1);
    QVERIFY(!form.setChoiceValue(0, 0, QStringLiteral("Custom text")));
    const QString output = files.filePath(QStringLiteral("editable-choice-saved.pdf"));
    QVERIFY(form.saveTo(output));
    QPDF inspected;
    inspected.processFile(output.toUtf8().constData());
    auto field = inspected.getRoot().getKey("/AcroForm").getKey("/Fields").getArrayItem(0);
    QCOMPARE(field.getKey("/V").getUTF8Value(), std::string("Custom text"));
    QVERIFY(field.getKey("/I").isNull());
    QVERIFY(form.setChoiceValue(0, 0, QStringLiteral("Green"), 1));
    QCOMPARE(form.pageFields(0)[0].choiceIndex, 1);
}

void TstFormModel::encryptedSourceStaysEncrypted()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString plain = writeTemp(dir, QStringLiteral("plain.pdf"), makeTextFormPdf());
    const QString path = dir.filePath(QStringLiteral("encrypted.pdf"));
    const QString pw = QStringLiteral("test-password");
    mervin::QpdfService service;
    QCOMPARE(service.encrypt(plain, path, {}, pw, pw, mervin::QpdfService::Algorithm::AES256, {}),
             mervin::QpdfService::Status::Ok);
    RenderEngine engine;
    QString err;
    std::unique_ptr<Document> doc = engine.openDocument(path, pw, &err);
    QVERIFY2(doc != nullptr, qPrintable(err));
    QVERIFY(doc->hasForm());

    FormModel fm(*doc);
    QVERIFY(!fm.pageFields(0).empty());
    fm.setTextValue(0, 0, QStringLiteral("x")); // fill whatever the first field is

    const QString out = dir.filePath(QStringLiteral("filled-enc.pdf"));
    QString serr;
    QVERIFY2(fm.saveTo(out, &serr), qPrintable(serr));

    // Opening with no password must report that one is required (still encrypted).
    bool needsPw = false;
    QString e2;
    std::unique_ptr<Document> noPw = engine.openDocument(out, QString(), &e2, &needsPw);
    QVERIFY(noPw == nullptr);
    QVERIFY(needsPw);
    // With the password it opens again.
    std::unique_ptr<Document> withPw = engine.openDocument(out, pw, &e2);
    QVERIFY2(withPw != nullptr, qPrintable(e2));

    // The application saves through MuPDF and then qpdf to include measurements.
    MeasureDoc measurements;
    measurements.measurements.push_back({0, MeasureKind::Distance, {{10, 20}, {30, 40}}});
    const QString snapshot = dir.filePath(QStringLiteral("snapshot.pdf"));
    QVERIFY2(DocumentOutput::snapshot(*doc, measurements, snapshot, pw, &serr), qPrintable(serr));
    QVERIFY(!engine.openDocument(snapshot, {}, &e2, &needsPw));
    QVERIFY(needsPw);
    auto saved = engine.openDocument(snapshot, pw, &e2);
    QVERIFY2(saved, qPrintable(e2));
    QCOMPARE(FormModel(*saved).pageFields(0).at(0).value, QStringLiteral("x"));
    QVERIFY(MeasureExport::readMervinBlob(snapshot, pw));

    const QString flattened = dir.filePath(QStringLiteral("export.pdf"));
    RenderMeasurement mark;
    mark.page = 0;
    mark.pts = {{10, 20}, {30, 40}};
    mark.label = QStringLiteral("20 mm");
    QCOMPARE(MeasureExport::flatten(snapshot, flattened, {mark}, pw, &serr),
             MeasureExport::Status::Ok);
    QVERIFY(!engine.openDocument(flattened, {}, &e2, &needsPw));
    QVERIFY(needsPw);
    auto exported = engine.openDocument(flattened, pw, &e2);
    QVERIFY2(exported, qPrintable(e2));
    QCOMPARE(FormModel(*exported).pageFields(0).at(0).value, QStringLiteral("x"));
    QVERIFY(!MeasureExport::readMervinBlob(flattened, pw));
}

QTEST_GUILESS_MAIN(TstFormModel)
#include "tst_form_model.moc"
