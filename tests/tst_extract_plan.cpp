#include "extract/ExtractPlan.h"

#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QTest>

using mervin::ExtractPlan;
using Kind = ExtractPlan::Cell::Kind;

// ExtractPlan owns everything the Extract Pages dialog shows: which pages each
// row contributes and where they land, the first problem, how the strip folds to
// a width, every tooltip, and the name the file gets. The dialog is only a
// rendering of this, so these cases are what guard the "which pages, in what
// order, where" promise. Each pairs inputs that must work with inputs that must
// be refused, so a rule that stopped rejecting anything would fail here too.
class TstExtractPlan : public QObject
{
    Q_OBJECT

private slots:
    void rowsKeepTheirPagesInOrder();
    void everyBadRowIsExplained_data();
    void everyBadRowIsExplained();
    void emptyRowsAndNoRowsBlock();
    void setPageCountReparses();
    void outputIsBlankBelowABadRow();
    void stripKnowsNoPositionBelowABadRow();
    void movesAndDropGaps();
    void separatorsSplitIntoRows_data();
    void separatorsSplitIntoRows();
    void removingFromARowSplitsIt();
    void foldsLongestRunsUntilItFits();
    void describeNamesThePosition();
    void namesFollowTheRunsAndStep();
    void destinationRules();
    void jobFollowsRowOrder();

private:
    static ExtractPlan plan(const QStringList &rows, const QString &source = sourcePath())
    {
        ExtractPlan p(source, 31);
        for (const QString &r : rows)
            p.append(r);
        return p;
    }
    static QString sourcePath() { return QStringLiteral("/reports/annual-report.pdf"); }
    static QList<int> pages(const ExtractPlan &p) { return p.job(QStringLiteral("x")).pages; }
    // "PPFP": one letter per cell (Page, Fold, Bad).
    static QString kinds(const QList<ExtractPlan::Cell> &cells)
    {
        QString s;
        for (const ExtractPlan::Cell &c : cells)
            s += c.kind == Kind::Page ? QLatin1Char('P')
               : c.kind == Kind::Fold ? QLatin1Char('F')
                                      : QLatin1Char('B');
        return s;
    }
    static int width(const QList<ExtractPlan::Cell> &cells, const ExtractPlan::Metrics &m)
    {
        int w = m.lead;
        for (const ExtractPlan::Cell &c : cells)
            w += c.width;
        return w;
    }
};

void TstExtractPlan::rowsKeepTheirPagesInOrder()
{
    ExtractPlan p = plan({QStringLiteral("5-7"), QStringLiteral("12")});
    QCOMPARE(p.count(), 2);
    QCOMPARE(p.pagesFor(0), QList<int>({4, 5, 6}));
    QCOMPARE(pages(p), QList<int>({4, 5, 6, 11}));
    QVERIFY(p.isValid());
    QCOMPARE(p.pageTotal(), 4);
    QCOMPARE(p.summaryText(), QStringLiteral("Result: 4 pages, in the order shown."));

    // The literal reading: nothing sorted, nothing de-duplicated.
    QCOMPARE(pages(plan({QStringLiteral("12"), QStringLiteral("1-3")})), QList<int>({11, 0, 1, 2}));
    QCOMPARE(pages(plan({QStringLiteral("1"), QStringLiteral("1")})), QList<int>({0, 0}));
    QCOMPARE(pages(plan({QStringLiteral("29-")})), QList<int>({28, 29, 30}));
    QCOMPARE(pages(plan({QStringLiteral("-3")})), QList<int>({0, 1, 2}));
    QCOMPARE(plan({QStringLiteral(" ALL ")}).pageTotal(), 31);
    QCOMPARE(plan({QStringLiteral("7")}).summaryText(), QStringLiteral("Result: 1 page."));

    p.setSpec(1, QStringLiteral("20-21"));
    QCOMPARE(pages(p), QList<int>({4, 5, 6, 19, 20}));
    QCOMPARE(p.specs(), QStringList({QStringLiteral("5-7"), QStringLiteral("20-21")}));
}

