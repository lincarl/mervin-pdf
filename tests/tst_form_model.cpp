#include "render/Document.h"
#include "render/MeasureContent.h"
#include "security/DocumentOutput.h"
#include "security/MeasureExport.h"
#include "security/QpdfService.h"
#include "render/FormModel.h"
#include "render/FormTypes.h"
#include "render/RenderEngine.h"

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

    const QString value = QString::fromUtf8("\xC3\xA5\xC3\xA4\xC3\xB6 \xE2\x82\xAC"); // "åäö €"
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
