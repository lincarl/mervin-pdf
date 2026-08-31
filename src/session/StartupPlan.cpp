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
        o.savedIndex = -1;    // not part of the session: appends, after the restored block
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
        batch.append(o);
    }
    return batch;
}

int insertIndexForSaved(const QStringList &currentTabs, const QStringList &savedOrder,
                        int savedIndex)
{
    if (savedIndex < 0)
        return -1; // append

    int at = 0;
    for (const QString &tab : currentTabs) {
        const int pos = savedOrder.indexOf(tab);
        if (pos >= 0 && pos < savedIndex)
            ++at;
    }
    return at;
}

} // namespace mervin
