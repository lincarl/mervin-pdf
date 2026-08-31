#include "render/TextIndex.h"

#include "render/Document.h"

#include <mupdf/fitz.h>

#include <QRegularExpression>

#include <algorithm>
#include <cstring>
#include <limits>
#include <mutex>

namespace mervin {

namespace {

bool isWordChar(QChar c)
{
    return c.isLetterOrNumber() || c == QLatin1Char('_');
}

bool isTrailingUrlPunctuation(QChar c)
{
    return c == QLatin1Char('.') || c == QLatin1Char(',') || c == QLatin1Char(';')
           || c == QLatin1Char(':') || c == QLatin1Char('!') || c == QLatin1Char('?');
}

QString normalizedWebUrl(const QString &raw)
{
    QString url = raw.trimmed();
    if (url.startsWith(QStringLiteral("www."), Qt::CaseInsensitive))
        url.prepend(QStringLiteral("https://"));
    return url;
}

} // namespace

TextIndex::TextIndex(fz_context *baseCtx, Document *doc)
    : doc_(doc)
{
    // Clone the base context so text extraction runs on its own context,
    // independent of the render workers (MuPDF's documented multi-thread model;
    // the shared store is protected by the base context's lock callbacks).
    if (baseCtx)
        ctx_ = fz_clone_context(baseCtx);
    pages_.resize(doc_ ? doc_->pageCount() : 0);
}

TextIndex::~TextIndex()
{
    if (ctx_)
        fz_drop_context(ctx_);
}

const TextIndex::PageText &TextIndex::ensure(int pageNo)
{
    static PageText empty;
    if (pageNo < 0 || pageNo >= static_cast<int>(pages_.size()))
        return empty;

    PageText &pt = pages_[pageNo];
    if (pt.ready)
        return pt;
    pt.ready = true;
    if (!ctx_ || !doc_)
        return pt;

    fz_page *page = nullptr;
    fz_stext_page *stext = nullptr;
    fz_var(page);
    fz_var(stext);
    // Text extraction loads the page and parses its content stream, both of
    // which touch the document's (non-thread-safe) object cache. Serialize
    // against the render workers via the document's access lock. Iterating the
    // resulting stext is in-memory and could run unlocked, but it is cheap, so
    // we hold the lock for the whole block for simplicity.
    std::lock_guard<std::mutex> docLk(doc_->accessMutex());
    fz_try(ctx_) {
        page = fz_load_page(ctx_, doc_->handle(), pageNo);

        // Page bounds may not start at the origin; the renderer shifts the
        // pixmap so its top-left is the transformed bound's min. Express text
        // coordinates relative to that same origin so the viewer's transform
        // (which assumes a (0,0)-based page) lines up exactly.
        const fz_rect bound = fz_bound_page(ctx_, page);
        const float ox = bound.x0;
        const float oy = bound.y0;

        fz_stext_options opts;
        std::memset(&opts, 0, sizeof(opts)); // default flags: ligatures expanded, whitespace normalised
        stext = fz_new_stext_page_from_page(ctx_, page, &opts);

        for (fz_stext_block *block = stext->first_block; block; block = block->next) {
            if (block->type != FZ_STEXT_BLOCK_TEXT)
                continue;
            for (fz_stext_line *line = block->u.t.first_line; line; line = line->next) {
                const int lineStart = static_cast<int>(pt.text.size());
                for (fz_stext_char *ch = line->first_char; ch; ch = ch->next) {
                    int c = ch->c;
                    if (c < 0x20 && c != 0x09)
                        continue; // drop control characters
                    if (c == 0x09)
                        c = ' '; // normalise tabs to spaces

                    const fz_rect r = fz_rect_from_quad(ch->quad);
                    const QRectF box(r.x0 - ox, r.y0 - oy, r.x1 - r.x0, r.y1 - r.y0);

                    if (QChar::requiresSurrogates(c)) {
                        pt.text.append(QChar(QChar::highSurrogate(c)));
                        pt.text.append(QChar(QChar::lowSurrogate(c)));
                        pt.rects.push_back(box);
                        pt.rects.push_back(box);
                    } else {
                        pt.text.append(QChar(c));
                        pt.rects.push_back(box);
                    }
                }
                const int lineEnd = static_cast<int>(pt.text.size());
                if (lineEnd > lineStart) {
                    const fz_rect lb = line->bbox;
                    pt.lines.push_back(
                        LineSpan{lineStart, lineEnd,
                                 QRectF(lb.x0 - ox, lb.y0 - oy, lb.x1 - lb.x0, lb.y1 - lb.y0)});
                    pt.text.append(QLatin1Char('\n')); // line separator
                    pt.rects.push_back(QRectF());      // null rect for the separator
                }
            }
        }
    }
    fz_always(ctx_) {
        if (stext)
            fz_drop_stext_page(ctx_, stext);
        if (page)
            fz_drop_page(ctx_, page);
    }
    fz_catch(ctx_) {
        // Keep whatever was extracted; pt stays marked ready so we don't retry.
    }
    return pt;
}

const QString &TextIndex::pageText(int pageNo)
{
    return ensure(pageNo).text;
}

int TextIndex::pageTextLength(int pageNo)
{
    return static_cast<int>(ensure(pageNo).text.size());
}

int TextIndex::offsetAt(int pageNo, QPointF p)
{
    const PageText &pt = ensure(pageNo);
    if (pt.lines.empty())
        return 0;

    // Choose the line nearest the point. Tables and other multi-column layouts
    // give each cell its own line, and adjacent rows overlap vertically, so
    // several lines can share the click's vertical band. Pick by band first (a
    // line that vertically contains the click beats one that doesn't), then
    // break ties by horizontal distance, so the click lands in the column
    // actually under the cursor rather than whichever cell comes first in
    // reading order. Choosing on y alone snaps to a far-away cell and makes the
    // clicked text impossible to select (a horizontal drag never moves the caret
    // off that distant line).
    int bestLine = -1;
    bool bestInside = false;
    double bestDy = std::numeric_limits<double>::max();
    double bestDx = std::numeric_limits<double>::max();
    for (int li = 0; li < static_cast<int>(pt.lines.size()); ++li) {
        const QRectF &b = pt.lines[li].bbox;
        const bool inside = p.y() >= b.top() && p.y() <= b.bottom();
        const double dy = inside ? 0.0 : (p.y() < b.top() ? b.top() - p.y() : p.y() - b.bottom());
        double dx = 0.0;
        if (p.x() < b.left())
            dx = b.left() - p.x();
        else if (p.x() > b.right())
            dx = p.x() - b.right();

        bool better;
        if (bestLine < 0)
            better = true;
        else if (inside != bestInside)
            better = inside; // a vertically-containing line always wins
        else if (inside)
            better = dx < bestDx; // among containing lines, nearest in x
        else
            better = dy < bestDy || (dy == bestDy && dx < bestDx); // else nearest in y, then x

        if (better) {
            bestLine = li;
            bestInside = inside;
            bestDy = dy;
            bestDx = dx;
        }
    }
    if (bestLine < 0)
        return 0;

    const LineSpan &ls = pt.lines[bestLine];
    for (int i = ls.start; i < ls.end; ++i) {
        const QRectF &r = pt.rects[i];
        if (r.isNull())
            continue;
        const double mid = r.left() + r.width() / 2.0;
        if (p.x() < mid)
            return i;
    }
    return ls.end; // caret after the last glyph on the line (before its '\n')
}

void TextIndex::wordBoundsAt(int pageNo, int offset, int *start, int *end)
{
    const PageText &pt = ensure(pageNo);
    const int n = static_cast<int>(pt.text.size());
    offset = std::clamp(offset, 0, n);

    int i = offset;
    if (i >= n || !isWordChar(pt.text.at(i))) {
        if (i > 0 && isWordChar(pt.text.at(i - 1)))
            i = i - 1; // click just past the end of a word selects that word
        else {
            *start = offset;
            *end = offset;
            return;
        }
    }
    int s = i;
    while (s > 0 && isWordChar(pt.text.at(s - 1)))
        --s;
    int e = i;
    while (e < n && isWordChar(pt.text.at(e)))
        ++e;
    *start = s;
    *end = e;
}

QString TextIndex::textRange(int pageNo, int start, int length)
{
    const PageText &pt = ensure(pageNo);
    const int n = static_cast<int>(pt.text.size());
    start = std::clamp(start, 0, n);
    const int end = std::clamp(start + length, start, n);
    return pt.text.mid(start, end - start);
}

std::vector<QRectF> TextIndex::rangeRects(int pageNo, int start, int length)
{
    const PageText &pt = ensure(pageNo);
    const int n = static_cast<int>(pt.text.size());
    start = std::clamp(start, 0, n);
    const int end = std::clamp(start + length, start, n);

    std::vector<QRectF> out;
    QRectF run;
    bool have = false;
    for (int i = start; i < end; ++i) {
        if (pt.text.at(i) == QLatin1Char('\n')) {
            if (have) {
                out.push_back(run);
                have = false;
            }
            continue;
        }
        const QRectF &r = pt.rects[i];
        if (r.isNull())
            continue;
        if (!have) {
            run = r;
            have = true;
        } else {
            run = run.united(r);
        }
    }
    if (have)
        out.push_back(run);
    return out;
}

std::vector<TextLink> TextIndex::detectLinksInText(const QString &text, int page)
{
    std::vector<TextLink> out;
    static const QRegularExpression rx(
        QStringLiteral(R"((?:https?://|www\.)[^\s<>()\[\]{}"]+)"),
        QRegularExpression::CaseInsensitiveOption);