void TstExtractPlan::everyBadRowIsExplained_data()
{
    QTest::addColumn<QString>("spec");
    QTest::addColumn<QString>("error");
    QTest::newRow("word") << "x" << "\"x\" is not a valid page number.";
    QTest::newRow("half range") << "4-x" << "\"4-x\" is not a valid page range.";
    QTest::newRow("past the end") << "30-40" << "Range \"30-40\" is out of range (1-31).";
    QTest::newRow("backwards") << "12-1" << "Range \"12-1\" is backwards - write it as low-high.";
    QTest::newRow("bare dash") << "-" << "\"-\" is not a valid page range.";
    QTest::newRow("zero") << "0" << "Page 0 is out of range (1-31).";
    QTest::newRow("past the last page") << "32" << "Page 32 is out of range (1-31).";
    // ...and the edges that are fine.
    QTest::newRow("last page") << "31" << "";
    QTest::newRow("whole range") << "1-31" << "";
    QTest::newRow("open end") << "31-" << "";
    QTest::newRow("all") << "all" << "";
}

void TstExtractPlan::everyBadRowIsExplained()
{
    QFETCH(QString, spec);
    QFETCH(QString, error);
    // Alone, the sentence is PageRange's, verbatim.
    const ExtractPlan one = plan({spec});
    QCOMPARE(one.errorText(), error);
    QCOMPARE(one.isValid(), error.isEmpty());
    QCOMPARE(one.isBadText(0), !error.isEmpty());
    QCOMPARE(one.summaryText().isEmpty(), !error.isEmpty());
    // With 2+ rows it says which row.
    const ExtractPlan two = plan({QStringLiteral("1"), spec});
    QCOMPARE(two.errorText(), error.isEmpty() ? QString() : QStringLiteral("Row 2: ") + error);
    QCOMPARE(two.rowError(1), error);
}

void TstExtractPlan::emptyRowsAndNoRowsBlock()
{
    ExtractPlan p(sourcePath(), 31);
    QVERIFY(!p.isValid());
    QCOMPARE(p.errorText(), QStringLiteral("Add a range of pages to extract."));

    p.append(QStringLiteral("  "));
    QVERIFY(!p.isValid());
    QCOMPARE(p.errorText(), QStringLiteral("Enter a page or range, like 5-7."));
    QVERIFY(!p.isBadText(0)); // empty is unfinished, not wrong: no red outline

    p.insert(0, QStringLiteral("7"));
    QCOMPARE(p.errorText(), QStringLiteral("Row 2: Enter a page or range, like 5-7."));
    QVERIFY(p.summaryText().isEmpty());
    p.setSpec(1, QStringLiteral("9"));
    QVERIFY(p.isValid());
    QCOMPARE(pages(p), QList<int>({6, 8}));
}

void TstExtractPlan::setPageCountReparses()
{
    ExtractPlan p = plan({QStringLiteral("30")});
    QVERIFY(p.isValid());
    p.setPageCount(20); // qpdf's count, once the password is verified
    QVERIFY(!p.isValid());
    QCOMPARE(p.errorText(), QStringLiteral("Page 30 is out of range (1-20)."));
    p.setPageCount(31);
    QVERIFY(p.isValid());
}

void TstExtractPlan::outputIsBlankBelowABadRow()
{
    const auto texts = [](const ExtractPlan &p) {
        QStringList out;
        for (const ExtractPlan::RowText &t : p.rowTexts())
            out << t.count + QLatin1Char('|') + t.output;
        return out;
    };
    QCOMPARE(texts(plan({QStringLiteral("5-7"), QStringLiteral("12"), QStringLiteral("all")})),
             QStringList({QStringLiteral("3|1-3"), QStringLiteral("1|4"), QStringLiteral("31|5-35")}));
    // A bad row counts "-", and no row from it down knows where it lands.
    QCOMPARE(texts(plan({QStringLiteral("5-7"), QStringLiteral("40"), QStringLiteral("12")})),
             QStringList({QStringLiteral("3|1-3"), QStringLiteral("-|"), QStringLiteral("1|")}));
    QCOMPARE(texts(plan({QString(), QStringLiteral("12")})),
             QStringList({QStringLiteral("-|"), QStringLiteral("1|")}));
}

