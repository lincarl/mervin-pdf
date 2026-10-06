#pragma once

#include <QList>
#include <QString>
#include <QStringList>

namespace mervin {

// Closed-tab identity for Ctrl+Shift+T. View position lives only in the per-file view-state
// store. Passwords are not retained: reopening encrypted files prompts again.
struct ClosedTab
{
    QString path;          // the file as it was opened, and how it is reopened
    QString canonicalPath; // identity: dedup, and the "is it open again?" test

    // Canonical sibling paths and the original position. Reopen derives the live insertion slot
    // with insertIndexForSaved; replaying absolute indices would scramble partially restored
    // bars.
    QStringList siblings;
    int index = 0;
};

// Bounded process-wide closed-tab history, owned by WindowManager. Reopening survives tab moves
// and destruction of the original window.
class ClosedTabStack
{
public:
    // Chrome remembers roughly this many. Each entry is two strings and an int,
    // so the cap is about bounding the history's age, not its memory.
    static constexpr int kMaxEntries = 25;

    // Prepend a closed tab, replacing any entry for the same canonical path. Ignore entries
    // without a canonical path.
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
