// Guard rails for the closed-tab history behind "Reopen Closed Tab" (Ctrl+Shift+T).
//
// The container is small, but every rule in it exists because the obvious
// implementation gets a case wrong: a plain LIFO hands back a document that is
// already open, an unbounded one grows forever, and one that keeps duplicates
// makes the user press the shortcut twice for a file they closed twice. The
// policy (is the file still reopenable?) lives in WindowManager; what is tested
// here is the bookkeeping it drives.

#include "session/ClosedTabStack.h"
#include "session/StartupPlan.h"

#include <QTest>

using mervin::ClosedTab;
using mervin::ClosedTabStack;

namespace {

ClosedTab tab(const QString &path, int index = 0)
{
    ClosedTab t;
    t.path = path;
    t.canonicalPath = path; // the real canonicalisation happens in WindowManager
    t.index = index;
    return t;
}

// ---- the close/reopen replay, without the widgets --------------------------
// The pieces below stand in for MainWindow::rememberOpenTabs() and
// WindowManager::reopenClosedTab(). The container on its own cannot show whether
// a bar comes back in the right order, and that is precisely where this feature
// went wrong first: replaying each tab's stored position as a literal tab index
// scrambled every bulk close, while passing a three-tab manual test by luck.

// MainWindow::rememberOpenTabs(): record the whole bar before anything is
// removed, the current tab LAST so it is the first one back.
void rememberBar(ClosedTabStack &s, const QStringList &bar, int current)
{
    const auto record = [&](int i) {
        ClosedTab t = tab(bar.at(i), i);
        t.siblings = bar;
        s.push(t);
    };
    for (int i = 0; i < bar.size(); ++i)
        if (i != current)
            record(i);
    if (current >= 0 && current < bar.size())
        record(current);
}

// WindowManager::reopenClosedTab(), `presses` times: pop, derive the slot from
// the live bar, insert. `gone` names files that vanished from disk and are
// therefore skipped, as pruneClosedTabs() would drop them.
QStringList replayReopen(ClosedTabStack &s, QStringList bar, int presses,
                         const QStringList &gone = {})
{
    while (presses > 0 && !s.isEmpty()) {
        const ClosedTab t = s.pop();
        if (gone.contains(t.canonicalPath) || bar.contains(t.canonicalPath))
            continue; // skipped without spending the press, as the real loop does
        const int at = mervin::insertIndexForSaved(bar, t.siblings, t.index);
        bar.insert(at < 0 ? bar.size() : at, t.canonicalPath);
        --presses;
    }
    return bar;
}

} // namespace

class TstClosedTabStack : public QObject
{
    Q_OBJECT

private slots:
    void popsMostRecentFirst();
    void popOnEmptyIsHarmless();
    void reclosingMovesTheEntryToTheTop();
    void slotSurvivesTheRoundTrip();
    void entryWithoutIdentityIsIgnored();
    void oldestFallsOffAtTheCap();
    void pruneDropsRejectedEntriesOnly();
    void clearEmpties();
    void closingOneTabPutsItBackWhereItWas();
    void undoingABulkCloseRebuildsTheBar_data();
    void undoingABulkCloseRebuildsTheBar();
    void partialUndoKeepsTheSurvivorsInOrder();
    void aMissingFileLeavesAGapNotAShuffle();
    void undoIntoAnotherWindowKeepsTheGroupTogether();
};

void TstClosedTabStack::popsMostRecentFirst()
{
    // Ctrl+Shift+T walks backwards through the closes, newest first.
    ClosedTabStack s;
    s.push(tab(QStringLiteral("C:/a.pdf")));
    s.push(tab(QStringLiteral("C:/b.pdf")));
    s.push(tab(QStringLiteral("C:/c.pdf")));
    QCOMPARE(s.count(), 3);
    QCOMPARE(s.pop().path, QStringLiteral("C:/c.pdf"));
    QCOMPARE(s.pop().path, QStringLiteral("C:/b.pdf"));
    QCOMPARE(s.pop().path, QStringLiteral("C:/a.pdf"));
    QVERIFY(s.isEmpty());
}