void TstExtractPlan::stripKnowsNoPositionBelowABadRow()
{
    // The strip's tooltips follow the Output column: fixing the blocking row
    // would move every page below it, so none of them names a position.
    for (const QString &blocker : {QStringLiteral("40"), QString()}) {
        const ExtractPlan p = plan({QStringLiteral("5-7"), blocker, QStringLiteral("12")});
        const QList<ExtractPlan::Cell> cells = p.cells({}, 10000, {});
        QCOMPARE(cells.first().position, 1);
        QCOMPARE(p.describe(cells.first()), QStringLiteral("Page 5 becomes page 1 of the extract."));
        QCOMPARE(cells.last().page, 11);
        QCOMPARE(cells.last().position, 0);
        QCOMPARE(p.describe(cells.last()), QStringLiteral("Page 12"));
    }
}

void TstExtractPlan::movesAndDropGaps()
{
    const QStringList abc{QStringLiteral("1"), QStringLiteral("2"), QStringLiteral("3")};
    ExtractPlan p = plan(abc);
    QCOMPARE(p.move(2, -1), 1);
    QCOMPARE(pages(p), QList<int>({0, 2, 1}));
    QCOMPARE(p.move(0, -1), -1); // off the top: nothing moves
    QCOMPARE(p.move(2, 1), -1);
    QCOMPARE(pages(p), QList<int>({0, 2, 1}));

    // Gaps are counted before the row is lifted out: 0 above the first row,
    // count() below the last.
    p = plan(abc);
    QCOMPARE(p.moveToGap(0, 3), 2);
    QCOMPARE(pages(p), QList<int>({1, 2, 0}));
    p = plan(abc);
    QCOMPARE(p.moveToGap(2, 0), 0);
    QCOMPARE(pages(p), QList<int>({2, 0, 1}));
    // Dropping a row just above or below itself changes nothing.
    p = plan(abc);
    QCOMPARE(p.moveToGap(1, 1), -1);
    QCOMPARE(p.moveToGap(1, 2), -1);
    QCOMPARE(p.moveToGap(1, 4), -1);
    QCOMPARE(pages(p), QList<int>({0, 1, 2}));
}

void TstExtractPlan::separatorsSplitIntoRows_data()
{
    QTest::addColumn<QString>("text");
    QTest::addColumn<QStringList>("pieces");
    QTest::newRow("one range") << "5-7" << QStringList{"5-7"};
    QTest::newRow("pasted list") << "1-3, 5, 8-10" << QStringList{"1-3", "5", "8-10"};
    QTest::newRow("semicolons too") << "1-4; 5-12" << QStringList{"1-4", "5-12"};
    QTest::newRow("typed comma") << "2," << QStringList{"2", ""};
    QTest::newRow("empty pieces") << "1,, 3 ;" << QStringList{"1", "3", ""};
    QTest::newRow("leading comma") << ",5" << QStringList{"5"};
    QTest::newRow("only a comma") << "," << QStringList{""};
    QTest::newRow("empty") << "" << QStringList{""};
}

void TstExtractPlan::separatorsSplitIntoRows()
{
    QFETCH(QString, text);
    QFETCH(QStringList, pieces);
    QCOMPARE(ExtractPlan::splitPieces(text), pieces);
}

void TstExtractPlan::removingFromARowSplitsIt()
{
    // The middle page: the row splits around it, and the row holding what
    // followed becomes current.
    ExtractPlan p = plan({QStringLiteral("5-7"), QStringLiteral("12")});
    QCOMPARE(p.removeFromRow(0, 1, 1), 1);
    QCOMPARE(p.specs(), QStringList({QStringLiteral("5"), QStringLiteral("7"), QStringLiteral("12")}));

    p = plan({QStringLiteral("5-7")});
    QCOMPARE(p.removeFromRow(0, 0, 0), 0); // the first page
    QCOMPARE(p.specs(), QStringList({QStringLiteral("6-7")}));

    p = plan({QStringLiteral("5-7"), QStringLiteral("12")});
    QCOMPARE(p.removeFromRow(0, 2, 2), 1); // the last page: what follows is row 2
    QCOMPARE(p.specs(), QStringList({QStringLiteral("5-6"), QStringLiteral("12")}));

    p = plan({QStringLiteral("all")});
    QCOMPARE(p.removeFromRow(0, 1, 29), 1); // a fold's hidden span
    QCOMPARE(p.specs(), QStringList({QStringLiteral("1"), QStringLiteral("31")}));

    p = plan({QStringLiteral("3"), QStringLiteral("9")});
    QCOMPARE(p.removeFromRow(1, 0, 0), 0); // a single-page row goes, clamped
    QCOMPARE(p.specs(), QStringList({QStringLiteral("3")}));
    QCOMPARE(p.removeFromRow(0, 0, 0), -1); // ...and with it the last row
    QCOMPARE(p.count(), 0);

    p = plan({QStringLiteral("1"), QStringLiteral("40"), QStringLiteral("2")});
    QCOMPARE(p.removeFromRow(1, 0, 0), 1); // a Bad cell removes its row
    QCOMPARE(p.specs(), QStringList({QStringLiteral("1"), QStringLiteral("2")}));

    // Out of range: nothing changes.
    QCOMPARE(p.removeFromRow(5, 0, 0), -1);
    QCOMPARE(p.removeFromRow(0, 0, 1), -1);
    QCOMPARE(p.removeFromRow(0, 1, 0), -1);
    QCOMPARE(p.specs(), QStringList({QStringLiteral("1"), QStringLiteral("2")}));
}

