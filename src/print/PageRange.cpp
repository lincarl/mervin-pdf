#include "print/PageRange.h"

#include <QCoreApplication>
#include <QStringList>

// A namespace has no tr(), so the strings go through QCoreApplication::translate
// with the context written out. lupdate files them under the same "PageRange".
namespace PageRange {

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
    const QString hint = QCoreApplication::translate("PageRange", "Enter pages like 1-3, 5, 8-10.");
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
            if (!ok) {
                //: %1 is what the user typed between two commas.
                return fail(QCoreApplication::translate("PageRange",
                                                        "\"%1\" is not a valid page number.")
                                .arg(tok));
            }
            if (n < 1 || n > pageCount) {
                //: %1 is the page number typed, %2 the document's last page.
                return fail(QCoreApplication::translate("PageRange",
                                                        "Page %1 is out of range (1-%2).")
                                .arg(n)
                                .arg(pageCount));
            }
            pages << n;
        } else {
            const auto notARange = [&tok] {
                //: %1 is what the user typed between two commas, like "4-x".
                return QCoreApplication::translate("PageRange", "\"%1\" is not a valid page range.")
                    .arg(tok);
            };
            const QString lhs = tok.left(dash).trimmed();
            const QString rhs = tok.mid(dash + 1).trimmed();
            if (lhs.isEmpty() && rhs.isEmpty()) // a bare "-"
                return fail(notARange());

            bool okF = true, okT = true;
            const int from = lhs.isEmpty() ? 1 : lhs.toInt(&okF);
            const int to = rhs.isEmpty() ? pageCount : rhs.toInt(&okT);
            if (!okF || !okT)
                return fail(notARange());
            if (from < 1 || from > pageCount || to < 1 || to > pageCount) {
                //: %1 is the range typed, like "30-40", %2 the document's last page.
                return fail(QCoreApplication::translate("PageRange",
                                                        "Range \"%1\" is out of range (1-%2).")
                                .arg(tok)
                                .arg(pageCount));
            }
            if (from > to) {
                //: %1 is the range typed, like "12-1".
                return fail(QCoreApplication::translate(
                                "PageRange", "Range \"%1\" is backwards - write it as low-high.")
                                .arg(tok));
            }
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
    if (!isAll(spec))
        return parse(spec, pageCount, error);

    if (error)
        error->clear();
    QList<int> pages;
    for (int p = 1; p <= qMax(1, pageCount); ++p)
        pages << p;
    return pages;
}

QString allKeyword()
{
    //: Typed in a page range field to mean every page, and a merge row's
    //: default range. One short word without commas or semicolons.
    return QCoreApplication::translate("PageRange", "All");
}

bool isAll(const QString &spec)
{
    const QString trimmed = spec.trimmed();
    return trimmed.compare(QLatin1String("all"), Qt::CaseInsensitive) == 0
           || trimmed.compare(allKeyword(), Qt::CaseInsensitive) == 0;
}

} // namespace PageRange
