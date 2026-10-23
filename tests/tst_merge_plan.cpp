#include "i18n/UiLanguage.h"
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
    void initTestCase();

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
    void rowTextsUseIndependentExpectedRows();
    void dropGapsMapToTheRightRow();
    void readingBlocksUnknownOutputPositions();
    void readingReasonFollowsOrderAndCompletion();
    void probeResolutionFollowsPendingCopies();
    void encryptionFollowsTheFilesRead();
    void aliasesUseWorkerIdentity();

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

void TstMergePlan::initTestCase()
{
    // The English plural forms ("1 page", "13 pages") come from the English
    // catalog; without it Qt would show "13 page(s)".
    mervin::i18n::apply(QStringLiteral("en"));
}

/// Checks that pending counts block Merge and hide dependent output positions.
void TstMergePlan::readingBlocksUnknownOutputPositions()
{
    MergePlan plan;
    plan.append(ok(QStringLiteral("/a/first.pdf"), 2));
    MergePlan::Entry pending;
    pending.path = QStringLiteral("/b/reading.pdf");
    pending.load = MergePlan::Load::Reading;
    pending.probeTicket = 1;
    plan.append(pending);
    plan.append(ok(QStringLiteral("/a/later.pdf"), 3));
    QVERIFY(plan.hasReading());
    QVERIFY(!plan.isValid());
    QCOMPARE(plan.errorText(), QStringLiteral("Row 2: \"\u2068reading.pdf\u2069\" is still being read."));
    QCOMPARE(plan.countText(1), QStringLiteral("Reading…"));
    QCOMPARE(plan.outputText(0), QStringLiteral("1-2"));
    QVERIFY(plan.outputText(1).isEmpty() && plan.outputText(2).isEmpty());
    auto result = ok(pending.path, 4);
    QVERIFY(plan.resolveProbe(1, result));
    QVERIFY(plan.isValid() && !plan.hasReading());
    QVERIFY(plan.errorText().isEmpty());
    QCOMPARE(plan.outputText(2), QStringLiteral("7-9"));
}

/// Checks that the footer follows pending rows before reporting settled failures.
void TstMergePlan::readingReasonFollowsOrderAndCompletion()
{
    MergePlan plan;
    auto failed = ok(QStringLiteral("/a/missing.pdf"), 0);
    failed.load = MergePlan::Load::Unreadable;
    plan.append(failed);
    auto first = ok(QStringLiteral("/a/first.pdf"), 0);
    first.load = MergePlan::Load::Reading;
    first.probeTicket = 1;
    plan.append(first);
    auto second = first;
    second.path = QStringLiteral("/b/second.pdf");
    second.probeTicket = 2;
    plan.append(second);
    QCOMPARE(plan.errorText(), QStringLiteral("Row 2: \"\u2068first.pdf\u2069\" is still being read."));
    QCOMPARE(plan.moveToGap(2, 0), 0);
    QCOMPARE(plan.errorText(), QStringLiteral("Row 1: \"\u2068second.pdf\u2069\" is still being read."));
    QVERIFY(plan.resolveProbe(2, ok(second.path, 1)));
    QCOMPARE(plan.errorText(), QStringLiteral("Row 3: \"\u2068first.pdf\u2069\" is still being read."));
    plan.remove(0);
    QCOMPARE(plan.errorText(), QStringLiteral("Row 2: \"\u2068first.pdf\u2069\" is still being read."));
    QVERIFY(plan.resolveProbe(1, ok(first.path, 1)));
    QCOMPARE(plan.errorText(), QStringLiteral("Row 1: \"missing.pdf\" could not be read."));
}