void TstExtractPlan::foldsLongestRunsUntilItFits()
{
    const ExtractPlan::Metrics m; // the strip's: 6 lead, 101 page, 52 fold
    // Longest first: folding the 19-page row is enough, so the 8-page row stays.
    const ExtractPlan three =
        plan({QStringLiteral("1-4"), QStringLiteral("5-12"), QStringLiteral("13-31")});
    QList<ExtractPlan::Cell> cells = three.cells(m, 1500, {});
    QCOMPARE(kinds(cells), QStringLiteral("PPPPPPPPPPPPPFP"));
    QVERIFY(width(cells, m) <= 1500);
    QCOMPARE(cells.at(0).gapBefore, m.lead);
    QCOMPARE(cells.at(1).gapBefore, 0);
    QCOMPARE(cells.at(4).row, 1);
    QCOMPARE(cells.at(13).run, (ExtractPlan::RunKey{12, 30}));
    // Runs never cross rows: "1-2" and "3-4" are two short runs, not one of four.
    QCOMPARE(kinds(plan({QStringLiteral("1-2"), QStringLiteral("3-4")}).cells(m, 200, {})),
             QStringLiteral("PPPP"));

    // The fold that makes it fit hands the spare room back as leading pages:
    // 1 2 3 4 5 6 [7-9] 10 fills 771 of 790 px instead of leaving 1 [2-9] 10's
    // 266. A 101 px narrower strip keeps one page fewer.
    const ExtractPlan ten = plan({QStringLiteral("1-10")});
    cells = ten.cells(m, 790, {});
    QCOMPARE(kinds(cells), QStringLiteral("PPPPPPFP"));
    QCOMPARE(width(cells, m), 771);
    QCOMPARE(cells.at(6).page, 6);
    QCOMPARE(cells.at(6).lastPage, 8);
    QCOMPARE(cells.at(6).first, 6);
    QCOMPARE(cells.at(6).last, 8);
    QCOMPARE(cells.at(7).firstFlat, 9);
    QCOMPARE(kinds(ten.cells(m, 689, {})), QStringLiteral("PPPPPFP"));
    // A fold always hides at least two pages.
    cells = plan({QStringLiteral("1-4"), QStringLiteral("1-4"), QStringLiteral("1-4")})
                .cells(m, 1200, {});
    QCOMPARE(kinds(cells), QStringLiteral("PFPPPPPPPPP")); // 1 [2-3] 4 1 2 3 4 1 2 3 4
    QCOMPARE(cells.at(1).lastPage - cells.at(1).page, 1);
    // ...unless the user opened that run.
    QCOMPARE(kinds(ten.cells(m, 790, {ExtractPlan::RunKey{0, 9}})),
             QStringLiteral("PPPPPPPPPP"));
    // Twin rows are told apart by occurrence, so opening one leaves the other.
    const ExtractPlan twins = plan({QStringLiteral("1-10"), QStringLiteral("1-10")});
    cells = twins.cells(m, 790, {});
    QCOMPARE(kinds(cells), QStringLiteral("PFPPPPFP"));
    QCOMPARE(cells.at(6).run, (ExtractPlan::RunKey{0, 9, 1}));
    QCOMPARE(kinds(twins.cells(m, 790, {ExtractPlan::RunKey{0, 9, 1}})),
             QStringLiteral("PFPPPPPPPPPPP"));
    QVERIFY(twins.hasRun({0, 9, 1}));
    QVERIFY(!ten.hasRun({0, 9, 1}));
    QVERIFY(!plan({QStringLiteral("5-7")}).hasRun({4, 6}));

    // Short runs never fold, and a bad row is one cell: the strip scrolls.
    cells = plan({QStringLiteral("5-7")}).cells(m, 200, {});
    QCOMPARE(kinds(cells), QStringLiteral("PPP"));
    QVERIFY(width(cells, m) > 200);
    cells = plan({QStringLiteral("1-3"), QStringLiteral("40"), QString(), QStringLiteral("4-6")})
                .cells(m, 200, {});
    QCOMPARE(kinds(cells), QStringLiteral("PPPBPPP")); // an empty row has no cell
    QCOMPARE(cells.at(3).row, 1);
    QCOMPARE(cells.at(4).row, 3);
    QCOMPARE(cells.at(4).firstFlat, 4);
}

