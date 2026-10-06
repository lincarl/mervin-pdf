#include "merge/MergePlan.h"

#include <QFileInfo>
#include <QStringList>
#include <QTemporaryDir>
#include <QTest>

#include <utility>

using mervin::MergePlan;

// MergePlan owns every number the merge dialog shows: the row ordinals, each
// row's page count, where each row lands in the finished document, the total,
// and the first thing wrong with the plan. The dialog is only a rendering of
// this, so these cases are what actually guard the "which pages, in what order"
// promise - a widget test could not reach the arithmetic any more directly.
class TstMergePlan : public QObject
{
    Q_OBJECT

private slots:
    void appendAndCount();
    void moveReordersAndClampsAtTheEnds();
    void removeShiftsTheRest();
    void duplicateInsertsBelow();
    void specDrivesPageSelection();
    void specPreservesTypedOrderAndDuplicates();
    void countTextDistinguishesAllFromASubset();
    void outputRangesAreARunningSum();
    void outputRangeHidesItselfAfterABadRow();
    void duplicatedFileGetsItsOwnOutputRange();
    void invalidSpecInvalidatesThePlan();
    void lockedAndUnreadableRowsBlock();
    void errorTextNamesTheFirstBadRow();
    void displayNameDisambiguatesOnlyDifferentFiles();
    void summaryTextCountsDistinctFiles();
    void inputsAreZeroBasedInTypedOrder();
    void defaultOutputPathAvoidsExistingAndInputFiles();
    void rowTextsMatchesThePerRowGetters();
    void dropGapsMapToTheRightRow();

private:
    static MergePlan::Entry ok(const QString &path, int pages, const QString &spec = QString())
    {
        MergePlan::Entry e;
        e.path = path;
        e.pageCount = pages;
        e.load = MergePlan::Load::Ok;
        if (!spec.isEmpty())
            e.spec = spec;
        return e;
    }
};

void TstMergePlan::appendAndCount()
{
    MergePlan p;
    QVERIFY(p.isEmpty());
    QVERIFY(!p.isValid()); // an empty plan is not a runnable merge
    p.append(ok(QStringLiteral("/a/one.pdf"), 3));
    p.append(ok(QStringLiteral("/a/two.pdf"), 5));
    QCOMPARE(p.count(), 2);
    QCOMPARE(p.totalPages(), 8);
    QVERIFY(p.isValid());
}

void TstMergePlan::moveReordersAndClampsAtTheEnds()
{
    MergePlan p;
    p.append(ok(QStringLiteral("/a/one.pdf"), 1));
    p.append(ok(QStringLiteral("/a/two.pdf"), 1));
    p.append(ok(QStringLiteral("/a/three.pdf"), 1));

    QCOMPARE(p.move(2, -1), 1);
    QCOMPARE(p.at(1).path, QStringLiteral("/a/three.pdf"));
    QCOMPARE(p.at(2).path, QStringLiteral("/a/two.pdf"));

    // Boundaries are no-ops that report themselves, not crashes or silent wraps.
    QCOMPARE(p.move(0, -1), -1);
    QCOMPARE(p.move(2, 1), -1);
    QCOMPARE(p.move(0, 0), -1);
    QCOMPARE(p.move(-1, 1), -1);
    QCOMPARE(p.move(9, 1), -1);
    QCOMPARE(p.at(0).path, QStringLiteral("/a/one.pdf"));
}

void TstMergePlan::removeShiftsTheRest()
{
    MergePlan p;
    p.append(ok(QStringLiteral("/a/one.pdf"), 1));
    p.append(ok(QStringLiteral("/a/two.pdf"), 1));
    p.append(ok(QStringLiteral("/a/three.pdf"), 1));
    p.remove(0);
    QCOMPARE(p.count(), 2);
    QCOMPARE(p.at(0).path, QStringLiteral("/a/two.pdf"));
    p.remove(17); // out of range is ignored, not fatal
    QCOMPARE(p.count(), 2);
}

void TstMergePlan::duplicateInsertsBelow()
{
    MergePlan p;
    p.append(ok(QStringLiteral("/a/one.pdf"), 4, QStringLiteral("2-3")));
    p.append(ok(QStringLiteral("/a/two.pdf"), 1));
    QCOMPARE(p.duplicate(0), 1);
    QCOMPARE(p.count(), 3);
    QCOMPARE(p.at(1).path, QStringLiteral("/a/one.pdf"));
    QCOMPARE(p.at(1).spec, QStringLiteral("2-3")); // the range comes with it
    QCOMPARE(p.at(2).path, QStringLiteral("/a/two.pdf"));
    QCOMPARE(p.duplicate(99), -1);
}