/// Checks probe identity across duplication, reordered ranges and stale answers.
void TstMergePlan::probeResolutionFollowsPendingCopies()
{
    MergePlan plan;
    MergePlan::Entry pending;
    pending.path = QStringLiteral("/a/file.pdf");
    pending.load = MergePlan::Load::Reading;
    pending.probeTicket = 7;
    pending.spec = QStringLiteral("3,1");
    plan.append(pending);
    plan.duplicate(0);
    plan.setSpec(1, QStringLiteral("2"));
    plan.append(ok(QStringLiteral("/b/ready.pdf"), 1));
    QCOMPARE(plan.moveToGap(1, 3), 2);
    const auto result = ok(pending.path, 3);
    QVERIFY(!plan.resolveProbe(99, result));
    QVERIFY(plan.resolveProbe(7, result));
    QCOMPARE(plan.pagesFor(0), (QList<int>{2, 0}));
    QCOMPARE(plan.pagesFor(2), (QList<int>{1}));
    QVERIFY(!plan.resolveProbe(7, result));
    QCOMPARE(plan.at(1).path, QStringLiteral("/b/ready.pdf"));
}

/// Checks that only a plan reading one file keeps its encryption and opens with its password.
void TstMergePlan::encryptionFollowsTheFilesRead()
{
    MergePlan plan;
    QVERIFY(!plan.losesEncryption());
    QCOMPARE(plan.resultPassword(), QString());

    // The tab's encrypted document in two rows is still one file for the merge.
    MergePlan::Entry pending;
    pending.path = QStringLiteral("/a/locked.pdf");
    pending.load = MergePlan::Load::Reading;
    pending.probeTicket = 3;
    plan.append(pending);
    auto locked = ok(pending.path, 3);
    locked.password = QStringLiteral("secret");
    locked.encrypted = true;
    QVERIFY(plan.resolveProbe(3, locked));
    QVERIFY(plan.at(0).encrypted);
    plan.duplicate(0);
    QVERIFY(!plan.losesEncryption());
    QCOMPARE(plan.resultPassword(), QStringLiteral("secret"));

    // Another file makes the merge read two, so the result has no encryption.
    plan.append(ok(QStringLiteral("/a/plain.pdf"), 1));
    QVERIFY(plan.losesEncryption());
    QCOMPARE(plan.resultPassword(), QString());
    plan.remove(2);
    QVERIFY(!plan.losesEncryption());

    // Plain files lose nothing, and a row that did not open does not count.
    MergePlan plain;
    plain.append(ok(QStringLiteral("/a/one.pdf"), 1));
    plain.append(ok(QStringLiteral("/a/two.pdf"), 1));
    QVERIFY(!plain.losesEncryption());
    auto unopened = ok(QStringLiteral("/a/other.pdf"), 0);
    unopened.load = MergePlan::Load::Locked;
    unopened.encrypted = true;
    plain.append(unopened);
    QVERIFY(!plain.losesEncryption());
    // A file with only an owner password opens without one and still counts.
    auto restricted = ok(QStringLiteral("/a/restricted.pdf"), 2);
    restricted.encrypted = true;
    plain.append(restricted);
    QVERIFY(plain.losesEncryption());
    QCOMPARE(plain.resultPassword(), QString());
}

/// Uses supplied probe identities even for paths that the UI cannot resolve itself.
void TstMergePlan::aliasesUseWorkerIdentity()
{
    MergePlan plan;
    auto first = ok(QStringLiteral("/unavailable/source.pdf"), 3, QStringLiteral("3"));
    first.encrypted = true;
    first.password = QStringLiteral("secret");
    first.sourceKey = QStringLiteral("/resolved/source.pdf");
    plan.append(first);
    auto alias = first;
    alias.path = QStringLiteral("/unavailable/link.pdf");
    alias.sourceKey.clear();
    alias.spec = QStringLiteral("1,3");
    alias.load = MergePlan::Load::Reading;
    alias.probeTicket = 9;
    plan.append(alias);
    auto result = first;
    result.path = alias.path;
    QVERIFY(plan.resolveProbe(9, result));
    QCOMPARE(plan.at(1).sourceKey, first.sourceKey);
    QVERIFY(!plan.losesEncryption());
    QCOMPARE(plan.resultPassword(), QStringLiteral("secret"));
    QCOMPARE(plan.summaryText(), QStringLiteral("Result: 3 pages from 1 file, in the order shown."));
    QCOMPARE(plan.inputs().at(1).path, alias.path);
    QCOMPARE(plan.inputs().at(1).pages, (QList<int>{0, 2}));
    plan.duplicate(1);
    plan.move(2, -2);
    QVERIFY(!plan.losesEncryption());
    auto other = first;
    other.sourceKey = QStringLiteral("/resolved/other.pdf");
    plan.append(other);
    QVERIFY(plan.losesEncryption());
    QVERIFY(plan.resultPassword().isEmpty());
}

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

