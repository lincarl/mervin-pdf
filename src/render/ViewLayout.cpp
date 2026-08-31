#include "render/ViewLayout.h"

#include "render/Document.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace mervin {
namespace {

// Two pages count as the same sheet when their areas agree within this factor.
// Slack for a scanner that rounds a millimetre differently on one page.
constexpr double kSheetTolerance = 1.02;

// Above this multiple of the reference sheet's AREA a page is treated as
// oversized and given a row of its own, instead of being paired with a normal
// sheet. 1.5 sits between 1.0 (the same sheet, turned) and 2.0 (the next ISO size
// up), so a document of uniform pages is never affected.
//
// AREA, not width, and the ISO ratios are why: one size up doubles the area but
// multiplies the width by only sqrt(2). Measured on width, an A3 sheet among A4s
// comes out at 1.41 and slips under any threshold that still ignores a page
// merely turned on its side - which is to say a width rule cannot separate the
// two cases at all, and would both miss the A3 title sheet in
// examples/schematic.pdf and give every landscape page of a portrait report a row
// of its own. Verified both ways in tst_view_layout.
constexpr double kOversizedAreaRatio = 1.5;

} // namespace

void ViewLayout::setDocument(const Document *doc)
{
    doc_ = doc;
    rebuildSpreadPlan();
    relayout();
}

void ViewLayout::setScale(double scale)
{
    scale_ = scale;
    relayout();
}

void ViewLayout::setRotation(int degrees)
{
    rotation_ = ((degrees % 360) + 360) % 360;
    relayout();
}

void ViewLayout::setMode(Mode mode)
{
    mode_ = mode;
    relayout();
}

void ViewLayout::setCurrentPage(int index)
{
    current_ = index;
    if (mode_.scroll == Scroll::Single)
        relayout();
}

double ViewLayout::pageWidthPt(int pageNo, int rotation) const
{
    const QSizeF pt = doc_->pageSize(pageNo);
    return (rotation == 90 || rotation == 270) ? pt.height() : pt.width();
}

double ViewLayout::pageHeightPt(int pageNo, int rotation) const
{
    const QSizeF pt = doc_->pageSize(pageNo);
    return (rotation == 90 || rotation == 270) ? pt.width() : pt.height();
}

QSize ViewLayout::displaySize(int pageNo) const
{
    const QSizeF pt = doc_->pageSize(pageNo);
    double w = pt.width() * scale_;
    double h = pt.height() * scale_;
    if (rotation_ == 90 || rotation_ == 270)
        std::swap(w, h);
    return {static_cast<int>(std::lround(w)), static_cast<int>(std::lround(h))};
}

void ViewLayout::rebuildSpreadPlan()
{
    plan_.clear();
    const int n = doc_ ? doc_->pageCount() : 0;
    rowOf_.assign(std::max(0, n), 0);
    if (n == 0)
        return;

    // The reference sheet is the largest cluster of pages whose areas agree
    // within kSheetTolerance, resolved to the smallest area in that cluster.
    // Unrotated sizes, so turning the view never reflows the book.
    std::vector<double> areas(n);
    for (int i = 0; i < n; ++i) {
        const QSizeF pt = doc_->pageSize(i);
        areas[i] = std::max(0.0, pt.width() * pt.height());
    }
    std::vector<double> sorted = areas;
    std::sort(sorted.begin(), sorted.end());
    double reference = sorted.front();
    int widest = 0;
    for (int lo = 0, hi = 0; lo < n; ++lo) {
        // The bound only grows with lo, so hi never has to walk back.
        while (hi < n && sorted[hi] <= sorted[lo] * kSheetTolerance)
            ++hi;
        if (hi - lo > widest) {
            widest = hi - lo;
            reference = sorted[lo];
        }
    }

    // A zero-area document (degenerate or malformed) has no meaningful reference,
    // so nothing is oversized and pairing falls back to plain index parity.
    const double limit = (reference > 0.0) ? reference * kOversizedAreaRatio
                                           : std::numeric_limits<double>::max();
    const auto oversized = [&](int i) { return areas[i] > limit; };

    for (int i = 0; i < n;) {
        if (oversized(i)) {
            plan_.push_back({i, 1, true});
            ++i;
        } else if (i + 1 < n && !oversized(i + 1)) {
            plan_.push_back({i, 2, false});
            i += 2;
        } else {
            // Last page of an odd-length document, or the page before an
            // oversized sheet: alone, but an ordinary sheet all the same.
            plan_.push_back({i, 1, false});
            ++i;
        }
    }
    for (int r = 0; r < static_cast<int>(plan_.size()); ++r)
        for (int k = 0; k < plan_[r].count; ++k)
            rowOf_[plan_[r].first + k] = r;
}