void TstClosedTabStack::popOnEmptyIsHarmless()
{
    // A press with nothing to undo must be a no-op, not a crash: the shortcut
    // stays enabled at all times so it works in a window with no document left.
    ClosedTabStack s;
    QVERIFY(s.isEmpty());
    const ClosedTab t = s.pop();
    QVERIFY(t.path.isEmpty());
    QVERIFY(t.canonicalPath.isEmpty());
    QVERIFY(s.isEmpty());
}

void TstClosedTabStack::reclosingMovesTheEntryToTheTop()
{
    // Close a.pdf, reopen it, close it again: one entry, and it is the newest.
    // Keeping both would cost the user a second press that does nothing, because
    // by then the file is already open and the duplicate gets skipped.
    ClosedTabStack s;
    s.push(tab(QStringLiteral("C:/a.pdf"), 0));
    s.push(tab(QStringLiteral("C:/b.pdf"), 1));
    s.push(tab(QStringLiteral("C:/a.pdf"), 4));
    QCOMPARE(s.count(), 2);

    const ClosedTab first = s.pop();
    QCOMPARE(first.path, QStringLiteral("C:/a.pdf"));
    QCOMPARE(first.index, 4); // and it carries the position of the LAST close
    QCOMPARE(s.pop().path, QStringLiteral("C:/b.pdf"));
}

void TstClosedTabStack::slotSurvivesTheRoundTrip()
{
    // The whole point of storing the index: a reopened tab goes back where it
    // was, not onto the end of the bar.
    ClosedTabStack s;
    s.push(tab(QStringLiteral("C:/a.pdf"), 3));
    QCOMPARE(s.pop().index, 3);
}

void TstClosedTabStack::entryWithoutIdentityIsIgnored()
{
    // No canonical path means nothing can be reopened and nothing can be deduped
    // against - such an entry would only ever waste a press.
    ClosedTabStack s;
    ClosedTab t;
    t.path = QStringLiteral("C:/a.pdf");
    t.index = 0; // canonicalPath deliberately left empty
    s.push(t);
    QVERIFY(s.isEmpty());
}

void TstClosedTabStack::oldestFallsOffAtTheCap()
{
    ClosedTabStack s;
    const int n = ClosedTabStack::kMaxEntries;
    for (int i = 0; i < n + 5; ++i)
        s.push(tab(QStringLiteral("C:/f%1.pdf").arg(i), i));
    QCOMPARE(s.count(), n);

    // The newest is still on top and the five oldest are the ones that went.
    QCOMPARE(s.pop().path, QStringLiteral("C:/f%1.pdf").arg(n + 4));
    ClosedTab last;
    while (!s.isEmpty())
        last = s.pop();
    QCOMPARE(last.path, QStringLiteral("C:/f5.pdf"));
}

void TstClosedTabStack::pruneDropsRejectedEntriesOnly()
{
    // prune() is how the window manager forgets entries whose file is open again
    // or gone from disk. Order among the survivors has to hold, or the next press
    // reopens the wrong document.
    ClosedTabStack s;
    s.push(tab(QStringLiteral("C:/a.pdf")));
    s.push(tab(QStringLiteral("C:/gone.pdf")));
    s.push(tab(QStringLiteral("C:/b.pdf")));
    s.push(tab(QStringLiteral("C:/open.pdf")));

    s.prune([](const ClosedTab &t) {
        return t.path != QStringLiteral("C:/gone.pdf")
            && t.path != QStringLiteral("C:/open.pdf");
    });
    QCOMPARE(s.count(), 2);
    QCOMPARE(s.pop().path, QStringLiteral("C:/b.pdf"));
    QCOMPARE(s.pop().path, QStringLiteral("C:/a.pdf"));

    // A predicate that rejects everything empties the history rather than
    // leaving a stale entry behind an enabled menu row.
    ClosedTabStack all;
    all.push(tab(QStringLiteral("C:/a.pdf")));
    all.push(tab(QStringLiteral("C:/b.pdf")));
    all.prune([](const ClosedTab &) { return false; });
    QVERIFY(all.isEmpty());
}

void TstClosedTabStack::clearEmpties()
{
    ClosedTabStack s;
    s.push(tab(QStringLiteral("C:/a.pdf")));
    s.clear();
    QVERIFY(s.isEmpty());
    QCOMPARE(s.count(), 0);
}