void TstMergePlan::specDrivesPageSelection()
{
    MergePlan p;
    p.append(ok(QStringLiteral("/a/one.pdf"), 10));
    QCOMPARE(p.pagesFor(0).size(), 10);

    p.setSpec(0, QStringLiteral("1-3, 7"));
    QCOMPARE(p.pagesFor(0), QList<int>({0, 1, 2, 6}));
    QCOMPARE(p.totalPages(), 4);

    p.setSpec(0, QStringLiteral("  all  ")); // any case, any padding
    QCOMPARE(p.pagesFor(0).size(), 10);
    p.setSpec(0, QStringLiteral("ALL"));
    QCOMPARE(p.pagesFor(0).size(), 10);
}

void TstMergePlan::specPreservesTypedOrderAndDuplicates()
{
    // The column caption says "Pages, in this order" and it has to be true.
    MergePlan p;
    p.append(ok(QStringLiteral("/a/one.pdf"), 10, QStringLiteral("5-6, 1, 1")));
    QCOMPARE(p.pagesFor(0), QList<int>({4, 5, 0, 0}));
}

void TstMergePlan::countTextDistinguishesAllFromASubset()
{
    MergePlan p;
    p.append(ok(QStringLiteral("/a/one.pdf"), 31));
    QCOMPARE(p.countText(0), QStringLiteral("31"));
    p.setSpec(0, QStringLiteral("1-3, 7"));
    QCOMPARE(p.countText(0), QStringLiteral("4 of 31"));
    p.setSpec(0, QStringLiteral("nonsense"));
    QCOMPARE(p.countText(0), QStringLiteral("-"));
}

void TstMergePlan::outputRangesAreARunningSum()
{
    MergePlan p;
    p.append(ok(QStringLiteral("/a/one.pdf"), 12));
    p.append(ok(QStringLiteral("/a/two.pdf"), 31, QStringLiteral("1-3, 7")));
    p.append(ok(QStringLiteral("/a/three.pdf"), 8));
    QCOMPARE(p.outputText(0), QStringLiteral("1-12"));
    QCOMPARE(p.outputText(1), QStringLiteral("13-16"));
    QCOMPARE(p.outputText(2), QStringLiteral("17-24"));
    QCOMPARE(p.totalPages(), 24);
}

void TstMergePlan::outputRangeHidesItselfAfterABadRow()
{
    // Where a row lands is unknowable once anything above it is broken, and a
    // number the user could read as authoritative would be a guess.
    MergePlan p;
    p.append(ok(QStringLiteral("/a/one.pdf"), 12));
    p.append(ok(QStringLiteral("/a/two.pdf"), 5, QStringLiteral("99")));
    p.append(ok(QStringLiteral("/a/three.pdf"), 8));
    QCOMPARE(p.outputText(0), QStringLiteral("1-12"));
    QVERIFY(p.outputText(1).isEmpty());
    QVERIFY(p.outputText(2).isEmpty());
}

void TstMergePlan::duplicatedFileGetsItsOwnOutputRange()
{
    // The same file twice, the second time one page, is the case the Output
    // column exists for: "site-plan.pdf page 12 comes out as page 25".
    MergePlan p;
    p.append(ok(QStringLiteral("/a/plan.pdf"), 12));
    p.append(ok(QStringLiteral("/a/scan.pdf"), 31, QStringLiteral("1-3, 7")));
    p.append(ok(QStringLiteral("/a/draw.pdf"), 8));
    p.append(ok(QStringLiteral("/a/plan.pdf"), 12, QStringLiteral("12")));
    QCOMPARE(p.outputText(3), QStringLiteral("25")); // a single page, not "25-25"
    QCOMPARE(p.countText(3), QStringLiteral("1 of 12"));
    QCOMPARE(p.totalPages(), 25);
}

void TstMergePlan::invalidSpecInvalidatesThePlan()
{
    MergePlan p;
    p.append(ok(QStringLiteral("/a/one.pdf"), 5));
    QVERIFY(p.isValid());
    p.setSpec(0, QStringLiteral("40"));
    QVERIFY(!p.isValid());
    QVERIFY(!p.rowError(0).isEmpty());
    p.setSpec(0, QString()); // empty is an error, not "everything"
    QVERIFY(!p.isValid());
    p.setSpec(0, QStringLiteral("all"));
    QVERIFY(p.isValid());
}