    QRegularExpressionMatchIterator it = rx.globalMatch(text);
    while (it.hasNext()) {
        const QRegularExpressionMatch m = it.next();
        const int start = m.capturedStart();
        int length = m.capturedLength();
        QString raw = m.captured();

        while (length > 0 && isTrailingUrlPunctuation(raw.at(length - 1)))
            --length;
        if (length <= 0)
            continue;

        raw.truncate(length);
        out.push_back(TextLink{page, start, length, normalizedWebUrl(raw)});
    }
    return out;
}

std::optional<TextLink> TextIndex::linkAt(int pageNo, QPointF pagePoint)
{
    if (pageNo < 0 || pageNo >= static_cast<int>(pages_.size()))
        return std::nullopt;

    ensure(pageNo);
    PageText &pt = pages_[pageNo];
    if (!pt.linksReady) {
        pt.links = detectLinksInText(pt.text, pageNo);
        pt.linksReady = true;
    }

    for (const TextLink &link : pt.links) {
        for (const QRectF &rect : rangeRects(pageNo, link.start, link.length)) {
            if (rect.adjusted(-1.5, -2.0, 1.5, 2.0).contains(pagePoint))
                return link;
        }
    }
    return std::nullopt;
}

std::vector<TextMatch> TextIndex::matchInText(const QString &text, int page,
                                              const QString &query,
                                              bool caseSensitive, bool wholeWord)
{
    std::vector<TextMatch> out;
    if (query.isEmpty())
        return out;

    const Qt::CaseSensitivity cs = caseSensitive ? Qt::CaseSensitive : Qt::CaseInsensitive;
    const int qlen = static_cast<int>(query.size());
    const int n = static_cast<int>(text.size());

    int from = 0;
    while (from <= n) {
        const int idx = text.indexOf(query, from, cs);
        if (idx < 0)
            break;
        const int matchEnd = idx + qlen;
        bool ok = true;
        if (wholeWord) {
            if (idx > 0 && isWordChar(text.at(idx - 1)))
                ok = false;
            if (matchEnd < n && isWordChar(text.at(matchEnd)))
                ok = false;
        }
        if (ok) {
            out.push_back(TextMatch{page, idx, qlen});
            from = matchEnd; // count non-overlapping matches
        } else {
            from = idx + 1; // keep scanning for a word-boundary match
        }
    }
    return out;
}

std::vector<TextMatch> TextIndex::search(const QString &query, bool caseSensitive, bool wholeWord)
{
    std::vector<TextMatch> out;
    if (query.isEmpty())
        return out;
    for (int p = 0; p < static_cast<int>(pages_.size()); ++p) {
        const PageText &pt = ensure(p);
        if (pt.text.isEmpty())
            continue;
        auto pageMatches = matchInText(pt.text, p, query, caseSensitive, wholeWord);
        out.insert(out.end(), pageMatches.begin(), pageMatches.end());
    }
    return out;
}

} // namespace mervin