void TstClosedTabStack::closingOneTabPutsItBackWhereItWas()
{
    // The single-close path: the rest of the bar never moved, so the tab has to
    // land back between the same two neighbours.
    const QStringList bar{"A", "B", "C"};
    ClosedTabStack s;
    ClosedTab t = tab(QStringLiteral("B"), 1);
    t.siblings = bar;
    s.push(t);
    QCOMPARE(replayReopen(s, {"A", "C"}, 1), bar);
}

void TstClosedTabStack::undoingABulkCloseRebuildsTheBar_data()
{
    QTest::addColumn<QStringList>("bar");
    QTest::addColumn<int>("current");
    // Which tab is on screen decides the order the entries come BACK in (the
    // current one is restored first), so it is the axis that breaks a replay that
    // trusts the stored index. Every one of these has to rebuild the same bar.
    const QStringList three{"A", "B", "C"};
    const QStringList four{"A", "B", "C", "D"};
    QTest::newRow("3 tabs, first current") << three << 0;
    QTest::newRow("3 tabs, middle current") << three << 1;
    QTest::newRow("3 tabs, last current") << three << 2;
    QTest::newRow("4 tabs, first current") << four << 0;
    QTest::newRow("4 tabs, second current") << four << 1;
    QTest::newRow("4 tabs, third current") << four << 2;
    QTest::newRow("4 tabs, last current") << four << 3;
    QTest::newRow("one tab") << QStringList{"A"} << 0;
    QTest::newRow("no current tab") << three << -1;
}

void TstClosedTabStack::undoingABulkCloseRebuildsTheBar()
{
    QFETCH(QStringList, bar);
    QFETCH(int, current);
    // "Close all tabs" (or closing the window), then Ctrl+Shift+T until the
    // history is empty. The bar must come back exactly as it was.
    ClosedTabStack s;
    rememberBar(s, bar, current);
    QCOMPARE(s.count(), bar.size());
    QCOMPARE(replayReopen(s, {}, bar.size()), bar);
}

void TstClosedTabStack::partialUndoKeepsTheSurvivorsInOrder()
{
    // Stopping half way through must not leave the restored tabs jumbled: what is
    // back has to be in its original relative order, ready for the next press.
    const QStringList bar{"A", "B", "C", "D"};
    ClosedTabStack s;
    rememberBar(s, bar, /*current=*/2); // C was on screen, so C comes back first
    QCOMPARE(replayReopen(s, {}, 1), QStringList{"C"});

    ClosedTabStack s2;
    rememberBar(s2, bar, 2);
    QCOMPARE(replayReopen(s2, {}, 2), (QStringList{"C", "D"}));

    ClosedTabStack s3;
    rememberBar(s3, bar, 2);
    QCOMPARE(replayReopen(s3, {}, 3), (QStringList{"B", "C", "D"}));
}

void TstClosedTabStack::aMissingFileLeavesAGapNotAShuffle()
{
    // A document deleted from disk between the close and the undo is skipped. The
    // ones on either side of it must still come back in order rather than closing
    // ranks in the wrong sequence.
    const QStringList bar{"A", "B", "C", "D"};
    ClosedTabStack s;
    rememberBar(s, bar, /*current=*/0);
    QCOMPARE(replayReopen(s, {}, 4, /*gone=*/{QStringLiteral("C")}),
             (QStringList{"A", "B", "D"}));
}

void TstClosedTabStack::undoIntoAnotherWindowKeepsTheGroupTogether()
{
    // Closing a window and undoing it in a DIFFERENT window: the restored block
    // stays contiguous and in order rather than interleaving with the tabs that
    // window already had (insertIndexForSaved counts only known siblings, which
    // is the same rule the staged session restore follows).
    const QStringList closedWindow{"A", "B", "C"};
    ClosedTabStack s;
    rememberBar(s, closedWindow, /*current=*/1);
    const QStringList got = replayReopen(s, {"X", "Y"}, 3);
    QCOMPARE(got.size(), 5);
    QCOMPARE(got.indexOf(QStringLiteral("A")) + 1, got.indexOf(QStringLiteral("B")));
    QCOMPARE(got.indexOf(QStringLiteral("B")) + 1, got.indexOf(QStringLiteral("C")));
    QVERIFY(got.indexOf(QStringLiteral("C")) < got.indexOf(QStringLiteral("X")));
}

QTEST_GUILESS_MAIN(TstClosedTabStack)
#include "tst_closed_tab_stack.moc"