const ViewLayout::Row *ViewLayout::rowFor(int pageNo) const
{
    if (rowOf_.empty())
        return nullptr;
    const int p = std::clamp(pageNo, 0, static_cast<int>(rowOf_.size()) - 1);
    return &plan_[rowOf_[p]];
}

std::vector<ViewLayout::Row> ViewLayout::rowsFor(Mode mode, int currentPage) const
{
    std::vector<Row> rows;
    const int n = doc_ ? doc_->pageCount() : 0;
    if (n == 0)
        return rows;
    const int cur = std::clamp(currentPage, 0, n - 1);

    // The spread axis picks WHICH rows exist; the scroll axis picks HOW MANY of
    // them are laid out. Single narrows to the row holding the current page, so
    // "one page at a time" and "one spread at a time" are the same code path.
    if (mode.scroll == Scroll::Single) {
        const Row *r = mode.spread ? rowFor(cur) : nullptr;
        rows.push_back(r ? *r : Row{cur, 1, false});
        return rows;
    }
    if (mode.spread)
        return plan_;
    rows.reserve(n);
    for (int i = 0; i < n; ++i)
        rows.push_back({i, 1, false});
    return rows;
}

int ViewLayout::rowStart(int pageNo) const
{
    const Row *r = mode_.spread ? rowFor(pageNo) : nullptr;
    return r ? r->first : pageNo;
}

int ViewLayout::rowEnd(int pageNo) const
{
    const Row *r = mode_.spread ? rowFor(pageNo) : nullptr;
    return r ? r->first + r->count : pageNo + 1;
}

bool ViewLayout::isStandalone(int pageNo) const
{
    if (rowOf_.empty() || pageNo < 0 || pageNo >= static_cast<int>(rowOf_.size()))
        return false;
    return plan_[rowOf_[pageNo]].standalone;
}

ViewLayout::Columns ViewLayout::columnWidths(const std::vector<Row> &rows,
                                             const std::vector<QSize> &sizes) const
{
    Columns c;
    const int n = static_cast<int>(sizes.size());
    for (const Row &r : rows) {
        if (r.first >= n)
            break;
        if (r.count == 2 && r.first + 1 < n) {
            c.left = std::max(c.left, sizes[r.first].width());
            c.right = std::max(c.right, sizes[r.first + 1].width());
        } else if (r.standalone) {
            c.standalone = std::max(c.standalone, sizes[r.first].width());
        } else {
            // An ordinary trailing page keeps the left column: the last page of a
            // book, not a page floating in the middle of the window.
            c.left = std::max(c.left, sizes[r.first].width());
        }
    }
    return c;
}

int ViewLayout::contentWidth(const Columns &c) const
{
    const int paired = c.left + (c.right > 0 ? innerGap_ + c.right : 0);
    return std::max(paired, c.standalone);
}

int ViewLayout::heightForScale(double scale, Mode mode, int rotation, int currentPage) const
{
    const int n = doc_ ? doc_->pageCount() : 0;
    if (n == 0)
        return 0;
    // Same rounding and rotation handling as displaySize(), same rows as
    // relayout(), so the prediction matches the height relayout() will produce.
    const auto pageH = [&](int i) {
        return static_cast<int>(std::lround(pageHeightPt(i, rotation) * scale));
    };

    const std::vector<Row> rows = rowsFor(mode, currentPage);
    if (rows.empty())
        return 0;
    int content = -gap_; // k rows contribute k-1 gaps
    for (const Row &r : rows) {
        int rowH = 0;
        for (int k = 0; k < r.count && r.first + k < n; ++k)
            rowH = std::max(rowH, pageH(r.first + k));
        content += rowH + gap_;
    }
    return content + 2 * margin_;
}

int ViewLayout::widthForScale(double scale, Mode mode, int rotation, int currentPage) const
{
    const int n = doc_ ? doc_->pageCount() : 0;
    if (n == 0)
        return 0;
    const auto pageW = [&](int i) {
        return static_cast<int>(std::lround(pageWidthPt(i, rotation) * scale));
    };

    const std::vector<Row> rows = rowsFor(mode, currentPage);
    if (rows.empty())
        return 0;

    if (mode.spread) {
        std::vector<QSize> sizes(n);
        for (int i = 0; i < n; ++i)
            sizes[i] = QSize(pageW(i), 0);
        return contentWidth(columnWidths(rows, sizes)) + 2 * margin_;
    }

    int maxW = 0;
    for (const Row &r : rows)
        maxW = std::max(maxW, pageW(r.first));
    return maxW + 2 * margin_;
}

