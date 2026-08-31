#pragma once

#include "security/PageOps.h"

#include <QList>
#include <QString>

namespace mervin {

// The ordered list of (file, page range) segments a merge will write, plus every
// number the merge dialog puts on screen: each row's page count, the range it
// occupies in the finished document, the running total, and the first thing
// wrong with the plan.
//
// It is deliberately GUI-free and lives in mervin_core so the ordering and
// validation rules can be unit-tested without QtWidgets (see
// tests/tst_merge_plan.cpp) - MergeDialog is then only a rendering of this.
// Nothing here touches the file system except defaultOutputPath(); the caller
// probes each file (PageOps::probe) and reports what it found through Entry.
class MergePlan
{
public:
    enum class Load {
        Ok,        // opened; pageCount is meaningful
        Locked,    // encrypted and we have no password for it
        Unreadable // missing, damaged, not a PDF
    };

    struct Entry
    {
        QString path;
        QString spec = QStringLiteral("All"); // as typed; "all" means every page
        int pageCount = 0;                    // pages in the source; 0 unless load == Ok
        Load load = Load::Ok;
        QString loadError;  // backend message, shown as the row's tooltip
        QString password;   // only ever non-empty once a caller unlocks the row
    };

    int count() const { return entries_.size(); }
    bool isEmpty() const { return entries_.isEmpty(); }
    const Entry &at(int i) const { return entries_.at(i); }
    const QList<Entry> &entries() const { return entries_; }

    void append(const Entry &e);
    void remove(int i);
    // Insert a copy of row `i` directly below it. Returns the new row's index,
    // or -1 if `i` is out of range.
    int duplicate(int i);
    // Move row `i` by `delta` slots. Returns the row's new index, or -1 when the
    // move would leave the list (so a caller can treat the boundary as a no-op).
    int move(int i, int delta);
    // Move row `from` to the insertion point `gap`, where gap 0 is above the
    // first row and gap count() is below the last - the positions a drag hovers
    // between. Counted before the row is lifted out, so every gap below `from`
    // shifts up by one on the way; dropping a row immediately above or below
    // itself is therefore a no-op. Returns the row's new index, or -1 if nothing
    // moved.
    int moveToGap(int from, int gap);
    void setSpec(int i, const QString &spec);
    void setPassword(int i, const QString &password);

    // Pages row `i` contributes, 0-based, in the order they were typed
    // (duplicates preserved). Empty when the row cannot contribute anything.
    QList<int> pagesFor(int i) const;

    // Why row `i` cannot contribute, as a user-facing sentence, or empty when it
    // is fine.
    QString rowError(int i) const;

    // "12" for a whole file, "4 of 31" for a subset, "Locked" / "Unreadable" for
    // a row that could not be read, "-" when the range does not parse.
    QString countText(int i) const;

    // Where row `i` lands in the finished document: "13-16", or "25" for a single
    // page. Empty when this row or any row above it is invalid, because then the
    // position is not knowable.
    QString outputText(int i) const;

    // Both derived columns for every row, in one pass. outputText() is a running
    // sum, so asking row by row is quadratic; the dialog redraws the whole table
    // on every keystroke and uses this instead.
    struct RowText
    {
        QString count;
        QString output;
    };
    QList<RowText> rowTexts() const;

    // The row's file name, with its parent folder appended when a *different*
    // file in the plan has the same name (two rows on the same path share a name
    // legitimately and are left alone).
    QString displayName(int i) const;

    int totalPages() const;
    bool isValid() const; // non-empty and every row contributes
    // "Result: 25 pages from 4 files, in the order shown."
    QString summaryText() const;
    // "Row 2: Page 40 is out of range (1-31)." - the first problem only. Empty
    // when the plan is valid.
    QString errorText() const;

    // The plan as the backend wants it. Only meaningful when isValid().
    QList<PageOps::MergeInput> inputs() const;

    // "<dir of row 0>/<base of row 0>-merged.pdf", stepping to "-merged-2.pdf"
    // and so on while the name is taken, and never colliding with a file already
    // in the plan. Empty when the plan is empty.
    QString defaultOutputPath() const;

private:
    QList<Entry> entries_;
};

} // namespace mervin
