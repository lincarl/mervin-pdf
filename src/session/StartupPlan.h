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
};

// Build the startup open plan: what to open and in which order. Two rules, both
// about a wait the user actually notices:
//
//   * Files named on the command line come FIRST. A launch almost always happens
//     because someone double-clicked a PDF, and that file used to queue up behind
//     the entire restored session - on a cold file cache, seconds of it.
//   * The restored session then opens its previously-active document first (the
//     one the user was last reading), and the others follow in saved order.
//
// Exactly one document takes the view: the command-line file when there is one
// (the last of them, as when they were opened inline), otherwise the restored
// active document - or, when the session recorded none, the first restored one.
//
// `sessionPaths` should be caller-filtered to files that still exist, but may
// legitimately contain a path that is also on the command line: that document
// opens once (as the command-line entry) and its restore entry becomes a no-op,
// while still reserving its saved tab position. `sessionActive` may be empty.
QList<StagedOpen> planStartupOpens(const QStringList &cliPaths,
                                   const QStringList &sessionPaths,
                                   const QString &sessionActive);

// Where a restored document's tab belongs in a tab bar that may already hold
// others: directly after every document of the saved session that precedes it
// there, and before everything else (so command-line files, which are not in the
// saved order, end up after the restored block).
//
// Derived from the live tab bar on every open instead of being precomputed, which
// is what makes the restored order hold when an open does not produce a tab -
// a corrupt file, a password prompt the user cancelled, or a document that is
// already open. Those simply leave a gap in the saved order rather than shifting
// everything after them.
//
// `currentTabs` are the window's canonical tab paths in order and `savedOrder` the
// session's canonical paths in order. Returns -1 (append) when savedIndex < 0.
int insertIndexForSaved(const QStringList &currentTabs, const QStringList &savedOrder,
                        int savedIndex);

} // namespace mervin
