#include "render/ContentSearch.h"
#include "render/RenderEngine.h"

#include <QFileInfo>
#include <QSignalSpy>
#include <QString>
#include <QStringList>
#include <QTest>

using mervin::ContentSearch;
using mervin::RenderEngine;

// ContentSearch drives MuPDF text extraction across files on a worker thread, so
// a meaningful test needs a real PDF. It uses an optional local fixture when
// available. Overrides:
//   MERVIN_TEST_PDF  - absolute path to a PDF known to contain the term
//   MERVIN_TEST_TERM - a string present somewhere in that PDF (default below)
//
// These cases used to QSKIP unless MERVIN_TEST_PDF was set, which meant they
// never ran anywhere - and a skip is reported by ctest as a pass unless the
// suite is set up to notice (see tests/CMakeLists.txt).
class TstContentSearch : public QObject
{
    Q_OBJECT

private slots:
    void blankQueryFinishesImmediately();
    void emptyPathListFinishes();
    void findsTermInRealPdf();
    void missesAbsentTerm();
};

namespace {

// The document the data-dependent cases search, or empty if none is available.
QByteArray searchPdf()
{
    QByteArray pdf = qgetenv("MERVIN_TEST_PDF");
#ifdef MERVIN_SEARCH_PDF
    if (pdf.isEmpty())
        pdf = QByteArray(MERVIN_SEARCH_PDF);
#endif
    if (!pdf.isEmpty() && !QFileInfo::exists(QString::fromUtf8(pdf)))
        return {}; // tree without the sample documents
    return pdf;
}

} // namespace

void TstContentSearch::blankQueryFinishesImmediately()
{
    RenderEngine engine;
    ContentSearch cs(&engine);
    QSignalSpy done(&cs, &ContentSearch::finished);
    cs.start({QStringLiteral("C:/whatever.pdf")}, QStringLiteral("   "));
    QCOMPARE(done.count(), 1);          // blank query -> immediate finish
    QVERIFY(!done.at(0).at(0).toBool()); // not canceled
    QCOMPARE(done.at(0).at(1).toInt(), 0);
}

void TstContentSearch::emptyPathListFinishes()
{
    RenderEngine engine;
    ContentSearch cs(&engine);
    QSignalSpy done(&cs, &ContentSearch::finished);
    cs.start({}, QStringLiteral("anything"));
    QCOMPARE(done.count(), 1);
    QCOMPARE(done.at(0).at(1).toInt(), 0);
}

void TstContentSearch::findsTermInRealPdf()
{
    const QByteArray pdf = searchPdf();
    if (pdf.isEmpty())
        QSKIP("no search document: set MERVIN_TEST_PDF to a PDF containing the term");
    QString term = QString::fromUtf8(qgetenv("MERVIN_TEST_TERM"));
    if (term.isEmpty())
        term = QStringLiteral("the");

    RenderEngine engine;
    ContentSearch cs(&engine);
    QSignalSpy hits(&cs, &ContentSearch::hit);
    QSignalSpy done(&cs, &ContentSearch::finished);

    cs.start({QString::fromUtf8(pdf)}, term);
    QVERIFY(done.wait(60000)); // extraction can be slow on huge / corrupt PDFs
    QCOMPARE(hits.count(), 1);
    QCOMPARE(hits.at(0).at(0).toString(), QString::fromUtf8(pdf));
    QVERIFY(hits.at(0).at(1).toInt() >= 1); // a 1-based page number
    QVERIFY(!done.at(0).at(0).toBool());    // not canceled
    QCOMPARE(done.at(0).at(1).toInt(), 1);  // one file matched
}

void TstContentSearch::missesAbsentTerm()
{
    const QByteArray pdf = searchPdf();
    if (pdf.isEmpty())
        QSKIP("no search document: set MERVIN_TEST_PDF to a PDF containing the term");

    RenderEngine engine;
    ContentSearch cs(&engine);
    QSignalSpy hits(&cs, &ContentSearch::hit);
    QSignalSpy done(&cs, &ContentSearch::finished);

    // A term overwhelmingly unlikely to appear in any document.
    cs.start({QString::fromUtf8(pdf)}, QStringLiteral("zqxjk_no_such_term_42"));
    QVERIFY(done.wait(60000));
    QCOMPARE(hits.count(), 0);
    QCOMPARE(done.at(0).at(1).toInt(), 0);
}

QTEST_GUILESS_MAIN(TstContentSearch)
#include "tst_content_search.moc"