void TstMergePlan::lockedAndUnreadableRowsBlock()
{
    MergePlan p;
    MergePlan::Entry locked;
    locked.path = QStringLiteral("/a/secret.pdf");
    locked.load = MergePlan::Load::Locked;
    p.append(locked);
    QVERIFY(!p.isValid());
    QCOMPARE(p.countText(0), QStringLiteral("Locked"));
    QVERIFY(p.pagesFor(0).isEmpty());

    MergePlan p2;
    MergePlan::Entry bad;
    bad.path = QStringLiteral("/a/gone.pdf");
    bad.load = MergePlan::Load::Unreadable;
    p2.append(bad);
    QVERIFY(!p2.isValid());
    QCOMPARE(p2.countText(0), QStringLiteral("Unreadable"));
}

void TstMergePlan::errorTextNamesTheFirstBadRow()
{
    MergePlan p;
    p.append(ok(QStringLiteral("/a/one.pdf"), 5));
    p.append(ok(QStringLiteral("/a/two.pdf"), 31, QStringLiteral("40")));
    p.append(ok(QStringLiteral("/a/three.pdf"), 5, QStringLiteral("nope")));
    const QString e = p.errorText();
    QVERIFY2(e.startsWith(QStringLiteral("Row 2:")), qPrintable(e));
    QVERIFY2(e.contains(QStringLiteral("31")), qPrintable(e)); // says what the limit is
    QVERIFY(p.errorText().count(QStringLiteral("Row ")) == 1); // one problem at a time
}

void TstMergePlan::displayNameDisambiguatesOnlyDifferentFiles()
{
    MergePlan p;
    p.append(ok(QStringLiteral("/jobs/alpha/plan.pdf"), 1));
    p.append(ok(QStringLiteral("/jobs/beta/plan.pdf"), 1));
    QCOMPARE(p.displayName(0), QStringLiteral("plan.pdf (alpha)"));
    QCOMPARE(p.displayName(1), QStringLiteral("plan.pdf (beta)"));

    // The same file listed twice is genuinely the same name - no folder noise.
    MergePlan q;
    q.append(ok(QStringLiteral("/jobs/alpha/plan.pdf"), 4));
    q.append(ok(QStringLiteral("/jobs/alpha/plan.pdf"), 4, QStringLiteral("1")));
    QCOMPARE(q.displayName(0), QStringLiteral("plan.pdf"));
    QCOMPARE(q.displayName(1), QStringLiteral("plan.pdf"));
}

void TstMergePlan::summaryTextCountsDistinctFiles()
{
    MergePlan p;
    p.append(ok(QStringLiteral("/a/one.pdf"), 12));
    p.append(ok(QStringLiteral("/a/one.pdf"), 12, QStringLiteral("12")));
    // Two rows, one file, 13 pages.
    const QString s = p.summaryText();
    QVERIFY2(s.contains(QStringLiteral("13 pages")), qPrintable(s));
    QVERIFY2(s.contains(QStringLiteral("1 file")), qPrintable(s));
    QVERIFY2(!s.contains(QStringLiteral("1 files")), qPrintable(s)); // plural forms wired up

    MergePlan single;
    single.append(ok(QStringLiteral("/a/one.pdf"), 1));
    QVERIFY2(single.summaryText().contains(QStringLiteral("1 page")),
             qPrintable(single.summaryText()));

    MergePlan empty;
    QCOMPARE(empty.summaryText(), QStringLiteral("Add PDFs to merge."));
}

void TstMergePlan::inputsAreZeroBasedInTypedOrder()
{
    MergePlan p;
    p.append(ok(QStringLiteral("/a/one.pdf"), 10, QStringLiteral("3, 1")));
    p.append(ok(QStringLiteral("/a/two.pdf"), 2));
    const QList<mervin::PageOps::MergeInput> in = p.inputs();
    QCOMPARE(in.size(), 2);
    QCOMPARE(in.at(0).path, QStringLiteral("/a/one.pdf"));
    QCOMPARE(in.at(0).pages, QList<int>({2, 0}));
    QCOMPARE(in.at(1).pages, QList<int>({0, 1}));
}

void TstMergePlan::defaultOutputPathAvoidsExistingAndInputFiles()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString a = dir.filePath(QStringLiteral("report.pdf"));
    QFile f(a);
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write("x");
    f.close();

    MergePlan p;
    p.append(ok(a, 3));
    QCOMPARE(p.defaultOutputPath(), dir.filePath(QStringLiteral("report-merged.pdf")));

    // A file already sitting on the proposed name steps the suffix along.
    QFile taken(dir.filePath(QStringLiteral("report-merged.pdf")));
    QVERIFY(taken.open(QIODevice::WriteOnly));
    taken.write("x");
    taken.close();
    QCOMPARE(p.defaultOutputPath(), dir.filePath(QStringLiteral("report-merged-2.pdf")));

    // And it must never propose one of the inputs: merge reads the sources while
    // it writes, so that name would destroy the file being copied from.
    MergePlan q;
    q.append(ok(a, 3));
    q.append(ok(dir.filePath(QStringLiteral("report-merged-2.pdf")), 1));
    QCOMPARE(q.defaultOutputPath(), dir.filePath(QStringLiteral("report-merged-3.pdf")));

    QVERIFY(MergePlan().defaultOutputPath().isEmpty());
}

