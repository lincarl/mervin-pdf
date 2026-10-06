#include "session/ClosedTabStack.h"

namespace mervin {

void ClosedTabStack::push(const ClosedTab &tab)
{
    if (tab.canonicalPath.isEmpty())
        return; // nothing identifiable to reopen

    // One entry per document, newest position wins.
    for (int i = int(tabs_.size()) - 1; i >= 0; --i)
        if (tabs_.at(i).canonicalPath == tab.canonicalPath)
            tabs_.removeAt(i);

    tabs_.append(tab);
    while (tabs_.size() > kMaxEntries)
        tabs_.removeFirst(); // the oldest falls off the back of the history
}

ClosedTab ClosedTabStack::pop()
{
    return tabs_.isEmpty() ? ClosedTab{} : tabs_.takeLast();
}

} // namespace mervin
