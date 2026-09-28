#pragma once

#include "extract/ExtractPlan.h"
#include "render/ThumbnailCache.h"

#include <QColor>
#include <QIcon>
#include <QListView>
#include <QPoint>
#include <QStringList>

class QTimer;

namespace mervin {

class Document;
class RenderEngine;
class ExtractCellModel;

// The result strip in the Extract Pages dialog: one QListView item per
// ExtractPlan::Cell, in output order, painted by an internal delegate. Owns the
// thumbnail cache and the render pump (one page per event-loop turn, after a
// 150 ms debounce, only for cells in view). Holds no plan state: the dialog hands
// it cells after each change and turns its signals into plan edits.
//
// Fixed height: the thumbnails are always 89 x 124, and the bottom 16 px of the
// viewport stay free for the horizontal scrollbar, so nothing moves when it
// appears.
class ExtractStrip : public QListView
{
    Q_OBJECT

public:
    enum Role { KindRole = Qt::UserRole + 1 };
    struct Content
    {
        QList<ExtractPlan::Cell> cells;
        QStringList captions; // plan.caption(cell), one per cell
        QStringList toolTips; // plan.describe(cell), one per cell
    };

    explicit ExtractStrip(QWidget *parent = nullptr);

    // The cell widths this strip draws, for ExtractPlan::cells().
    static ExtractPlan::Metrics metrics();
    void setSource(Document *doc, RenderEngine *engine); // either may be null
    // Replace every cell and make the cell holding `currentFlat` current (the last
    // cell when it is past the end). Does not scroll: see ensureRowVisible.
    void setContent(const Content &content, int currentFlat);
    const ExtractPlan::Cell *currentCell() const; // null when the strip is empty
    // The cells of this row get a quiet wash, so the list's current row can be
    // found in the strip. -1 for none.
    void setHighlightedRow(int row);
    // Scroll the row's cells into view, unless one of them already is; when they
    // do not all fit, its first cell wins.
    void ensureRowVisible(int row);

signals:
    void viewportResized();                          // the dialog refolds
    void foldActivated(const ExtractPlan::RunKey &); // a click on a fold
    void removeClicked(const ExtractPlan::Cell &);   // the hover ✕
    void currentCellChanged();                       // refresh action states
    // The user picked a cell of `row`, by click or keyboard (never by setContent).
    void rowPicked(int row);

protected:
    bool event(QEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    bool viewportEvent(QEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void focusInEvent(QFocusEvent *event) override;
    void focusOutEvent(QFocusEvent *event) override;
    void currentChanged(const QModelIndex &current, const QModelIndex &previous) override;
    void keyboardSearch(const QString &) override {} // no type-ahead: digits mean nothing here

private:
    friend class ExtractCellDelegate;

    // A cell's content: its item rect minus the lead before it, hover wash height.
    QRect contentRect(int row) const;
    int cellAt(const QPoint &pos) const; // the row whose content is under `pos`, or -1
    QRect closeRect(int row) const;      // the hover ✕, in viewport coordinates
    void paintCell(QPainter *p, int row) const;
    // Re-derive the hovered cell from the pointer, for when the cells move under a
    // pointer that did not (a rebuild or a scroll), and repaint both rows.
    void syncHoverToCursor();
    void updateRow(int row); // the item rect plus the wash overhang below it
    void pump();             // render one visible page that needs it, then re-arm

    ExtractCellModel *model_ = nullptr;
    Document *doc_ = nullptr;
    RenderEngine *engine_ = nullptr;
    ThumbnailCache cache_{128}; // 89 x 124 each: about 22 MB at 200% scaling
    QTimer *renderTimer_ = nullptr; // the only trigger for pump(): debounce, then 0 per page
    int innerWidth_ = -1;
    int highlightedRow_ = -1;
    bool settingContent_ = false; // setContent() moves the current cell, not the user
    mutable QIcon foldGlyph_;       // FileText for fold tiles, cached per ink by paintCell
    mutable QColor foldGlyphInk_;
    int hoverRow_ = -1;
    QPoint hoverPos_;
    int pressRow_ = -1;
    bool pressOnClose_ = false;
};

} // namespace mervin