void TstMergePlan::rowTextsMatchesThePerRowGetters()
{
    // The dialog reads rowTexts() (one pass) while the per-row getters are what
    // the rest of these cases assert; the two must not drift apart.
    MergePlan p;
    p.append(ok(QStringLiteral("/a/one.pdf"), 12));
    p.append(ok(QStringLiteral("/a/two.pdf"), 31, QStringLiteral("1-3, 7")));
    p.append(ok(QStringLiteral("/a/three.pdf"), 8));
    p.append(ok(QStringLiteral("/a/one.pdf"), 12, QStringLiteral("12")));
    for (int pass = 0; pass < 2; ++pass) {
        const QList<MergePlan::RowText> t = p.rowTexts();
        QCOMPARE(t.size(), p.count());
        for (int i = 0; i < p.count(); ++i) {
            QCOMPARE(t.at(i).count, p.countText(i));
            QCOMPARE(t.at(i).output, p.outputText(i));
        }
        // Second pass with a broken row in the middle: everything from there down
        // must lose its Output in both code paths alike.
        if (pass == 0) {
            p.setSpec(1, QStringLiteral("99"));
            QVERIFY(p.rowTexts().at(2).output.isEmpty());
            QCOMPARE(p.rowTexts().at(0).output, QStringLiteral("1-12"));
        }
    }
}

void TstMergePlan::dropGapsMapToTheRightRow()
{
    // Drag-to-reorder hands over an insertion GAP (0 = above the first row,
    // count() = below the last), numbered against the list as it stands. Lifting
    // the row out closes the gap it occupied, so the mapping is off by one below
    // it - and both gaps adjacent to a row mean "leave it alone". This is the
    // whole of the drop arithmetic; the widget layer only hit-tests the gap,
    // because synthesised drag events cannot be delivered in a test (see the note
    // at the top of tst_merge_dialog.cpp).
    auto abc = [this] {
        MergePlan p;
        p.append(ok(QStringLiteral("/a/a.pdf"), 1));
        p.append(ok(QStringLiteral("/a/b.pdf"), 1));
        p.append(ok(QStringLiteral("/a/c.pdf"), 1));
        return p;
    };
    auto order = [](const MergePlan &p) {
        QStringList s;
        for (const MergePlan::Entry &e : p.entries())
            s << QFileInfo(e.path).fileName();
        return s.join(QLatin1Char(','));
    };

    MergePlan p = abc();
    QCOMPARE(p.moveToGap(2, 0), 0); // last row to the very top
    QCOMPARE(order(p), QStringLiteral("c.pdf,a.pdf,b.pdf"));

    p = abc();
    QCOMPARE(p.moveToGap(0, 3), 2); // first row past the last one
    QCOMPARE(order(p), QStringLiteral("b.pdf,c.pdf,a.pdf"));

    p = abc();
    QCOMPARE(p.moveToGap(0, 2), 1); // one place down
    QCOMPARE(order(p), QStringLiteral("b.pdf,a.pdf,c.pdf"));

    p = abc();
    QCOMPARE(p.moveToGap(2, 1), 1); // one place up
    QCOMPARE(order(p), QStringLiteral("a.pdf,c.pdf,b.pdf"));

    // Both gaps touching a row leave it where it is, and say so.
    for (const auto pair : {std::pair<int, int>{1, 1}, {1, 2}, {0, 0}, {0, 1}, {2, 2}, {2, 3}}) {
        p = abc();
        QVERIFY2(p.moveToGap(pair.first, pair.second) == -1,
                 qPrintable(QStringLiteral("row %1 -> gap %2 should be a no-op")
                                .arg(pair.first).arg(pair.second)));
        QCOMPARE(order(p), QStringLiteral("a.pdf,b.pdf,c.pdf"));
    }

    // Out of range is refused, not clamped into a silent wrong move.
    p = abc();
    QCOMPARE(p.moveToGap(-1, 0), -1);
    QCOMPARE(p.moveToGap(3, 0), -1);
    QCOMPARE(p.moveToGap(0, -1), -1);
    QCOMPARE(p.moveToGap(0, 4), -1);
    QCOMPARE(order(p), QStringLiteral("a.pdf,b.pdf,c.pdf"));
}

QTEST_GUILESS_MAIN(TstMergePlan)
#include "tst_merge_plan.moc"
