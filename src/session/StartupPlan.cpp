#include "session/StartupPlan.h"

namespace mervin {

QList<StagedOpen> planStartupOpens(const QStringList &cliPaths, const QStringList &sessionPaths,
                                   const QString &sessionActive)
{
    QList<StagedOpen> batch;
    batch.reserve(cliPaths.size() + sessionPaths.size());

    for (const QString &p : cliPaths) {
        StagedOpen o;
        o.path = p;
        o.savedIndex = -1;    // appends, and stays where its batch opens it
        o.makeCurrent = true; // opened in order, so the last one ends up on screen
        batch.append(o);
    }

    if (sessionPaths.isEmpty())
        return batch;

    // Open order: the previously-active document, then the rest as saved. With
    // nothing recorded (a session file written before the field existed) the first
    // saved document stands in, so exactly one document still takes the view.
    int activeIdx = sessionPaths.indexOf(sessionActive);
    if (activeIdx < 0)
        activeIdx = 0;

    QList<int> order;
    order.reserve(sessionPaths.size());
    order.append(activeIdx);
    for (int i = 0; i < sessionPaths.size(); ++i)
        if (i != activeIdx)
            order.append(i);

    for (int k = 0; k < order.size(); ++k) {
        StagedOpen o;
        o.path = sessionPaths.at(order.at(k));
        o.savedIndex = order.at(k);
        o.makeCurrent = (k == 0 && cliPaths.isEmpty());
        o.lazy = true;
        batch.append(o);
    }
    return batch;
}

int insertIndexForSaved(const QStringList &currentTabs, const QStringList &savedOrder,
                        int savedIndex)
{
    if (savedIndex < 0)
        return -1; // append

    // Find the last predecessor rather than count them. A count is an index only
    // while every predecessor sits at the front, which a command-line tab or a
    // moved tab breaks.
    for (int i = int(currentTabs.size()) - 1; i >= 0; --i) {
        const int pos = savedOrder.indexOf(currentTabs.at(i));
        if (pos >= 0 && pos < savedIndex)
            return i + 1;
    }
    return 0;
}

} // namespace mervin
