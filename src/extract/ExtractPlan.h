#pragma once

#include <QList>
#include <QString>
#include <QStringList>

namespace mervin {

// One output file assembled from ordered page/range rows. ExtractDialog renders this model and
// writes edits back to it. Each row accepts one PageRange token (5, 5-7, 7-, -5, all); the
// dialog splits commas/semicolons via splitPieces. Preserve page order and duplicates.
// API pages are zero-based; row text and UI strings are one-based. Filesystem access is limited
// to output validation and resolving the source path at construction.
class ExtractPlan
{
public:
    ExtractPlan(const QString &sourcePath, int pageCount);

    const QString &sourcePath() const { return sourcePath_; }
    int pageCount() const { return pageCount_; }
    // Re-read every row against a corrected count (qpdf's, once the password is
    // verified). A page past the new end makes its row bad.
    void setPageCount(int pageCount);

    // ---- Rows ---------------------------------------------------------------
    int count() const { return int(rows_.size()); }
    QString spec(int i) const;
    QStringList specs() const;
    void setSpec(int i, const QString &spec);
    void insert(int i, const QString &spec); // `i` clamped to 0..count()
    void append(const QString &spec) { insert(count(), spec); }
    void remove(int i);
    // Both as MergePlan's (see RowMoves.h): the row's new index, or -1 when
    // nothing moved.
    int move(int i, int delta);
    int moveToGap(int from, int gap);
    // A row's text cut at every ',' and ';', for the typing and pasting habit of
    // print dialogs: the first piece stays in the row and each further one becomes
    // a new row below it, trimmed, with empty pieces dropped. A trailing separator
    // after text adds one empty piece, so typing "2," moves on to a fresh row for
    // the next range. Never empty: ("") when nothing is left. "1-3, 5, 8-10" ->
    // ("1-3", "5", "8-10"); "2," -> ("2", ""); ",5" -> ("5"); "," -> ("").
    static QStringList splitPieces(const QString &text);

    // ---- Reading it -----------------------------------------------------------
    QList<int> pagesFor(int i) const; // empty when the row is bad
    // Why row `i` contributes nothing, "" when it is fine: "Enter a page or range,
    // like 5-7." for an empty row, else PageRange's sentence.
    QString rowError(int i) const;
    // True for a row whose text does not parse, as opposed to an empty one: the
    // row the dialog outlines in red and the strip shows as a Bad cell.
    bool isBadText(int i) const;
    // The Count and Output columns for every row, in one pass: "3" and "1-3" (or
    // "4" for one page). A bad row counts "-", and its Output and every Output
    // below it are empty, since those positions are not knowable (Merge's rule).
    struct RowText
    {
        QString count;
        QString output;
    };
    QList<RowText> rowTexts() const;
    int pageTotal() const;
    bool isValid() const; // at least one row and no row error
    // "" when invalid, else "Result: 1 page." / "Result: 4 pages, in the order
    // shown."
    QString summaryText() const;
    // The first problem, "" when valid: "Add a range of pages to extract." with no
    // rows, else the first rowError(), prefixed "Row 2: " when there are 2+ rows.
    QString errorText() const;

