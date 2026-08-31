#include "session/StartupPlan.h"

#include <QStringList>
#include <QTest>

using mervin::insertIndexForSaved;
using mervin::planStartupOpens;
using mervin::StagedOpen;

// The startup open plan decides what a launch opens first and where each tab
// lands. Two properties matter and are easy to break silently:
//   * the document that takes the view (the file the user double-clicked, or the
//     one they were last reading - never an arbitrary one), and
//   * the final tab order, which must stay what inline restore produced, or every
//     launch reshuffles the user's tab bar - and the reshuffle is then written
//     back to session.json, so it compounds.
class TstStartupPlan : public QObject
{
    Q_OBJECT

private slots:
    void commandLineFileOpensFirstAndTakesTheView();
    void restoreOpensActiveDocumentFirst();
    void restoreKeepsSavedTabOrder();
    void restoreWithoutRecordedActiveUsesFirst();
    void staleActivePathIsIgnored();
    void commandLineFileKeepsTheViewDuringRestore();
    void commandLineFileFromTheSessionKeepsItsSavedSlot();
    void documentThatFailsToOpenLeavesTheRestInOrder();
    void insertIndexIsRelativeToTheLiveTabBar();
    void emptyInputsPlanNothing();
};

namespace {

struct Replay
{
    QStringList tabs;
    QString current;
};

// Replay a plan exactly as WindowManager::stagedStep and MainWindow::openFile
// apply it, so the assertions below are about the tab bar the user ends up looking
// at rather than about index arithmetic:
//   - the insert index is derived from the LIVE tab list via insertIndexForSaved,
//   - a document that is already open is a no-op, and a background open
//     (makeCurrent=false) does not even take the view,
//   - a document in `failing` never produces a tab (corrupt file, or the user
//     cancelled its password prompt), and
//   - the first tab in a window always takes the view, since a window with no
//     tabs is showing the Recent page.
Replay applyPlan(const QList<StagedOpen> &plan, const QStringList &savedOrder,
                 const QStringList &failing = {})
{
    Replay r;
    for (const StagedOpen &o : plan) {
        if (r.tabs.contains(o.path)) {
            if (o.makeCurrent)
                r.current = o.path;
            continue;
        }
        if (failing.contains(o.path))
            continue;
        const int at = insertIndexForSaved(r.tabs, savedOrder, o.savedIndex);
        r.tabs.insert((at >= 0 && at <= r.tabs.size()) ? at : r.tabs.size(), o.path);
        if (o.makeCurrent || r.tabs.size() == 1)
            r.current = o.path;
    }
    return r;
}

int currentCount(const QList<StagedOpen> &plan)
{
    int n = 0;
    for (const StagedOpen &o : plan)
        if (o.makeCurrent)
            ++n;
    return n;
}

} // namespace

void TstStartupPlan::commandLineFileOpensFirstAndTakesTheView()
{
    // The whole point of the ordering: a double-clicked file must not queue up
    // behind the restored session.
    const QStringList saved = {QStringLiteral("A.pdf"), QStringLiteral("B.pdf")};
    const QList<StagedOpen> plan =
        planStartupOpens({QStringLiteral("X.pdf")}, saved, QStringLiteral("B.pdf"));
    QCOMPARE(plan.size(), 3);
    QCOMPARE(plan.at(0).path, QStringLiteral("X.pdf")); // opened first
    QCOMPARE(plan.at(0).savedIndex, -1);                // not from the session: appends

    const Replay r = applyPlan(plan, saved);
    QCOMPARE(r.current, QStringLiteral("X.pdf"));
    // Tab order as inline restore left it: restored documents, then the new file.
    QCOMPARE(r.tabs, QStringList({QStringLiteral("A.pdf"), QStringLiteral("B.pdf"),
                                  QStringLiteral("X.pdf")}));
}

void TstStartupPlan::restoreOpensActiveDocumentFirst()
{
    const QStringList saved = {QStringLiteral("A.pdf"), QStringLiteral("B.pdf"),
                               QStringLiteral("C.pdf"), QStringLiteral("D.pdf")};
    const QList<StagedOpen> plan = planStartupOpens({}, saved, QStringLiteral("C.pdf"));
    QCOMPARE(plan.size(), 4);
    QCOMPARE(plan.at(0).path, QStringLiteral("C.pdf")); // the document being read
    QVERIFY(plan.at(0).makeCurrent);
    QCOMPARE(currentCount(plan), 1); // and nothing else steals the view later

    const Replay r = applyPlan(plan, saved);
    QCOMPARE(r.tabs, saved); // order preserved
    QCOMPARE(r.current, QStringLiteral("C.pdf"));
}