void TstExtractPlan::describeNamesThePosition()
{
    const ExtractPlan p = plan({QStringLiteral("5-7"), QStringLiteral("12")});
    const QList<ExtractPlan::Cell> cells = p.cells({}, 10000, {});
    QCOMPARE(p.describe(cells.at(3)), QStringLiteral("Page 12 becomes page 4 of the extract."));
    QCOMPARE(p.caption(cells.at(3)), QStringLiteral("12"));

    const ExtractPlan all = plan({QStringLiteral("all")});
    const ExtractPlan::Cell fold = all.cells({}, 790, {}).at(6);
    QCOMPARE(fold.kind, Kind::Fold);
    QCOMPARE(all.caption(fold), QStringLiteral("7-30"));
    QCOMPARE(all.describe(fold), QStringLiteral("Pages 7-30 (24 pages). Click to show them."));

    const ExtractPlan bad = plan({QStringLiteral("5-7"), QStringLiteral(" 40 ")});
    const ExtractPlan::Cell badCell = bad.cells({}, 10000, {}).at(3);
    QCOMPARE(badCell.kind, Kind::Bad);
    QCOMPARE(bad.caption(badCell), QStringLiteral("40"));
    QCOMPARE(bad.describe(badCell), QStringLiteral("Page 40 is out of range (1-31)."));
}

void TstExtractPlan::namesFollowTheRunsAndStep()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString source = dir.filePath(QStringLiteral("annual-report.pdf"));
    const auto touch = [](const QString &path) {
        QFile f(path);
        QVERIFY(f.open(QIODevice::WriteOnly));
    };
    touch(source);

    const auto name = [&source](const QStringList &rows) { return plan(rows, source).fileName(); };
    QCOMPARE(name({QStringLiteral("7")}), QStringLiteral("annual-report-p7.pdf"));
    QCOMPARE(name({QStringLiteral("12-18")}), QStringLiteral("annual-report-p12-18.pdf"));
    QCOMPARE(name({QStringLiteral("all")}), QStringLiteral("annual-report-p1-31.pdf"));
    // Rows that continue each other are still one run.
    QCOMPARE(name({QStringLiteral("5-7"), QStringLiteral("8")}), QStringLiteral("annual-report-p5-8.pdf"));
    // Anything else gets the generic name.
    QCOMPARE(name({QStringLiteral("2"), QStringLiteral("5")}), QStringLiteral("annual-report-extract.pdf"));
    QCOMPARE(name({QStringLiteral("12"), QStringLiteral("1-3")}),
             QStringLiteral("annual-report-extract.pdf"));
    QCOMPARE(name({QStringLiteral("7"), QStringLiteral("x")}), QStringLiteral("annual-report-extract.pdf"));
    QCOMPARE(name({}), QStringLiteral("annual-report-extract.pdf"));
    // An empty row, still being filled in, does not change the name.
    QCOMPARE(name({QStringLiteral("7"), QString()}), QStringLiteral("annual-report-p7.pdf"));

    // Stepping past a file on disk.
    const ExtractPlan seven = plan({QStringLiteral("7")}, source);
    QCOMPARE(seven.defaultOutputPath(), dir.filePath(QStringLiteral("annual-report-p7.pdf")));
    touch(dir.filePath(QStringLiteral("annual-report-p7.pdf")));
    QCOMPARE(seven.defaultOutputPath(), dir.filePath(QStringLiteral("annual-report-p7-2.pdf")));
}