ViewLayout::FitBasis ViewLayout::fitBasis(double fullW, double fullH, Mode mode,
                                          int rotation, int currentPage) const
{
    FitBasis b;
    const int n = doc_ ? doc_->pageCount() : 0;
    if (n == 0)
        return b;

    // The canvas is `content + 2 * margin_`, and a fitted view leaves slack_ px
    // of breathing space, so the content budget is what remains. Every pixel
    // constant of the layout is applied here and nowhere else - the viewer used
    // to hard-code `- 40`, which silently equalled 2 * margin_ + slack_ and so
    // spent the whole breathing space on a two-page row's inner gap.
    const double availW = std::max(1.0, fullW - 2 * margin_ - slack_);
    const double availH = std::max(1.0, fullH - 2 * margin_ - slack_);
    const int cur = std::clamp(currentPage, 0, n - 1);
    const auto pw = [&](int i) { return std::max(1.0, pageWidthPt(i, rotation)); };
    const auto ph = [&](int i) { return std::max(1.0, pageHeightPt(i, rotation)); };

    // Each laid-out column rounds up by at most half a pixel (displaySize's
    // lround), so a k-column row can exceed its points prediction by k/2 px.
    // Spend that up front and a fitted view can never grow a horizontal
    // scrollbar for a rounding error.
    const std::vector<Row> rows = rowsFor(mode, cur);
    double ws = std::numeric_limits<double>::max();
    if (mode.spread) {
        // Mirror of the column model in relayout(): the canvas is the widest
        // left-hand page plus the widest right-hand page (plus the inner gap),
        // or the widest standalone sheet, whichever is larger. Every laid-out row
        // must fit, so take the tightest constraint. In Single that is one row,
        // which is exactly the spread on screen.
        double left = 0.0, right = 0.0, standalone = 0.0;
        for (const Row &r : rows) {
            if (r.first >= n)
                break;
            if (r.count == 2 && r.first + 1 < n) {
                left = std::max(left, pw(r.first));
                right = std::max(right, pw(r.first + 1));
            } else if (r.standalone) {
                standalone = std::max(standalone, pw(r.first));
            } else {
                left = std::max(left, pw(r.first));
            }
        }
        if (left > 0.0) {
            const double pad = (right > 0.0) ? (innerGap_ + 1.0) : 0.5;
            ws = std::min(ws, (availW - pad) / (left + right));
        }
        if (standalone > 0.0)
            ws = std::min(ws, (availW - 0.5) / standalone);
    } else { // the canvas is as wide as the widest laid-out page
        double maxW = 0.0;
        for (const Row &r : rows)
            maxW = std::max(maxW, pw(r.first));
        ws = (availW - 0.5) / std::max(1.0, maxW);
    }

    // Fit Page fits the row you are looking at, not the tallest row in the
    // document: a foldout elsewhere must not shrink the sheet on screen.
    // rowFor(), not rowStart(): the viewer re-fits for a mode before it has laid
    // that mode out, so mode_ still reads as the mode being left behind.
    double rowPt = ph(cur);
    if (mode.spread) {
        const Row *row = rowFor(cur);
        for (int k = 0; row && k < row->count && row->first + k < n; ++k)
            rowPt = std::max(rowPt, ph(row->first + k));
    }
    b.heightScale = availH / std::max(1.0, rowPt);

    b.widthScale = std::max(1e-6, ws);
    // Validate the closed form against the predictor that actually rounds. The
    // algebra above should land it, but margin_ / innerGap_ / the column count
    // can all move later, and a fit mode that grows a horizontal scrollbar is
    // exactly the bug this function exists to prevent. One or two steps at most.
    for (int guard = 0; guard < 8
         && widthForScale(b.widthScale, mode, rotation, cur) > fullW - slack_;
         ++guard)
        b.widthScale = std::max(1e-6, b.widthScale - 1.0 / availW);
    return b;
}