    // ---- The strip, fitted to a width ---------------------------------------
    struct Metrics
    {
        int lead = 6;   // before the first cell (and kept free after the last)
        int page = 101; // a page or bad cell: thumbnail width + 12
        int fold = 52;  // a fold tile cell
    };
    // A run of 4+ consecutive ascending pages inside one row, by source pages, so
    // an expanded run stays expanded across edits elsewhere and across moves.
    struct RunKey
    {
        int firstPage = 0;
        int lastPage = 0;
        int occurrence = 0; // earlier runs with these pages (two rows of "1-10")
        bool operator==(const RunKey &) const = default;
    };
    struct Cell
    {
        enum class Kind { Page, Fold, Bad };
        Kind kind = Kind::Page;
        int row = 0;       // the row it comes from
        int first = 0;     // which of pagesFor(row) it stands for; equal unless Fold
        int last = 0;      // (0 and 0 for a Bad cell)
        int firstFlat = 0; // the same, counted across the whole strip, a Bad cell
        int lastFlat = 0;  // counting as one: a cell's identity across refolds
        int page = -1;     // Page: source page. Fold: first hidden page. Bad: -1
        int lastPage = -1; // Fold: last hidden page
        int position = 0;  // Page: 1-based position in the output; 0 below a row
                           // with an error, where it is not knowable (rowTexts)
        RunKey run;        // Fold: the whole run it belongs to (what "expand" records)
        int gapBefore = 0; // the lead, on the first cell only; included in width
        int width = 0;
    };
    // Every page as a cell, and each bad row as one Bad cell (an empty row has
    // none). Then, while sum(width) + m.lead > viewportWidth, take the longest run
    // not yet folded and not in `expanded` (leftmost on a tie) and fold it to its
    // first page, one Fold cell for everything in between, and its last page. The
    // fold that makes the strip fit gives any room it left back as more leading
    // pages, while still hiding at least two. When nothing is left to fold, the
    // strip scrolls. Pure: geometry is the model's, so the rule is unit-tested.
    QList<Cell> cells(const Metrics &m, int viewportWidth, const QList<RunKey> &expanded) const;
    // True while `run` is one of the runs cells() can fold. The dialog forgets an
    // expansion once its run is edited away, so typing it again starts folded.
    bool hasRun(const RunKey &run) const;
    // The text under a cell: the source page ("12"), a fold's hidden range
    // ("14-30"), or a bad row's text.
    QString caption(const Cell &cell) const;
    // Tooltip text: "Page 12 becomes page 4 of the extract." ("Page 12" when its
    // position is not knowable), "Pages 14-30 (17 pages). Click to show them."
    // (Fold), or the bad row's error.
    QString describe(const Cell &cell) const;
    // Remove pagesFor(row)[first..last] (one cell, or a fold's hidden span). The
    // row splits into up to two rows around them ("5-7" minus 6 becomes "5" and
    // "7"); a row left empty, and a bad row, is removed. Returns the row now
    // holding what followed the removed pages (clamped to the last row), or -1
    // when there are no rows left or the arguments are out of range (then nothing
    // changes).
    int removeFromRow(int row, int first, int last);

    // ---- Output (reads the file system) -----------------------------------------
    struct Job
    {
        QString path;
        QList<int> pages; // 0-based, output order, duplicates kept
    };
    // `destination` taken against the source's folder when relative, cleaned.
    QString resolvePath(const QString &destination) const;
    // "<base>-p7.pdf" / "<base>-p12-18.pdf" when the output is one ascending run,
    // otherwise "<base>-extract.pdf".
    QString fileName() const;
    // Save as default: fileName() in the source's folder, stepping to "-2", "-3"...
    // while the name exists or is the source.
    QString defaultOutputPath() const;
    // Files open in the app's tabs. The viewer holds each one open, so writing
    // over one fails on Windows; destinationError() refuses them up front.
    void setOpenFiles(const QStringList &paths);
    // First problem with `destination`, "" when usable. Relative paths are taken
    // against the source's folder. "Choose where to save the extracted pages." /
    // "That folder does not exist." / "That is a folder. Add a file name." / "The
    // extract cannot replace the document it comes from." / "That file is open in
    // a tab. Choose another name."
    QString destinationError(const QString &destination) const;
    // What to write: `destination` resolved with ".pdf" appended if missing, and
    // every page in row order. Only meaningful when isValid() and
    // destinationError(destination) is empty.
    Job job(const QString &destination) const;
    // "Extracted page 7 to annual-report-p7.pdf" / "Extracted 4 pages to
    // annual-report-extract.pdf".
    static QString doneText(const Job &job);

private:
    struct Row
    {
        QString spec;
        QList<int> pages; // 0-based; empty when error is set
        QString error;
    };
    Row parsed(const QString &spec) const;
    struct Run
    {
        int row = 0;
        int first = 0; // index into the row's pages of the run's first page...
        int last = 0;  // ...and of its last
        RunKey key;
        bool folded = false; // working state for cells()...
        int keep = 1;        // ...and the leading pages a folded run still shows
    };
    QList<Run> foldableRuns() const; // every run of 4+ pages, in output order

    QString sourcePath_;
    QString sourceKey_; // fileKey(sourcePath_), once: it opens the file on Windows
    QStringList openKeys_; // fileKey() of every file open in a tab
    int pageCount_ = 0;
    QList<Row> rows_;
};

} // namespace mervin
