#include "print/PageRange.h"

#include <QCoreApplication>
#include <QStringList>

namespace PageRange {

namespace {
QString tr(const char *s) { return QCoreApplication::translate("PageRange", s); }
}

QList<int> parse(const QString &spec, int pageCount, QString *error)
{
    auto fail = [&](const QString &msg) -> QList<int> {
        if (error)
            *error = msg;
        return {};
    };
    if (error)
        error->clear();
    pageCount = qMax(1, pageCount);

    const QString trimmed = spec.trimmed();
    const QString hint = tr("Enter pages like 1-3, 5, 8-10.");
    if (trimmed.isEmpty())
        return fail(hint);

    QList<int> pages;
    const QStringList tokens = trimmed.split(QLatin1Char(','), Qt::SkipEmptyParts);
    for (const QString &rawTok : tokens) {
        const QString tok = rawTok.trimmed();
        if (tok.isEmpty())
            continue; // tolerate stray commas: "1-3,,5"

        const int dash = tok.indexOf(QLatin1Char('-'));
        if (dash < 0) {
            bool ok = false;
            const int n = tok.toInt(&ok);
            if (!ok)
                return fail(tr("\"%1\" is not a valid page number.").arg(tok));
            if (n < 1 || n > pageCount)
                return fail(tr("Page %1 is out of range (1-%2).").arg(n).arg(pageCount));
            pages << n;
        } else {
            const QString lhs = tok.left(dash).trimmed();
            const QString rhs = tok.mid(dash + 1).trimmed();
            if (lhs.isEmpty() && rhs.isEmpty()) // a bare "-"
                return fail(tr("\"%1\" is not a valid page range.").arg(tok));

            bool okF = true, okT = true;
            const int from = lhs.isEmpty() ? 1 : lhs.toInt(&okF);
            const int to = rhs.isEmpty() ? pageCount : rhs.toInt(&okT);
            if (!okF || !okT)
                return fail(tr("\"%1\" is not a valid page range.").arg(tok));
            if (from < 1 || from > pageCount || to < 1 || to > pageCount)
                return fail(tr("Range \"%1\" is out of range (1-%2).").arg(tok).arg(pageCount));
            if (from > to)
                return fail(tr("Range \"%1\" is backwards - write it as low-high.").arg(tok));
            for (int p = from; p <= to; ++p)
                pages << p;
        }
    }

    if (pages.isEmpty())
        return fail(hint);
    return pages;
}

QList<int> parseAllowingAll(const QString &spec, int pageCount, QString *error)
{
    const QString trimmed = spec.trimmed();
    if (trimmed.compare(QLatin1String("all"), Qt::CaseInsensitive) != 0)
        return parse(spec, pageCount, error);

    if (error)
        error->clear();
    QList<int> pages;
    for (int p = 1; p <= qMax(1, pageCount); ++p)
        pages << p;
    return pages;
}

} // namespace PageRange