void TstStartupPlan::restoreKeepsSavedTabOrder()
{
    // Every possible active document, since the open order is derived from where it
    // sat: the tab bar must come back identical in all of them.
    const QStringList saved = {QStringLiteral("A.pdf"), QStringLiteral("B.pdf"),
                               QStringLiteral("C.pdf"), QStringLiteral("D.pdf"),
                               QStringLiteral("E.pdf")};
    for (const QString &active : saved) {
        const QList<StagedOpen> plan = planStartupOpens({}, saved, active);
        const Replay r = applyPlan(plan, saved);
        QVERIFY2(r.tabs == saved, qPrintable(QStringLiteral("active=%1 gave %2")
                                                .arg(active, r.tabs.join(QLatin1Char(',')))));
        QCOMPARE(r.current, active);
        QCOMPARE(plan.at(0).path, active);
    }
}

void TstStartupPlan::restoreWithoutRecordedActiveUsesFirst()
{
    // A session file written before the "active" field existed. Exactly one
    // document must still take the view, or the window would sit on the Recent
    // page with documents loaded invisibly behind it.
    const QStringList saved = {QStringLiteral("A.pdf"), QStringLiteral("B.pdf")};
    const QList<StagedOpen> plan = planStartupOpens({}, saved, QString());
    QCOMPARE(plan.size(), 2);
    QCOMPARE(plan.at(0).path, QStringLiteral("A.pdf"));
    QCOMPARE(currentCount(plan), 1);

    const Replay r = applyPlan(plan, saved);
    QCOMPARE(r.tabs, saved);
    QCOMPARE(r.current, QStringLiteral("A.pdf"));
}

void TstStartupPlan::staleActivePathIsIgnored()
{
    // An active path that is not among the restored documents (its file was
    // deleted, so the caller filtered it out) falls back to the first document.
    const QStringList saved = {QStringLiteral("A.pdf"), QStringLiteral("B.pdf")};
    const QList<StagedOpen> plan = planStartupOpens({}, saved, QStringLiteral("gone.pdf"));
    QCOMPARE(plan.size(), 2);
    QCOMPARE(plan.at(0).path, QStringLiteral("A.pdf"));
    QCOMPARE(currentCount(plan), 1);
    const Replay r = applyPlan(plan, saved);
    QCOMPARE(r.tabs, saved);
    QCOMPARE(r.current, QStringLiteral("A.pdf"));
}

void TstStartupPlan::commandLineFileKeepsTheViewDuringRestore()
{
    // With a file on the command line, the restored session must not pull the view
    // off it - not even the document that was active last time.
    const QStringList saved = {QStringLiteral("A.pdf"), QStringLiteral("B.pdf")};
    const QList<StagedOpen> plan =
        planStartupOpens({QStringLiteral("X.pdf")}, saved, QStringLiteral("A.pdf"));
    QCOMPARE(currentCount(plan), 1);
    QVERIFY(plan.at(0).makeCurrent);
    QCOMPARE(plan.at(0).path, QStringLiteral("X.pdf"));
    const Replay r = applyPlan(plan, saved);
    QCOMPARE(r.tabs, QStringList({QStringLiteral("A.pdf"), QStringLiteral("B.pdf"),
                                  QStringLiteral("X.pdf")}));
    QCOMPARE(r.current, QStringLiteral("X.pdf"));

    // Several files on the command line: the last one ends up on screen, which is
    // what opening them inline did.
    const QStringList saved1 = {QStringLiteral("A.pdf")};
    const QList<StagedOpen> multi = planStartupOpens(
        {QStringLiteral("X.pdf"), QStringLiteral("Y.pdf")}, saved1, QStringLiteral("A.pdf"));
    const Replay rm = applyPlan(multi, saved1);
    QCOMPARE(rm.tabs, QStringList({QStringLiteral("A.pdf"), QStringLiteral("X.pdf"),
                                   QStringLiteral("Y.pdf")}));
    QCOMPARE(rm.current, QStringLiteral("Y.pdf"));
}

