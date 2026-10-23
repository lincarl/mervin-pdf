#include "render/ContentSearch.h"
#include "io/JobContext.h"
#include "recent/PathKey.h"

#include <QByteArray>
#include <QFile>
#include <QFileInfo>
#include <QList>
#include <QSignalSpy>
#include <QString>
#include <QStringList>
#include <QTest>
#include <QTemporaryDir>

#include <memory>

using mervin::ContentSearch;

// Exercises worker-thread searches using real PDFs with deterministic page text.
class TstContentSearch : public QObject
{
    Q_OBJECT

private slots:
    void blankQueryFinishesImmediately();
    void emptyPathListFinishes();
    void findsTermInRealPdf();
    void missesAbsentTerm();
    void replacedQueryRejectsQueuedResults();
    void skipsTakenDestinations_data();
    void skipsTakenDestinations();
};

namespace {

// Assembles numbered PDF objects and their cross-reference table.
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

// Writes three pages with distinct matches on pages 2 and 3, or returns empty on failure.
QString writeSearchPdf(const QTemporaryDir &dir)
{
    const QList<QByteArray> contents = {
        "BT /F1 10 Tf 50 740 Td (Opening page without the requested text.) Tj ET",
        "BT /F1 10 Tf 14 TL 50 740 Td "
        "(STARTMARK This introduction belongs outside the short preview.) Tj T* "
        "(More introductory text keeps the match away from the page start.) Tj T* "
        "(Amber context) Tj T* "
        "(pineNeedlework follows the wrapped line with cedar context.) Tj T* "
        "(Additional trailing text keeps the preview shorter than this page.) Tj T* "
        "(Further trailing words belong outside the preview for this result.) Tj T* "
        "(ENDMARK) Tj ET",
        "BT /F1 10 Tf 50 740 Td (Later context has another pineNeedlework match.) Tj ET",
    };

    QList<QByteArray> objs;
    objs << "<< /Type /Catalog /Pages 2 0 R >>";
    objs << "<< /Type /Pages /Kids [3 0 R 5 0 R 7 0 R] /Count 3 >>";
    for (int page = 0; page < contents.size(); ++page) {
        const QByteArray &content = contents[page];
        objs << "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] "
                "/Resources << /Font << /F1 9 0 R >> >> /Contents "
                + QByteArray::number(4 + 2 * page) + " 0 R >>";
        objs << "<< /Length " + QByteArray::number(content.size()) + " >>\nstream\n"
                + content + "\nendstream";
    }
    objs << "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>";

    const QString path = dir.filePath(QStringLiteral("search.pdf"));
    const QByteArray pdf = assemblePdf(objs);
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(pdf) != pdf.size())
        return {};
    file.close();
    return path;
}

} // namespace

void TstContentSearch::blankQueryFinishesImmediately()
{
    ContentSearch cs;
    QSignalSpy done(&cs, &ContentSearch::finished);
    cs.start({QStringLiteral("C:/whatever.pdf")}, QStringLiteral("   "));
    QCOMPARE(done.count(), 1);          // blank query -> immediate finish
    QVERIFY(!done.at(0).at(0).toBool()); // not canceled
    QCOMPARE(done.at(0).at(1).toInt(), 0);
}

void TstContentSearch::emptyPathListFinishes()
{
    ContentSearch cs;
    QSignalSpy done(&cs, &ContentSearch::finished);
    cs.start({}, QStringLiteral("anything"));
    QCOMPARE(done.count(), 1);
    QCOMPARE(done.at(0).at(1).toInt(), 0);
}

// Checks the first matching page, snippet context and continuation after an unreadable input.
void TstContentSearch::findsTermInRealPdf()
{
    QTemporaryDir files;
    QVERIFY(files.isValid());
    const QString pdf = writeSearchPdf(files);
    QVERIFY(!pdf.isEmpty());
    const QString missing = files.filePath(QStringLiteral("missing.pdf"));

    ContentSearch cs;
    QSignalSpy hits(&cs, &ContentSearch::hit);
    QSignalSpy progress(&cs, &ContentSearch::progress);
    QSignalSpy done(&cs, &ContentSearch::finished);

    // Match a differently cased substring within pineNeedlework on pages 2 and 3.
    cs.start({missing, pdf}, QStringLiteral("nEeDlE"));
    QTRY_COMPARE_WITH_TIMEOUT(done.count(), 1, 10000);
    QCOMPARE(hits.count(), 1);
    QCOMPARE(hits.at(0).at(0).toString(), pdf);
    QCOMPARE(hits.at(0).at(1).toInt(), 2);
    const QString snippet = hits.at(0).at(2).toString();
    QVERIFY2(snippet.contains(QStringLiteral("Amber context pineNeedlework")), qPrintable(snippet));
    QVERIFY2(snippet.contains(QStringLiteral("cedar context")), qPrintable(snippet));
    QVERIFY(!snippet.contains(QLatin1Char('\n')));
    QVERIFY(!snippet.contains(QLatin1Char('\r')));
    QVERIFY(!snippet.contains(QStringLiteral("STARTMARK")));
    QVERIFY(!snippet.contains(QStringLiteral("ENDMARK")));
    QVERIFY(!progress.isEmpty());
    QCOMPARE(progress.last().at(0).toInt(), 2);
    QCOMPARE(progress.last().at(1).toInt(), 2);
    QVERIFY(!done.at(0).at(0).toBool());
    QCOMPARE(done.at(0).at(1).toInt(), 1);
    QVERIFY(!cs.isRunning());
}