void TstExtractPlan::destinationRules()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString source = dir.filePath(QStringLiteral("annual-report.pdf"));
    QFile f(source);
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.close();
    const ExtractPlan p = plan({QStringLiteral("7")}, source);

    QCOMPARE(p.destinationError(QStringLiteral("  ")),
             QStringLiteral("Choose where to save the extracted pages."));
    const QString missing = dir.filePath(QStringLiteral("missing"));
    QCOMPARE(p.destinationError(missing + QStringLiteral("/out.pdf")),
             QStringLiteral("That folder does not exist."));
    const QString replace = QStringLiteral("The extract cannot replace the document it comes from.");
    QCOMPARE(p.destinationError(source), replace);
    // ".pdf" is appended before comparing, so leaving it off does not get round it.
    QCOMPARE(p.destinationError(dir.filePath(QStringLiteral("annual-report"))), replace);

    // A relative name lands beside the source.
    QVERIFY(p.destinationError(QStringLiteral("out.pdf")).isEmpty());
    QCOMPARE(p.job(QStringLiteral("out")).path, dir.filePath(QStringLiteral("out.pdf")));

    // A folder, or text ending in a separator, is not a file name (cleaning it
    // would otherwise write "<folder>.pdf" beside it).
    const QString folder = QStringLiteral("That is a folder. Add a file name.");
    QCOMPARE(p.destinationError(dir.path()), folder);
    QCOMPARE(p.destinationError(QDir::toNativeSeparators(dir.path() + QLatin1Char('/'))), folder);
    QCOMPARE(p.destinationError(missing + QLatin1Char('/')),
             QStringLiteral("That folder does not exist."));

    // A file open in a tab cannot be replaced: the viewer holds it open, so the
    // write would fail. Another spelling of the same path is the same file; an
    // existing file that no tab holds is fine (the dialog asks before replacing).
    const QString opened = dir.filePath(QStringLiteral("opened.pdf"));
    const QString closed = dir.filePath(QStringLiteral("closed.pdf"));
    for (const QString &path : {opened, closed}) {
        QFile out(path);
        QVERIFY(out.open(QIODevice::WriteOnly));
    }
    ExtractPlan withTabs = plan({QStringLiteral("7")}, source);
    withTabs.setOpenFiles({source, opened});
    const QString inTab = QStringLiteral("That file is open in a tab. Choose another name.");
    QCOMPARE(withTabs.destinationError(opened), inTab);
    QCOMPARE(withTabs.destinationError(QStringLiteral("opened")), inTab); // relative, no ".pdf"
    QCOMPARE(withTabs.destinationError(QDir::toNativeSeparators(opened)), inTab);
#ifdef Q_OS_WIN
    QCOMPARE(withTabs.destinationError(opened.toUpper()), inTab);
#endif
    QVERIFY(withTabs.destinationError(closed).isEmpty());
    QCOMPARE(withTabs.destinationError(source), replace); // the source's own reason wins
    withTabs.setOpenFiles({});
    QVERIFY(withTabs.destinationError(opened).isEmpty());
}

void TstExtractPlan::jobFollowsRowOrder()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    ExtractPlan p = plan({QStringLiteral("5-7"), QStringLiteral("12"), QStringLiteral("9")},
                         dir.filePath(QStringLiteral("annual-report.pdf")));
    ExtractPlan::Job job = p.job(QStringLiteral("x"));
    QCOMPARE(job.path, dir.filePath(QStringLiteral("x.pdf")));
    QCOMPARE(job.pages, QList<int>({4, 5, 6, 11, 8}));
    QCOMPARE(p.move(2, -2), 0);
    QCOMPARE(p.job(QStringLiteral("x")).pages, QList<int>({8, 4, 5, 6, 11}));

    QCOMPARE(ExtractPlan::doneText({dir.filePath(QStringLiteral("annual-report-p7.pdf")), {6}}),
             QStringLiteral("Extracted page 7 to annual-report-p7.pdf"));
    job.path = dir.filePath(QStringLiteral("annual-report-extract.pdf"));
    QCOMPARE(ExtractPlan::doneText(job), QStringLiteral("Extracted 5 pages to annual-report-extract.pdf"));
}

QTEST_GUILESS_MAIN(TstExtractPlan)
#include "tst_extract_plan.moc"
