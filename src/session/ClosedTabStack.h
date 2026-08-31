#pragma once

#include <QList>
#include <QString>
#include <QStringList>

namespace mervin {

// One document tab that was closed, kept so "Reopen Closed Tab" (Ctrl+Shift+T)
// can bring it back where it was.
//
// Deliberately no page / zoom / scroll position. Closing a tab already pushes
// that to the per-file view-state store (MainWindow::saveTabViewState), and
// reopening the file restores it through the ordinary resume path - duplicating
// it here would give two sources of truth that drift apart the moment a file is
// closed in one window while open in another.
struct ClosedTab
{
    QString path;          // the file as it was opened, and how it is reopened
    QString canonicalPath; // identity: dedup, and the "is it open again?" test

    // The bar this tab was closed from - every tab's canonical path, in order -
    // and this tab's position in it.
    //
    // `index` is NOT replayed as a literal tab index. The slot is re-derived from
    // the live bar at reopen time with insertIndexForSaved() (session/StartupPlan.h),
    // exactly as the staged session restore does, and for exactly the same reason:
    // a stored index is only right while the rest of the bar is untouched. Replay
    // absolute indices into a bar being rebuilt from empty by repeated presses and
    // every index that overshoots the live count silently appends instead of
    // inserting, so a whole window's tabs come back scrambled. Keeping the sibling
    // order lets each tab land after whichever of its neighbours are already back.
    QStringList siblings;
    int index = 0;
};

// Bounded most-recently-closed-first history of closed tabs.
//
// Process-wide, not per-window: tabs move between windows (detach / merge), and
// a tab closed in a window that has since gone should still be reachable from
// the window the user is looking at now. Plain value type, unit-testable; the
// WindowManager owns the one live instance and supplies the reopen policy.
class ClosedTabStack
{
public:
    // Chrome remembers roughly this many. Each entry is two strings and an int,
    // so the cap is about bounding the history's age, not its memory.
    static constexpr int kMaxEntries = 25;

    // Record a closed tab as the newest entry.
    //
    // An older entry for the same canonical path is dropped rather than kept as a
    // duplicate: closing the same document twice should not cost two presses to
    // undo, and the second press would find the file already open and skip its
    // entry anyway. An entry with no canonical path is ignored - there is nothing
    // to reopen.
    void push(const ClosedTab &tab);

    bool isEmpty() const { return tabs_.isEmpty(); }
    int count() const { return int(tabs_.size()); }

    // Remove and return the most recently closed tab; a default-constructed
    // ClosedTab when the history is empty.
    ClosedTab pop();

    // Drop every entry `keep` rejects. The caller owns the policy (is the file
    // open again? is it still on disk?); this only knows how to forget. Run
    // before the history is used, so "there is nothing to bring back" means the
    // real thing and not just "the newest entry happens to be stale".
    template <typename Pred>
    void prune(Pred keep)
    {
        for (int i = int(tabs_.size()) - 1; i >= 0; --i)
            if (!keep(tabs_.at(i)))
                tabs_.removeAt(i);
    }

    void clear() { tabs_.clear(); }

private:
    QList<ClosedTab> tabs_; // oldest first; newest last, so push/pop work the back
};

} // namespace mervin