// Checks that a known absent query finishes with no hits.
void TstContentSearch::missesAbsentTerm()
{
    QTemporaryDir files;
    QVERIFY(files.isValid());
    const QString pdf = writeSearchPdf(files);
    QVERIFY(!pdf.isEmpty());

    ContentSearch cs;
    QSignalSpy hits(&cs, &ContentSearch::hit);
    QSignalSpy progress(&cs, &ContentSearch::progress);
    QSignalSpy done(&cs, &ContentSearch::finished);

    cs.start({pdf}, QStringLiteral("zqxjk_no_such_term_42"));
    QTRY_COMPARE_WITH_TIMEOUT(done.count(), 1, 10000);
    QCOMPARE(hits.count(), 0);
    QVERIFY(!progress.isEmpty());
    QCOMPARE(progress.last().at(0).toInt(), 1);
    QCOMPARE(progress.last().at(1).toInt(), 1);
    QVERIFY(!done.at(0).at(0).toBool());
    QCOMPARE(done.at(0).at(1).toInt(), 0);
    QVERIFY(!cs.isRunning());
}


void TstContentSearch::replacedQueryRejectsQueuedResults()
{
    ContentSearch search;
    QSignalSpy hits(&search, &ContentSearch::hit);
    QSignalSpy done(&search, &ContentSearch::finished);
    search.start({QStringLiteral(MERVIN_FIXTURE_PDF)}, "STANDARD");
    // Keep UI delivery queued while the worker finishes the tiny document.
    QTest::qSleep(100);
    search.start({QStringLiteral(MERVIN_FIXTURE_PDF)}, "absent");
    QTRY_COMPARE(done.size(), 1);
    QCOMPARE(hits.size(), 0);
    QCOMPARE(done[0][1].toInt(), 0);
    search.start({QStringLiteral(MERVIN_FIXTURE_PDF)}, "STANDARD");
    QTRY_COMPARE(hits.size(), 1);
}

// Selects locked or reserved output keys and aliases that must not be searched.
void TstContentSearch::skipsTakenDestinations_data()
{
    QTest::addColumn<bool>("reserved");
    QTest::addColumn<bool>("alias");
    QTest::newRow("destination lock") << false << false;
    QTest::newRow("output reservation") << true << false;
#ifdef Q_OS_UNIX
    QTest::newRow("aliased destination lock") << false << true;
    QTest::newRow("aliased output reservation") << true << true;
#endif
}

// Checks search skips protected inputs without opening their document bytes.
void TstContentSearch::skipsTakenDestinations()
{
    QFETCH(bool, reserved);
    QFETCH(bool, alias);
    QTemporaryDir files;
    const QString source = files.filePath(QStringLiteral("matching.pdf"));
    const QString alternate = files.filePath(QStringLiteral("alias.pdf"));
    QVERIFY(QFile::copy(QStringLiteral(MERVIN_FIXTURE_PDF), source));
    if (alias)
        QVERIFY(QFile::link(source, alternate));
    const QString path = alias ? alternate : source;
    mervin::job::State writer;
    const mervin::job::Scope scope(writer);
    std::unique_ptr<mervin::job::DestinationLock> lock;
    if (reserved) {
        QVERIFY(mervin::job::reserveDestinations({mervin::fileKey(QFileInfo(source))}));
    } else {
        lock = std::make_unique<mervin::job::DestinationLock>(source);
        QVERIFY(lock->owns());
    }
    ContentSearch search;
    QSignalSpy hits(&search, &ContentSearch::hit);
    QSignalSpy progress(&search, &ContentSearch::progress);
    QSignalSpy done(&search, &ContentSearch::finished);
    search.start({path}, QStringLiteral("STANDARD"));
    QTRY_COMPARE(done.size(), 1);
    QCOMPARE(hits.size(), 0);
    QCOMPARE(progress.size(), 1);
    QCOMPARE(progress[0][0].toInt(), 1);
    QCOMPARE(progress[0][1].toInt(), 1);
    QVERIFY(!done[0][0].toBool());
    QCOMPARE(done[0][1].toInt(), 0);
    QVERIFY(!search.isRunning());

    lock.reset();
    writer.stop();
    done.clear();
    progress.clear();
    search.start({path}, QStringLiteral("STANDARD"));
    QTRY_COMPARE(done.size(), 1);
    QCOMPARE(hits.size(), 1);
    QCOMPARE(hits[0][0].toString(), path);
    QVERIFY(hits[0][1].toInt() >= 1);
    QVERIFY(hits[0][2].toString().contains(QStringLiteral("STANDARD"), Qt::CaseInsensitive));
    QCOMPARE(progress.size(), 1);
    QCOMPARE(progress[0][0].toInt(), 1);
    QCOMPARE(progress[0][1].toInt(), 1);
    QVERIFY(!done[0][0].toBool());
    QCOMPARE(done[0][1].toInt(), 1);
}

QTEST_GUILESS_MAIN(TstContentSearch)
#include "tst_content_search.moc"
