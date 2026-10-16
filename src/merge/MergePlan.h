#pragma once

#include "print/PageRange.h"
#include "security/PageOps.h"

#include <QCoreApplication>
#include <QList>
#include <QString>
#include <QStringList>

namespace mervin {

// GUI-free ordered file/range segments with page counts, output spans, totals and validation
// errors. MergeDialog displays this model and writes edits back to it.
class MergePlan
{
    Q_DECLARE_TR_FUNCTIONS(mervin::MergePlan)

public:
    enum class Load {
        Ok,        // opened; pageCount is meaningful
        Locked,    // encrypted and we have no password for it
        Unreadable, // missing, damaged, not a PDF
        Reading,    // its background probe has not answered
    };

    /// Retains one input row, its range, load facts and background probe identity.
    struct Entry
    {
        QString path;
        QString spec = PageRange::allKeyword(); // as typed; PageRange::isAll() means every page
        int pageCount = 0;                      // pages in the source; 0 unless load == Ok
        Load load = Load::Ok;
        QString loadError;  // backend message, shown as the row's tooltip
        QString password;   // only ever non-empty once a caller unlocks the row
        bool encrypted = false; // opened, but has a password or owner restrictions
        quint64 probeTicket = 0; // pending duplicates share their probe's identity
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
    // Applies load facts to every pending copy of this ticket without changing
    // ranges or row order. False when every corresponding row was removed.
    bool resolveProbe(quint64 ticket, const Entry &result);
    /// Reports whether any row still awaits its input count and load status.
    bool hasReading() const;

    // Pages row `i` contributes, 0-based, in the order they were typed
    // (duplicates preserved). Empty when the row cannot contribute anything.
    QList<int> pagesFor(int i) const;

    // Why row `i` cannot contribute, as a user-facing sentence, or empty when it
    // is fine.
    QString rowError(int i) const;

    // "12" for a whole file, "4 of 31" for a subset, "Reading…" while its probe
    // runs, "Locked" / "Unreadable" for a row that could not be read, "-" when
    // the range does not parse.
    QString countText(int i) const;
    // The widest texts countText() gives in the UI language ("Locked",
    // "Unreadable", "Reading…", "998 of 999"), for sizing the Count column.
    static QStringList widestCountTexts();

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
    // Names the first Reading row while probes remain, otherwise the first
    // problem, such as "Row 2: Page 40 is out of range (1-31)." Empty when valid.
    QString errorText() const;

    // The plan as the backend wants it. Only meaningful when isValid().
    QList<PageOps::MergeInput> inputs() const;

    // PageOps::merge keeps encryption only when every row names the same file.
    // True when the rows name several files and one that opened is encrypted, so
    // the merged file has no password and none of that file's restrictions.
    bool losesEncryption() const;
    // The password that opens the merged file. When every row names the same file,
    // the merge keeps its encryption and this is the first row's password. Otherwise
    // it is empty.
    QString resultPassword() const;

    // "<dir of row 0>/<base of row 0>-merged.pdf": the Save as default before
    // defaultOutputPath() steps past names in use. Empty when the plan is empty.
    QString plainOutputPath() const;
    // plainOutputPath(), stepping to "-merged-2.pdf" and so on while the name is
    // taken, and never colliding with a file already in the plan. Empty when the
    // plan is empty. Touches the disk.
    QString defaultOutputPath() const;

private:
    QList<Entry> entries_;
};

} // namespace mervin
