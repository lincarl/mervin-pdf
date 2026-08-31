#include "merge/MergePlan.h"

#include "print/PageRange.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QSet>

namespace mervin {

namespace {
QString tr(const char *s, const char *c = nullptr, int n = -1)
{
    return QCoreApplication::translate("MergePlan", s, c, n);
}
} // namespace

void MergePlan::append(const Entry &e)
{
    entries_.append(e);
}

void MergePlan::remove(int i)
{
    if (i >= 0 && i < entries_.size())
        entries_.removeAt(i);
}

int MergePlan::duplicate(int i)
{
    if (i < 0 || i >= entries_.size())
        return -1;
    entries_.insert(i + 1, entries_.at(i));
    return i + 1;
}

int MergePlan::move(int i, int delta)
{
    const int to = i + delta;
    if (i < 0 || i >= entries_.size() || to < 0 || to >= entries_.size() || delta == 0)
        return -1;
    entries_.move(i, to);
    return to;
}

int MergePlan::moveToGap(int from, int gap)
{
    if (from < 0 || from >= entries_.size() || gap < 0 || gap > entries_.size())
        return -1;
    // Gaps are numbered against the list as it stands; lifting the row out closes
    // the gap it occupied, so everything below it shifts up one.
    const int to = gap > from ? gap - 1 : gap;
    if (to == from)
        return -1;
    entries_.move(from, to);
    return to;
}

void MergePlan::setSpec(int i, const QString &spec)
{
    if (i >= 0 && i < entries_.size())
        entries_[i].spec = spec;
}

void MergePlan::setPassword(int i, const QString &password)
{
    if (i >= 0 && i < entries_.size())
        entries_[i].password = password;
}

QList<int> MergePlan::pagesFor(int i) const
{
    if (i < 0 || i >= entries_.size())
        return {};
    const Entry &e = entries_.at(i);
    if (e.load != Load::Ok || e.pageCount <= 0)
        return {};
    const QList<int> oneBased = PageRange::parseAllowingAll(e.spec, e.pageCount, nullptr);
    QList<int> zeroBased;
    zeroBased.reserve(oneBased.size());
    for (int p : oneBased)
        zeroBased.append(p - 1);
    return zeroBased;
}

QString MergePlan::rowError(int i) const
{
    if (i < 0 || i >= entries_.size())
        return {};
    // displayName() is O(rows) and this runs per row per keystroke, so it is
    // only paid on the branches that actually format a name.
    const Entry &e = entries_.at(i);
    switch (e.load) {
    case Load::Locked:
        return tr("\"%1\" is encrypted and cannot be merged.").arg(displayName(i));
    case Load::Unreadable:
        return tr("\"%1\" could not be read.").arg(displayName(i));
    case Load::Ok:
        break;
    }
    if (e.pageCount <= 0)
        return tr("\"%1\" has no pages.").arg(displayName(i));

    QString err;
    const QList<int> pages = PageRange::parseAllowingAll(e.spec, e.pageCount, &err);
    if (pages.isEmpty())
        return err.isEmpty() ? tr("Enter pages like 1-3, 5, 8-10.") : err;
    return {};
}

QString MergePlan::countText(int i) const
{
    if (i < 0 || i >= entries_.size())
        return {};
    const Entry &e = entries_.at(i);
    switch (e.load) {
    case Load::Locked:
        return tr("Locked");
    case Load::Unreadable:
        return tr("Unreadable");
    case Load::Ok:
        break;
    }
    const QList<int> pages = pagesFor(i);
    if (pages.isEmpty())
        return QStringLiteral("-");
    // "All" is the whole file and needs no arithmetic; an explicit range says how
    // much of the file it took, even when that happens to be all of it.
    if (e.spec.trimmed().compare(QLatin1String("all"), Qt::CaseInsensitive) == 0)
        return QString::number(pages.size());
    return tr("%1 of %2").arg(pages.size()).arg(e.pageCount);
}

QList<MergePlan::RowText> MergePlan::rowTexts() const
{
    // One pass for both derived columns. outputText() is a running sum, so
    // asking for it row by row re-derives every row above it; the dialog
    // recomputes the whole table on each keystroke, which made that quadratic.
    QList<RowText> out;
    out.reserve(entries_.size());
    int start = 1;
    bool blocked = false; // a bad row makes every position below it unknowable
    for (int i = 0; i < entries_.size(); ++i) {
        RowText t;
        t.count = countText(i);
        const bool bad = !rowError(i).isEmpty();
        const int n = bad ? 0 : pagesFor(i).size();
        if (!blocked && !bad && n > 0) {
            t.output = n == 1 ? QString::number(start)
                              : QStringLiteral("%1-%2").arg(start).arg(start + n - 1);
            start += n;
        }
        if (bad)
            blocked = true;
        out.append(t);
    }
    return out;
}

QString MergePlan::outputText(int i) const
{
    if (i < 0 || i >= entries_.size())
        return {};
    return rowTexts().at(i).output;
}

QString MergePlan::displayName(int i) const
{
    if (i < 0 || i >= entries_.size())
        return {};
    const QFileInfo fi(entries_.at(i).path);
    const QString name = fi.fileName();
    for (int r = 0; r < entries_.size(); ++r) {
        if (r == i || entries_.at(r).path == entries_.at(i).path)
            continue; // the same file twice legitimately shares a name
        if (QFileInfo(entries_.at(r).path).fileName() == name) {
            const QString folder = fi.dir().dirName();
            return folder.isEmpty() ? name : QStringLiteral("%1 (%2)").arg(name, folder);
        }
    }
    return name;
}

int MergePlan::totalPages() const
{
    int n = 0;
    for (int i = 0; i < entries_.size(); ++i)
        n += pagesFor(i).size();
    return n;
}

bool MergePlan::isValid() const
{
    if (entries_.isEmpty())
        return false;
    for (int i = 0; i < entries_.size(); ++i)
        if (!rowError(i).isEmpty())
            return false;
    return true;
}

QString MergePlan::summaryText() const
{
    if (entries_.isEmpty())
        return tr("Add PDFs to merge.");
    if (!isValid())
        return tr("This merge cannot run yet.");
    QSet<QString> distinct;
    for (const Entry &e : entries_)
        distinct.insert(e.path);
    // Spelled out rather than tr("%n page(s)", ..., n): with no translator loaded
    // - and this project ships no .ts files - Qt's %n substitutes the number but
    // leaves the "(s)" verbatim, so the headline sentence of this dialog would
    // read "13 page(s) from 1 file(s)".
    const int pages = totalPages();
    const QString pagesText = pages == 1 ? tr("1 page") : tr("%1 pages").arg(pages);
    const QString filesText =
        distinct.size() == 1 ? tr("1 file") : tr("%1 files").arg(distinct.size());
    return tr("Result: %1 from %2, in the order shown.").arg(pagesText, filesText);
}

QString MergePlan::errorText() const
{
    for (int i = 0; i < entries_.size(); ++i) {
        const QString e = rowError(i);
        if (!e.isEmpty())
            return tr("Row %1: %2").arg(i + 1).arg(e);
    }
    return {};
}

QList<PageOps::MergeInput> MergePlan::inputs() const
{
    QList<PageOps::MergeInput> out;
    out.reserve(entries_.size());
    for (int i = 0; i < entries_.size(); ++i)
        out.append(PageOps::MergeInput{entries_.at(i).path, pagesFor(i), entries_.at(i).password});
    return out;
}

QString MergePlan::defaultOutputPath() const
{
    if (entries_.isEmpty())
        return {};
    const QFileInfo fi(entries_.first().path);
    const QDir dir = fi.dir();
    const QString base = fi.completeBaseName() + QStringLiteral("-merged");

    QSet<QString> taken;
    for (const Entry &e : entries_)
        taken.insert(QFileInfo(e.path).absoluteFilePath());

    for (int n = 1;; ++n) {
        const QString name = n == 1 ? base + QStringLiteral(".pdf")
                                    : QStringLiteral("%1-%2.pdf").arg(base).arg(n);
        const QString path = dir.filePath(name);
        // Never propose a name that is already one of the inputs: merge reads the
        // sources while it writes, so overwriting one destroys the very file it
        // is copying from.
        if (!QFileInfo::exists(path) && !taken.contains(QFileInfo(path).absoluteFilePath()))
            return path;
    }
}

} // namespace mervin
