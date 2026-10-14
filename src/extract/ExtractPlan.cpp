#include "extract/ExtractPlan.h"

#include "merge/RowMoves.h"
#include "print/PageRange.h"
#include "recent/PathKey.h"

#include <QDir>
#include <QFileInfo>
#include <QRegularExpression>

#include <algorithm>

namespace mervin {

namespace {
// "a-b", or "a" for one page: 1-based text for a run of 0-based pages.
QString rangeText(int first, int last)
{
    return first == last ? QString::number(first + 1)
                         : QStringLiteral("%1-%2").arg(first + 1).arg(last + 1);
}

// Index of the last page of the ascending run that starts at pages[i].
int runEnd(const QList<int> &pages, int i)
{
    while (i + 1 < pages.size() && pages.at(i + 1) == pages.at(i) + 1)
        ++i;
    return i;
}

// Pages as row texts, one per maximal ascending run: {4, 5, 6, 11} -> "5-7", "12".
QStringList runTexts(const QList<int> &pages)
{
    QStringList out;
    for (int i = 0; i < pages.size(); ++i) {
        const int j = runEnd(pages, i);
        out << rangeText(pages.at(i), pages.at(j));
        i = j;
    }
    return out;
}

// A comparison key for "is this the same file": symlinks resolved when the file
// exists, case and separators normalised (PathKey) either way. Unlike fileKey() in
// PathKey.h, the folder of a missing file is not resolved.
QString pathKey(const QString &path)
{
    const QFileInfo fi(path);
    const QString canonical = fi.canonicalFilePath();
    return normalizePathKey(canonical.isEmpty() ? fi.absoluteFilePath() : canonical);
}

QString withPdfSuffix(const QString &path)
{
    return path.endsWith(QStringLiteral(".pdf"), Qt::CaseInsensitive)
               ? path
               : path + QStringLiteral(".pdf");
}

// Delete mode's result line names at most this many runs of remaining pages, then "…".
constexpr int kKeptRuns = 8;
} // namespace

ExtractPlan::ExtractPlan(const QString &sourcePath, int pageCount, Mode mode,
                         const QString &canonicalPath)
    : mode_(mode)
    , sourcePath_(sourcePath)
    , sourceKey_(normalizePathKey(canonicalPath.isEmpty() ? QFileInfo(sourcePath).absoluteFilePath()
                                                          : canonicalPath))
    , pageCount_(pageCount)
{
}

void ExtractPlan::setPageCount(int pageCount)
{
    pageCount_ = pageCount;
    for (Row &row : rows_)
        row = parsed(row.spec);
}

ExtractPlan::Row ExtractPlan::parsed(const QString &spec) const
{
    Row row{spec, {}, {}};
    if (spec.trimmed().isEmpty()) {
        row.error = tr("Enter a page or range, like 5-7.");
        return row;
    }
    for (int p : PageRange::parseAllowingAll(spec, pageCount_, &row.error))
        row.pages << p - 1;
    return row;
}

// ---- Rows -------------------------------------------------------------------

QString ExtractPlan::spec(int i) const
{
    return i >= 0 && i < count() ? rows_.at(i).spec : QString();
}

QStringList ExtractPlan::specs() const
{
    QStringList out;
    for (const Row &row : rows_)
        out << row.spec;
    return out;
}

void ExtractPlan::setSpec(int i, const QString &spec)
{
    if (i >= 0 && i < count())
        rows_[i] = parsed(spec);
}

void ExtractPlan::insert(int i, const QString &spec)
{
    rows_.insert(qBound(0, i, count()), parsed(spec));
}

void ExtractPlan::remove(int i)
{
    if (i >= 0 && i < count())
        rows_.removeAt(i);
}

int ExtractPlan::move(int i, int delta)
{
    return moveRow(rows_, i, delta);
}

int ExtractPlan::moveToGap(int from, int gap)
{
    return moveRowToGap(rows_, from, gap);
}

QStringList ExtractPlan::splitPieces(const QString &text)
{
    static const QRegularExpression separator(QStringLiteral("[,;]"));
    QStringList out;
    for (const QString &piece : text.split(separator)) {
        if (!piece.trimmed().isEmpty())
            out << piece.trimmed();
    }
    // Typing "2," leaves nothing after the separator yet: give the next range a
    // row to go in. A separator with nothing before it is simply dropped.
    const bool trailing = text.trimmed().endsWith(QLatin1Char(','))
                          || text.trimmed().endsWith(QLatin1Char(';'));
    if (out.isEmpty() || trailing)
        out << QString();
    return out;
}

// ---- Reading it -------------------------------------------------------------

QList<int> ExtractPlan::pagesFor(int i) const
{
    return i >= 0 && i < count() ? rows_.at(i).pages : QList<int>();
}

QString ExtractPlan::rowError(int i) const
{
    return i >= 0 && i < count() ? rows_.at(i).error : QString();
}

bool ExtractPlan::isBadText(int i) const
{
    return !rowError(i).isEmpty() && !rows_.at(i).spec.trimmed().isEmpty();
}

QList<ExtractPlan::RowText> ExtractPlan::rowTexts() const
{
    QList<RowText> out;
    int start = 1;
    bool blocked = false; // a bad row makes every position below it unknowable
    for (const Row &row : rows_) {
        RowText t;
        blocked = blocked || !row.error.isEmpty();
        const int n = int(row.pages.size());
        t.count = row.error.isEmpty() ? QString::number(n) : QStringLiteral("-");
        if (!blocked) {
            t.output = n == 1 ? QString::number(start)
                              : QStringLiteral("%1-%2").arg(start).arg(start + n - 1);
            start += n;
        }
        out << t;
    }
    return out;
}

int ExtractPlan::pageTotal() const
{
    int n = 0;
    for (const Row &row : rows_)
        n += int(row.pages.size());
    return n;
}

bool ExtractPlan::isValid() const
{
    return !rows_.isEmpty() && errorText().isEmpty();
}

QString ExtractPlan::summaryText() const
{
    // Empty while invalid: the error line says why, so a second sentence would
    // only repeat it.
    if (!isValid())
        return {};
    if (mode_ == Mode::Delete) {
        const QList<int> deleted = deletedPages();
        QList<int> kept;
        for (int p = 0, d = 0; p < pageCount_; ++p) {
            if (d < deleted.size() && deleted.at(d) == p)
                ++d;
            else
                kept << p;
        }
        QStringList runs = runTexts(kept);
        if (runs.size() > kKeptRuns) {
            runs.resize(kKeptRuns);
            runs << QStringLiteral("…");
        }
        //: Result line of Delete pages, under the page strip. %n is how many pages
        //: the new file keeps, %1 the document's page count, %2 the pages that
        //: remain as ranges, like "1, 3-4". In English the verb follows %n:
        //: "1 of 6 pages remains", "3 of 6 pages remain".
        return tr("Result: %n of %1 page(s) remain (%2).", nullptr, int(kept.size()))
            .arg(QString::number(pageCount_), runs.join(QStringLiteral(", ")));
    }
    const int pages = pageTotal();
    // One page has no order to speak of, so it gets its own sentence. %n still
    // carries the rest: some languages have more than one plural form.
    if (pages == 1)
        return tr("Result: 1 page.");
    return tr("Result: %n page(s), in the order shown.", nullptr, pages);
}

QString ExtractPlan::errorText() const
{
    if (rows_.isEmpty()) {
        if (mode_ == Mode::Delete) {
            //: Delete pages with no rows left. Clicking a page in the strip below
            //: marks it for deletion; "add a range" refers to the Add Range button.
            return tr("Click the pages to delete, or add a range.");
        }
        return tr("Add a range of pages to extract.");
    }
    for (int i = 0; i < count(); ++i) {
        const QString &e = rows_.at(i).error;
        if (e.isEmpty())
            continue;
        //: %1 is the row number, %2 the sentence saying what is wrong with it.
        return count() > 1 ? tr("Row %1: %2").arg(QString::number(i + 1), e) : e;
    }
    // PageOps::deletePages() refuses this too, so its sentence is reused and
    // translated once.
    if (mode_ == Mode::Delete && deletedPages().size() >= pageCount_)
        return QCoreApplication::translate("mervin::PageOps", "Cannot delete every page.");
    return {};
}

// ---- Delete mode ------------------------------------------------------------

QList<int> ExtractPlan::deletedPages() const
{
    QList<int> out;
    for (const Row &row : rows_)
        out << row.pages;
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

int ExtractPlan::deletePage(int page)
{
    if (page < 0 || page >= pageCount_ || deletedPages().contains(page))
        return -1;
    // An empty row, left by Add Range or a trailing comma, waits for the next
    // page. Filling it first means a click never leaves it blocking the window.
    for (int r = 0; r < count(); ++r) {
        if (rows_.at(r).spec.trimmed().isEmpty()) {
            rows_[r] = parsed(QString::number(page + 1));
            return r;
        }
    }
    // Rows that are one ascending run and end just before the page or start just
    // after it. The page joins them rather than becoming a row of its own.
    int before = -1;
    int after = -1;
    for (int r = 0; r < count(); ++r) {
        const QList<int> &pages = rows_.at(r).pages;
        if (pages.isEmpty() || runEnd(pages, 0) != pages.size() - 1)
            continue;
        if (before < 0 && pages.last() == page - 1)
            before = r;
        if (after < 0 && pages.first() == page + 1)
            after = r;
    }
    if (before >= 0 && after >= 0) {
        // The page closes the gap between two rows, so "2" and "4" become "2-4".
        rows_[before] = parsed(rangeText(rows_.at(before).pages.first(), rows_.at(after).pages.last()));
        rows_.removeAt(after);
        return after < before ? before - 1 : before;
    }
    if (before >= 0 || after >= 0) {
        const int r = before >= 0 ? before : after;
        const QList<int> &pages = rows_.at(r).pages;
        rows_[r] = parsed(rangeText(qMin(pages.first(), page), qMax(pages.last(), page)));
        return r;
    }
    append(QString::number(page + 1));
    return count() - 1;
}

int ExtractPlan::keepPage(int page)
{
    // Bottom up, so a row's split never moves a row still to be visited. The top
    // row naming the page, visited last, decides which row is current.
    int current = -1;
    for (int r = count() - 1; r >= 0; --r) {
        const QList<int> pages = rows_.at(r).pages;
        const int at = int(pages.indexOf(page));
        if (at < 0)
            continue;
        QList<int> rest = pages;
        rest.removeAll(page);
        rows_.removeAt(r);
        int i = r;
        for (const QString &piece : runTexts(rest))
            rows_.insert(i++, parsed(piece));
        current = r + int(runTexts(pages.mid(0, at)).size());
    }
    return current < 0 ? -1 : qMin(current, count() - 1);
}

// ---- The strip --------------------------------------------------------------

QList<ExtractPlan::Run> ExtractPlan::foldableRuns() const
{
    QList<Run> runs;
    for (int r = 0; r < count(); ++r) {
        const QList<int> &pages = rows_.at(r).pages;
        for (int i = 0; i < pages.size(); ++i) {
            const int j = runEnd(pages, i);
            if (j - i + 1 >= 4) {
                RunKey key{pages.at(i), pages.at(j)};
                key.occurrence = int(std::count_if(runs.cbegin(), runs.cend(), [&key](const Run &x) {
                    return x.key.firstPage == key.firstPage && x.key.lastPage == key.lastPage;
                }));
                runs << Run{r, i, j, key};
            }
            i = j;
        }
    }
    return runs;
}

bool ExtractPlan::hasRun(const RunKey &run) const
{
    const QList<Run> runs = foldableRuns();
    return std::any_of(runs.begin(), runs.end(), [&run](const Run &r) { return r.key == run; });
}

QList<ExtractPlan::Cell> ExtractPlan::deleteCells(const Metrics &m) const
{
    // The first row naming each page, and the page's index in it.
    QList<int> rowOf(pageCount_, -1);
    QList<int> indexIn(pageCount_, 0);
    for (int r = 0; r < count(); ++r) {
        const QList<int> &pages = rows_.at(r).pages;
        for (int i = 0; i < pages.size(); ++i) {
            if (rowOf.at(pages.at(i)) < 0) {
                rowOf[pages.at(i)] = r;
                indexIn[pages.at(i)] = i;
            }
        }
    }
    QList<Cell> out;
    int position = 0;
    for (int p = 0; p < pageCount_; ++p) {
        Cell c;
        c.row = rowOf.at(p);
        c.first = c.last = indexIn.at(p);
        c.firstFlat = c.lastFlat = p; // the page is the cell's identity
        c.page = p;
        c.deleted = c.row >= 0;
        c.position = c.deleted ? 0 : ++position;
        c.gapBefore = out.isEmpty() ? m.lead : 0;
        c.width = c.gapBefore + m.page;
        out << c;
    }
    return out;
}

QList<ExtractPlan::Cell> ExtractPlan::cells(const Metrics &m, int viewportWidth,
                                            const QList<RunKey> &expanded) const
{
    if (mode_ == Mode::Delete)
        return deleteCells(m);
    // Foldable runs in output order, and the width with nothing folded.
    QList<Run> runs = foldableRuns();
    runs.removeIf([&expanded](const Run &r) { return expanded.contains(r.key); });
    int items = 0;
    for (int r = 0; r < count(); ++r)
        items += isBadText(r) ? 1 : int(rows_.at(r).pages.size());
    int total = 2 * m.lead + items * m.page; // one lead kept free after the last cell

    while (total > viewportWidth) {
        Run *best = nullptr; // longest unfolded run, leftmost on a tie
        for (Run &r : runs)
            if (!r.folded && (!best || r.last - r.first > best->last - best->first))
                best = &r;
        if (!best)
            break; // nothing left to fold: the strip scrolls
        best->folded = true;
        total -= (best->last - best->first - 1) * m.page - m.fold;
        if (total <= viewportWidth) {
            // This fold made it fit: hand the room it left back as leading pages,
            // so "400-420" shows 400-405 [406-419] 420, not 400 [401-419] 420. The
            // fold keeps at least two pages, or it would hide next to nothing.
            const int room = (viewportWidth - total) / m.page;
            best->keep += qMin(room, best->last - best->first - 3);
            total += (best->keep - 1) * m.page;
        }
    }

    QList<Cell> out;
    int flat = 0;         // cells' identity: every page, and each bad row as one
    int position = 0;     // pages before this one in the output
    bool blocked = false; // from the first row with an error down (rowTexts' rule)
    int next = 0;         // the first run that does not end before the current page
    const auto add = [&out, &m](Cell c, int width) {
        c.gapBefore = out.isEmpty() ? m.lead : 0;
        c.width = c.gapBefore + width;
        out << c;
    };
    for (int r = 0; r < count(); ++r) {
        const QList<int> &pages = rows_.at(r).pages;
        blocked = blocked || !rows_.at(r).error.isEmpty();
        if (isBadText(r)) {
            Cell c;
            c.kind = Cell::Kind::Bad;
            c.row = r;
            c.firstFlat = c.lastFlat = flat++;
            add(c, m.page);
            continue;
        }
        for (int i = 0; i < pages.size(); ++i) {
            while (next < runs.size()
                   && (runs.at(next).row < r || (runs.at(next).row == r && runs.at(next).last < i)))
                ++next;
            Cell c;
            c.row = r;
            c.first = c.last = i;
            c.firstFlat = c.lastFlat = flat + i;
            c.page = pages.at(i);
            const bool fold = next < runs.size() && runs.at(next).row == r && runs.at(next).folded
                              && i == runs.at(next).first + runs.at(next).keep;
            if (fold) {
                const Run &run = runs.at(next);
                c.kind = Cell::Kind::Fold;
                c.last = run.last - 1;
                c.lastFlat = flat + c.last;
                c.lastPage = pages.at(c.last);
                c.run = run.key;
                add(c, m.fold);
                i = c.last; // resume at the run's last page
            } else {
                c.kind = Cell::Kind::Page;
                c.position = blocked ? 0 : position + i + 1;
                add(c, m.page);
            }
        }
        flat += int(pages.size());
        position += int(pages.size());
    }
    return out;
}

QString ExtractPlan::caption(const Cell &cell) const
{
    switch (cell.kind) {
    case Cell::Kind::Page:
        return QString::number(cell.page + 1);
    case Cell::Kind::Fold:
        return rangeText(cell.page, cell.lastPage);
    case Cell::Kind::Bad:
        return spec(cell.row).trimmed();
    }
    return {};
}

QString ExtractPlan::describe(const Cell &cell) const
{
    switch (cell.kind) {
    case Cell::Kind::Page:
        if (mode_ == Mode::Delete) {
            if (cell.deleted) {
                //: Tooltip of a page struck out in the Delete pages strip. %1 is its
                //: page number. The original file keeps it; only the new file leaves
                //: it out.
                return tr("Page %1 will be deleted. Click to keep it.").arg(cell.page + 1);
            }
            //: Tooltip of a page in the Delete pages strip. %1 is its page number.
            return tr("Click to delete page %1.").arg(cell.page + 1);
        }
        if (cell.position == 0) {
            //: Tooltip of a page in the extract strip: %1 is its page number.
            return tr("Page %1").arg(cell.page + 1);
        }
        //: %1 is the page number in the source document, %2 its position in the new file.
        return tr("Page %1 becomes page %2 of the extract.").arg(cell.page + 1).arg(cell.position);
    case Cell::Kind::Fold:
        //: Tooltip of a tile standing in for hidden pages: %1 is their range, like "14-30".
        return tr("Pages %1 (%n page(s)). Click to show them.", nullptr,
                  cell.lastPage - cell.page + 1)
            .arg(rangeText(cell.page, cell.lastPage));
    case Cell::Kind::Bad:
        return rowError(cell.row);
    }
    return {};
}

int ExtractPlan::removeFromRow(int row, int first, int last)
{
    if (row < 0 || row >= count())
        return -1;
    const QList<int> pages = rows_.at(row).pages;
    if (!pages.isEmpty() && (first < 0 || last < first || last >= pages.size()))
        return -1;
    // A bad or empty row has no pages: both halves come out empty.
    const QStringList head = runTexts(pages.mid(0, first));
    const QStringList tail = pages.isEmpty() ? QStringList() : runTexts(pages.mid(last + 1));
    rows_.removeAt(row);
    int at = row;
    for (const QString &piece : head + tail)
        rows_.insert(at++, parsed(piece));
    return qMin(row + int(head.size()), count() - 1);
}

// ---- Output -----------------------------------------------------------------

QString ExtractPlan::fileName() const
{
    const QString base = QFileInfo(sourcePath_).completeBaseName();
    if (mode_ == Mode::Delete)
        return QStringLiteral("%1-edited.pdf").arg(base);
    QList<int> pages;
    bool bad = false;
    for (int r = 0; r < count(); ++r) {
        bad = bad || isBadText(r);
        pages << rows_.at(r).pages;
    }
    if (!bad && !pages.isEmpty() && runEnd(pages, 0) == pages.size() - 1)
        return QStringLiteral("%1-p%2.pdf").arg(base, rangeText(pages.first(), pages.last()));
    return QStringLiteral("%1-extract.pdf").arg(base);
}

QString ExtractPlan::resolvePath(const QString &destination) const
{
    const QString d = destination.trimmed();
    if (QDir::isAbsolutePath(d))
        return QDir::cleanPath(d);
    return QDir::cleanPath(QFileInfo(sourcePath_).absoluteDir().absoluteFilePath(d));
}

QString ExtractPlan::plainOutputPath() const
{
    return QDir::cleanPath(QFileInfo(sourcePath_).absoluteDir().filePath(fileName()));
}

QString ExtractPlan::defaultOutputPath() const
{
    const QDir dir = QFileInfo(sourcePath_).absoluteDir();
    const QString name = fileName();
    const QString stem = QFileInfo(name).completeBaseName();
    for (int n = 1;; ++n) {
        const QString path = QDir::cleanPath(
            dir.filePath(n == 1 ? name : QStringLiteral("%1-%2.pdf").arg(stem, QString::number(n))));
        if (!QFileInfo::exists(path) && pathKey(path) != sourceKey_)
            return path;
    }
}

ExtractPlan::Destination ExtractPlan::examine(const QString &destination) const
{
    Destination facts;
    if (destination.trimmed().isEmpty())
        return facts;
    const QString path = resolvePath(destination);
    facts.isFolder = QFileInfo(path).isDir();
    const QString file = withPdfSuffix(path);
    const QFileInfo info(file);
    facts.folderExists = info.absoluteDir().exists();
    facts.exists = info.exists();
    facts.key = pathKey(file);
    return facts;
}

QString ExtractPlan::destinationError(const QString &destination, const Destination &facts) const
{
    const QString d = destination.trimmed();
    if (d.isEmpty()) {
        if (mode_ == Mode::Delete) {
            //: Delete pages with an empty Save as field. The edited copy is the new
            //: file without the deleted pages.
            return tr("Choose where to save the edited copy.");
        }
        return tr("Choose where to save the extracted pages.");
    }
    // A file name is needed: resolvePath() drops a trailing separator, so a folder
    // would otherwise become "<folder>.pdf" beside it.
    if (d.endsWith(QLatin1Char('/')) || d.endsWith(QLatin1Char('\\')) || facts.isFolder)
        return facts.isFolder ? tr("That is a folder. Add a file name.")
                              : tr("That folder does not exist.");
    if (!facts.folderExists)
        return tr("That folder does not exist.");
    // qpdf reads the source while it writes, so replacing it destroys the very
    // file being copied from.
    const QString &key = facts.key;
    if (key == sourceKey_) {
        if (mode_ == Mode::Delete) {
            //: Delete pages always writes a new file and never changes the original.
            return tr("The edited copy cannot replace the document it comes from.");
        }
        return tr("The extract cannot replace the document it comes from.");
    }
    // The viewer keeps an open tab's file open, so the write would fail only
    // after the dialog had closed and taken the rows with it.
    if (openKeys_.contains(key))
        return tr("That file is open in a tab. Choose another name.");
    return {};
}

void ExtractPlan::setOpenFiles(const QStringList &paths)
{
    openKeys_.clear();
    for (const QString &p : paths)
        openKeys_ << normalizePathKey(p);
}

ExtractPlan::Job ExtractPlan::job(const QString &destination) const
{
    Job job{withPdfSuffix(resolvePath(destination)), {}, mode_};
    if (mode_ == Mode::Delete) {
        job.pages = deletedPages();
        return job;
    }
    for (const Row &row : rows_)
        job.pages << row.pages;
    return job;
}

QString ExtractPlan::doneText(const Job &job)
{
    const QString name = QFileInfo(job.path).fileName();
    if (job.mode == Mode::Delete) {
        //: Status bar message after Delete pages wrote its new file. %n is how many
        //: pages it left out, %1 the new file's name. The original is unchanged.
        return tr("Deleted %n page(s) in a copy saved as %1", nullptr, int(job.pages.size()))
            .arg(name);
    }
    // One page is named by its number, so it gets its own sentence.
    if (job.pages.size() == 1) {
        //: %1 is the page number, %2 the new file's name.
        return tr("Extracted page %1 to %2").arg(QString::number(job.pages.first() + 1), name);
    }
    //: %1 is the new file's name.
    return tr("Extracted %n page(s) to %1", nullptr, int(job.pages.size())).arg(name);
}

} // namespace mervin