/// Checks translated count text for complete, partial and pending inputs.
void TstMergePlan::countTextDistinguishesAllFromASubset()
{
    MergePlan p;
    p.append(ok(QStringLiteral("/a/one.pdf"), 31));
    QCOMPARE(p.at(0).spec, QStringLiteral("All")); // a new row takes every page
    QCOMPARE(p.countText(0), QStringLiteral("31"));
    p.setSpec(0, QStringLiteral("1-3, 7"));
    QCOMPARE(p.countText(0), QStringLiteral("4 of 31"));
    p.setSpec(0, QStringLiteral("nonsense"));
    QCOMPARE(p.countText(0), QStringLiteral("-"));
    p.setSpec(0, QStringLiteral("all"));
    QCOMPARE(p.countText(0), QStringLiteral("31"));

    // What the dialog sizes its Count column for.
    QCOMPARE(MergePlan::widestCountTexts(),
             QStringList({QStringLiteral("Locked"), QStringLiteral("Unreadable"),
                          QStringLiteral("998 of 999"), QStringLiteral("Reading…")}));
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
    QCOMPARE(p.summaryText(), QStringLiteral("Result: 13 pages from 1 file, in the order shown."));

    p.append(ok(QStringLiteral("/a/two.pdf"), 2));
    QCOMPARE(p.summaryText(), QStringLiteral("Result: 15 pages from 2 files, in the order shown."));

    MergePlan single;
    single.append(ok(QStringLiteral("/a/one.pdf"), 1));
    QCOMPARE(single.summaryText(),
             QStringLiteral("Result: 1 page from 1 file, in the order shown."));

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

void TstMergePlan::rowTextsUseIndependentExpectedRows()
{
    // Both APIs must match the planned counts and output positions. outputText()
    // delegates to rowTexts(), so comparing the APIs cannot verify those values.
    MergePlan p;
    p.append(ok(QStringLiteral("/a/one.pdf"), 12));
    p.append(ok(QStringLiteral("/a/two.pdf"), 31, QStringLiteral("1-3, 7")));
    p.append(ok(QStringLiteral("/a/three.pdf"), 8));
    p.append(ok(QStringLiteral("/a/one.pdf"), 12, QStringLiteral("12")));
    const QList<MergePlan::RowText> expected[] = {
        {{QStringLiteral("12"), QStringLiteral("1-12")},
         {QStringLiteral("4 of 31"), QStringLiteral("13-16")},
         {QStringLiteral("8"), QStringLiteral("17-24")},
         {QStringLiteral("1 of 12"), QStringLiteral("25")}},
        {{QStringLiteral("12"), QStringLiteral("1-12")},
         {QStringLiteral("-"), QString()},
         {QStringLiteral("8"), QString()},
         {QStringLiteral("1 of 12"), QString()}},
    };
    QCOMPARE(p.count(), 4);
    for (int pass = 0; pass < 2; ++pass) {
        // A broken row leaves later counts intact but their output unknown.
        if (pass == 1)
            p.setSpec(1, QStringLiteral("99"));
        const QList<MergePlan::RowText> t = p.rowTexts();
        QCOMPARE(t.size(), 4);
        for (int i = 0; i < 4; ++i) {
            QCOMPARE(t.at(i).count, expected[pass].at(i).count);
            QCOMPARE(t.at(i).output, expected[pass].at(i).output);
            QCOMPARE(p.countText(i), expected[pass].at(i).count);
            QCOMPARE(p.outputText(i), expected[pass].at(i).output);
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