void TstStartupPlan::commandLineFileFromTheSessionKeepsItsSavedSlot()
{
    // Double-clicking a file that was already in the last session is the most
    // ordinary launch there is (reopening what you were reading). It must take the
    // view AND stay in its saved tab position: the resulting order is written back
    // to session.json, so a reshuffle here migrates that tab one step every launch.
    // Both orderings matter - the failure only showed when the command-line file
    // sat BEFORE the previously-active document in the saved order.
    const QStringList saved = {QStringLiteral("A.pdf"), QStringLiteral("B.pdf"),
                               QStringLiteral("C.pdf")};
    for (const QString &active : saved) {
        for (const QString &cli : saved) {
            const QList<StagedOpen> plan = planStartupOpens({cli}, saved, active);
            const Replay r = applyPlan(plan, saved);
            QVERIFY2(r.tabs == saved,
                     qPrintable(QStringLiteral("cli=%1 active=%2 gave %3")
                                    .arg(cli, active, r.tabs.join(QLatin1Char(',')))));
            // The double-clicked file is what the user asked to see.
            QCOMPARE(r.current, cli);
            // And it is opened first, not after the rest of the session.
            QCOMPARE(plan.at(0).path, cli);
        }
    }
}

void TstStartupPlan::documentThatFailsToOpenLeavesTheRestInOrder()
{
    // One document that never produces a tab - corrupt, or a password prompt the
    // user cancelled - must leave a gap, not shift everything after it. The
    // previously-active document is opened out of saved order, so a naive
    // precomputed index strands it at the wrong end of the bar.
    const QStringList saved = {QStringLiteral("A.pdf"), QStringLiteral("B.pdf"),
                               QStringLiteral("C.pdf"), QStringLiteral("D.pdf"),
                               QStringLiteral("E.pdf")};
    for (const QString &fails : saved) {
        for (const QString &active : saved) {
            if (fails == active)
                continue; // the active document failing is covered by the gap case below
            QStringList expect = saved;
            expect.removeAll(fails);
            const Replay r = applyPlan(planStartupOpens({}, saved, active), saved, {fails});
            QVERIFY2(r.tabs == expect,
                     qPrintable(QStringLiteral("failed=%1 active=%2 gave %3, wanted %4")
                                    .arg(fails, active, r.tabs.join(QLatin1Char(',')),
                                         expect.join(QLatin1Char(',')))));
            QCOMPARE(r.current, active);
        }
    }

    // The active document itself failing: the rest still come back in order, and
    // the first tab that does land takes the view (its window has none).
    const Replay r = applyPlan(planStartupOpens({}, saved, QStringLiteral("C.pdf")), saved,
                               {QStringLiteral("C.pdf")});
    QCOMPARE(r.tabs, QStringList({QStringLiteral("A.pdf"), QStringLiteral("B.pdf"),
                                  QStringLiteral("D.pdf"), QStringLiteral("E.pdf")}));
    QCOMPARE(r.current, QStringLiteral("A.pdf"));
}

void TstStartupPlan::insertIndexIsRelativeToTheLiveTabBar()
{
    const QStringList saved = {QStringLiteral("A.pdf"), QStringLiteral("B.pdf"),
                               QStringLiteral("C.pdf")};
    // Not part of the session -> append.
    QCOMPARE(insertIndexForSaved({QStringLiteral("A.pdf")}, saved, -1), -1);
    // Empty bar -> front, whatever the saved position.
    QCOMPARE(insertIndexForSaved({}, saved, 2), 0);
    // Only documents from the session that precede it are counted; a
    // command-line-only tab is not, so restored tabs land ahead of it.
    QCOMPARE(insertIndexForSaved({QStringLiteral("X.pdf")}, saved, 0), 0);
    QCOMPARE(insertIndexForSaved({QStringLiteral("A.pdf"), QStringLiteral("X.pdf")}, saved, 1), 1);
    QCOMPARE(insertIndexForSaved({QStringLiteral("C.pdf")}, saved, 1), 0);
    QCOMPARE(insertIndexForSaved({QStringLiteral("A.pdf"), QStringLiteral("C.pdf")}, saved, 1), 1);
}

void TstStartupPlan::emptyInputsPlanNothing()
{
    QVERIFY(planStartupOpens({}, {}, QString()).isEmpty());
    QVERIFY(planStartupOpens({}, {}, QStringLiteral("A.pdf")).isEmpty());
}

QTEST_GUILESS_MAIN(TstStartupPlan)
#include "tst_startup_plan.moc"
