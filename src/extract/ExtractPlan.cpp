#include "extract/ExtractPlan.h"

#include "merge/RowMoves.h"
#include "print/PageRange.h"
#include "recent/PathKey.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QRegularExpression>

#include <algorithm>

namespace mervin {

namespace {
QString tr(const char *s)
{
    return QCoreApplication::translate("ExtractPlan", s);
}

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
// exists, case and separators normalised (PathKey) either way.
QString fileKey(const QString &path)
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
} // namespace

ExtractPlan::ExtractPlan(const QString &sourcePath, int pageCount)
    : sourcePath_(sourcePath)
    , sourceKey_(fileKey(sourcePath))
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
    // only repeat it. Plurals are spelled out for the reason given in
    // MergePlan::summaryText.
    if (!isValid())
        return {};
    const int pages = pageTotal();
    return pages == 1 ? tr("Result: 1 page.")
                      : tr("Result: %1 pages, in the order shown.").arg(pages);
}

QString ExtractPlan::errorText() const
{
    if (rows_.isEmpty())
        return tr("Add a range of pages to extract.");
    for (int i = 0; i < count(); ++i) {
        const QString &e = rows_.at(i).error;
        if (!e.isEmpty())
            return count() > 1 ? tr("Row %1: %2").arg(QString::number(i + 1), e) : e;
    }
    return {};
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

QList<ExtractPlan::Cell> ExtractPlan::cells(const Metrics &m, int viewportWidth,
                                            const QList<RunKey> &expanded) const
{
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
        if (cell.position == 0)
            return tr("Page %1").arg(cell.page + 1);
        return tr("Page %1 becomes page %2 of the extract.").arg(cell.page + 1).arg(cell.position);
    case Cell::Kind::Fold:
        return tr("Pages %1 (%2 pages). Click to show them.")
            .arg(rangeText(cell.page, cell.lastPage))
            .arg(cell.lastPage - cell.page + 1);
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

QString ExtractPlan::defaultOutputPath() const
{
    const QDir dir = QFileInfo(sourcePath_).absoluteDir();
    const QString name = fileName();
    const QString stem = QFileInfo(name).completeBaseName();
    for (int n = 1;; ++n) {
        const QString path = QDir::cleanPath(
            dir.filePath(n == 1 ? name : QStringLiteral("%1-%2.pdf").arg(stem, QString::number(n))));
        if (!QFileInfo::exists(path) && fileKey(path) != sourceKey_)
            return path;
    }
}

QString ExtractPlan::destinationError(const QString &destination) const
{
    const QString d = destination.trimmed();
    if (d.isEmpty())
        return tr("Choose where to save the extracted pages.");
    const QString path = resolvePath(destination);
    // A file name is needed: resolvePath() drops a trailing separator, so a folder
    // would otherwise become "<folder>.pdf" beside it.
    if (d.endsWith(QLatin1Char('/')) || d.endsWith(QLatin1Char('\\')) || QFileInfo(path).isDir())
        return QFileInfo(path).isDir() ? tr("That is a folder. Add a file name.")
                                       : tr("That folder does not exist.");
    const QString file = withPdfSuffix(path);
    if (!QFileInfo(file).absoluteDir().exists())
        return tr("That folder does not exist.");
    // qpdf reads the source while it writes, so replacing it destroys the very
    // file being copied from.
    if (fileKey(file) == sourceKey_)
        return tr("The extract cannot replace the document it comes from.");
    return {};
}

ExtractPlan::Job ExtractPlan::job(const QString &destination) const
{
    Job job{withPdfSuffix(resolvePath(destination)), {}};
    for (const Row &row : rows_)
        job.pages << row.pages;
    return job;
}

QString ExtractPlan::doneText(const Job &job)
{
    const QString name = QFileInfo(job.path).fileName();
    if (job.pages.size() == 1)
        return tr("Extracted page %1 to %2").arg(QString::number(job.pages.first() + 1), name);
    return tr("Extracted %1 pages to %2").arg(QString::number(job.pages.size()), name);
}

} // namespace mervin