void ViewLayout::relayout()
{
    const int n = doc_ ? doc_->pageCount() : 0;
    rects_.assign(n, QRect());
    rows_ = rowsFor(mode_, current_);
    if (n == 0 || rows_.empty()) {
        total_ = QSize(0, 0);
        return;
    }

    std::vector<QSize> sizes(n);
    for (int i = 0; i < n; ++i)
        sizes[i] = displaySize(i);

    if (mode_.spread) {
        // Column model with one spine shared by every laid-out row: every
        // left-hand page is right-aligned to it and every right-hand page
        // left-aligned after innerGap_, so the gutter sits at the same x on every
        // spread and the sheets form two clean columns even when they differ in
        // size. (Centring each row inside the widest row, as this used to, slid
        // the gutter sideways by up to 170 px as the reader scrolled a mixed
        // document.) In Scroll::Single there is one row, so the spine simply
        // centres that spread. Rows are top-aligned: facing sheets share the head
        // edge as a bound book does, which also makes pageRect(i).top() the row
        // top, so navigating to a page lands on the whole spread rather than part
        // way into it.
        const Columns cols = columnWidths(rows_, sizes);
        const int content = contentWidth(cols);
        const int paired = cols.left + (cols.right > 0 ? innerGap_ + cols.right : 0);
        const int spineX = margin_ + (content - paired) / 2 + cols.left;
        const int rightX = spineX + innerGap_;
        int y = margin_;
        for (const Row &r : rows_) {
            if (r.first >= n)
                break;
            int rowH = 0;
            for (int k = 0; k < r.count && r.first + k < n; ++k)
                rowH = std::max(rowH, sizes[r.first + k].height());
            if (r.count == 2 && r.first + 1 < n) {
                rects_[r.first] = QRect(spineX - sizes[r.first].width(), y,
                                        sizes[r.first].width(), sizes[r.first].height());
                rects_[r.first + 1] = QRect(rightX, y, sizes[r.first + 1].width(),
                                            sizes[r.first + 1].height());
            } else if (r.standalone) {
                // An oversized sheet is centred on the canvas, so its centreline
                // lands on the gutter and it reads as a title plate.
                rects_[r.first] = QRect(margin_ + (content - sizes[r.first].width()) / 2, y,
                                        sizes[r.first].width(), sizes[r.first].height());
            } else {
                rects_[r.first] = QRect(spineX - sizes[r.first].width(), y,
                                        sizes[r.first].width(), sizes[r.first].height());
            }
            y += rowH + gap_;
        }
        y -= gap_;
        total_ = QSize(content + 2 * margin_, y + margin_);
        return;
    }

    // One page per row, each centred in the widest laid-out page. In
    // Scroll::Single that is the single current page, so the canvas is the page
    // plus its margins and the centring is a no-op.
    int maxW = 0;
    for (const Row &r : rows_)
        maxW = std::max(maxW, sizes[r.first].width());
    int y = margin_;
    for (const Row &r : rows_) {
        const QSize &s = sizes[r.first];
        rects_[r.first] = QRect(margin_ + (maxW - s.width()) / 2, y, s.width(), s.height());
        y += s.height() + gap_;
    }
    y -= gap_;
    total_ = QSize(maxW + 2 * margin_, y + margin_);
}

QRect ViewLayout::pageRect(int pageNo) const
{
    if (pageNo >= 0 && pageNo < static_cast<int>(rects_.size()))
        return rects_[pageNo];
    return {};
}

std::vector<int> ViewLayout::pagesInViewport(const QRect &viewport) const
{
    std::vector<int> out;
    for (int i = 0; i < static_cast<int>(rects_.size()); ++i)
        if (rects_[i].isValid() && rects_[i].intersects(viewport))
            out.push_back(i);
    return out;
}

int ViewLayout::pageAtY(int y) const
{
    // Walk the laid-out ROWS, not rects: the two halves of a spread can differ in
    // height, so rects_ is not sorted by bottom() and a plain scan would skip the
    // shorter half of every row for ever (it ends above its partner, so its band
    // is already consumed) or hand back the right-hand page mid-spread. Comparing
    // against the row's lowest edge and reporting the row's first page keeps the
    // answer one page per spread, whichever way the sizes fall. With the spread
    // axis off every row is one page, so this is the plain scan it replaces.
    int last = -1;
    for (const Row &r : rows_) {
        int bottom = 0;
        int leader = -1;
        for (int k = 0; k < r.count && r.first + k < static_cast<int>(rects_.size()); ++k) {
            const QRect &rc = rects_[r.first + k];
            if (!rc.isValid())
                continue;
            if (leader < 0)
                leader = r.first + k;
            bottom = std::max(bottom, rc.bottom());
        }
        if (leader < 0)
            continue;
        last = leader;
        if (y <= bottom + gap_ / 2)
            return leader;
    }
    return last;
}

} // namespace mervin
