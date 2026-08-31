#pragma once

#include <QMetaType>
#include <QRect>
#include <QSize>

#include <vector>

namespace mervin {

class Document;

// Computes page rectangles in logical (device-independent) pixels.
//
// The layout has TWO INDEPENDENT AXES, and every combination is legal:
//
//   scroll: Continuous - every row is laid out, the reader scrolls the document.
//           Single     - only the row holding the current page is laid out, so
//                        the canvas is one screenful and paging replaces it.
//   spread: false      - one page per row.
//           true       - facing pages share a row, taken from the document's
//                        spread plan (see rebuildSpreadPlan).
//
// The two used to be one three-valued enum, which forced the reader to give up
// their scrolling preference to see a spread and gave "two-page" no way back.
// Keeping them apart means Single+spread - one spread at a time - simply exists.
//
// ViewLayout also owns the fit arithmetic (fitBasis), because the canvas size is
// a property of the layout and not of any single page: getting a fit right means
// knowing the margins, the inner gap, the breathing space and the per-page
// rounding, all of which live here.
class ViewLayout
{
public:
    enum class Scroll { Continuous, Single };

    struct Mode
    {
        Scroll scroll = Scroll::Continuous;
        bool spread = false;
        bool operator==(const Mode &) const = default;
    };

    // The two candidate scales for a viewport of fullW x fullH logical pixels
    // (the caller has already decided which scrollbars are showing):
    //   widthScale  - the largest scale whose laid-out canvas still fits fullW.
    //   heightScale - the largest scale at which the row holding the current page
    //                 fits fullH.
    // Fit Width takes widthScale; Fit Page takes min(widthScale, heightScale).
    struct FitBasis
    {
        double widthScale = 1.0;
        double heightScale = 1.0;
    };

    void setDocument(const Document *doc);
    void setScale(double scale);   // points -> logical pixels
    void setRotation(int degrees); // 0 / 90 / 180 / 270
    void setMode(Mode mode);
    void setCurrentPage(int index); // used by Scroll::Single

    Mode mode() const { return mode_; }
    QSize totalSize() const { return total_; }
    // Total layout height/width at a hypothetical scale/mode/rotation/current
    // page, without touching the laid-out state. Lets the viewer's fit
    // computation predict the canvas BEFORE committing to a scale (notably
    // whether content will scroll vertically, and thus show the vertical
    // scrollbar). Both mirror relayout()'s math exactly, same rounding and same
    // pixel constants - keep the three in sync.
    int heightForScale(double scale, Mode mode, int rotation, int currentPage) const;
    int widthForScale(double scale, Mode mode, int rotation, int currentPage) const;
    // The fit scales for a viewport of fullW x fullH logical pixels.
    FitBasis fitBasis(double fullW, double fullH, Mode mode, int rotation,
                      int currentPage) const;

    // Rows. With spread on a row is a spread taken from the document's spread
    // plan; otherwise every page is its own row. rowEnd() is exclusive. Both
    // clamp, so they are safe on an empty layout.
    int rowStart(int pageNo) const;
    int rowEnd(int pageNo) const;
    // Whether this page is an oversized sheet holding a row of its own.
    bool isStandalone(int pageNo) const;

    QRect pageRect(int pageNo) const;
    std::vector<int> pagesInViewport(const QRect &viewport) const;
    // The first page of the row at canvas y (the row *leader*, so a spread
    // reports one page however its two halves are sized).
    int pageAtY(int y) const;
    int pageCount() const { return static_cast<int>(rects_.size()); }

    double scale() const { return scale_; }
    int rotation() const { return rotation_; }

private:
    // One row of the layout. `standalone` marks an oversized sheet that was given
    // a row to itself; a `count == 1` row that is not standalone is an ordinary
    // trailing page at the end of an odd-length document.
    struct Row
    {
        int first = 0;
        int count = 1;
        bool standalone = false;
    };

    void rebuildSpreadPlan();
    void relayout();
    QSize displaySize(int pageNo) const;
    // The rows a given mode actually lays out: the spread plan or one row per
    // page (the spread axis), narrowed to the row holding currentPage when the
    // scroll axis is Single. One definition, shared by relayout() and all three
    // predictors, so a hypothetical mode is described exactly like the live one.
    std::vector<Row> rowsFor(Mode mode, int currentPage) const;
    // The row holding `pageNo` in the spread plan, or nullptr when there is no
    // plan. Unlike rowStart()/rowEnd() this ignores the *current* mode, so the
    // predictors can answer for a hypothetical one: the viewer re-fits for a mode
    // it has not laid out yet when the reader turns the spread on.
    const Row *rowFor(int pageNo) const;
    // Page extents in points under an arbitrary rotation (the quarter turns swap
    // the axes). Used by the predictors, which must not disturb rotation_.
    double pageWidthPt(int pageNo, int rotation) const;
    double pageHeightPt(int pageNo, int rotation) const;
    // The three column widths of a spread layout at `scale`, in logical pixels:
    // the widest left-hand page (ordinary trailing pages included - they sit in
    // the left column), the widest right-hand page, and the widest standalone
    // sheet. Shared by relayout() and widthForScale() so the two cannot drift.
    struct Columns
    {
        int left = 0;
        int right = 0;
        int standalone = 0;
    };
    Columns columnWidths(const std::vector<Row> &rows, const std::vector<QSize> &sizes) const;
    // Canvas width (excluding margins) implied by those columns.
    int contentWidth(const Columns &c) const;

    const Document *doc_ = nullptr;
    double scale_ = 1.0;
    int rotation_ = 0;
    Mode mode_;
    int current_ = 0;
    int gap_ = 12;      // between rows
    int innerGap_ = 8;  // between the two pages of a spread
    int margin_ = 16;
    int slack_ = 8;     // breathing space a fitted canvas leaves in the viewport

    // The spread plan: which pages share a row when the spread axis is on.
    // Computed once per document (rebuildSpreadPlan) from unrotated page sizes,
    // so rotating or zooming never reflows the book and page indices stay stable.
    std::vector<Row> plan_;
    std::vector<int> rowOf_; // page -> index into plan_

    // The rows relayout() actually placed, i.e. rowsFor(mode_, current_). Kept so
    // pageAtY - which runs on every scroll - walks them without rebuilding.
    std::vector<Row> rows_;

    std::vector<QRect> rects_;
    QSize total_{0, 0};
};

} // namespace mervin

Q_DECLARE_METATYPE(mervin::ViewLayout::Mode)
