#pragma once

#include <QList>
#include <QString>
#include <QStringList>

namespace mervin {

// One document in a staged open (see WindowManager::openStaged): which document,
// where its tab belongs, and whether it takes the view when it lands.
struct StagedOpen
{
    QString path;
    // Position of this document in the saved session, or -1 for a document that is
    // not part of it (a command-line file), which appends. NOT a tab index - the
    // tab index is derived from the live tab bar at open time by
    // insertIndexForSaved, because a precomputed one goes wrong the moment an
    // earlier open does not land where the plan assumed.
    int savedIndex = -1;
    bool makeCurrent = true;
    bool lazy = false;
    bool allowDuplicate = false; // independent recovery copies of the same original
};

// Open command-line files first, then the restored active document, then remaining session
// files in saved order. Exactly one becomes current: the last command-line file, otherwise the
// saved active file or first restored file.
// Callers filter missing sessionPaths. Paths shared with the command line open once but retain
// their saved tab positions. sessionActive may be empty.
QList<StagedOpen> planStartupOpens(const QStringList &cliPaths,
                                   const QStringList &sessionPaths,
                                   const QString &sessionActive);

// Place a restored tab after its live predecessors in savedOrder and before other tabs. Derive
// from currentTabs on every open so failed, canceled and duplicate opens leave the remaining
// order intact. Both lists use canonical paths; savedIndex < 0 returns -1 (append).
int insertIndexForSaved(const QStringList &currentTabs, const QStringList &savedOrder,
                        int savedIndex);

} // namespace mervin
