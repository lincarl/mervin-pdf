#pragma once

#include <QTabBar>
#include <QTabWidget>

namespace mervin {

class DetachableTabBar;

// A QTabWidget that uses a DetachableTabBar. Needed only because
// QTabWidget::setTabBar() is protected, so the bar must be installed from a
// subclass constructor.
class DetachableTabWidget : public QTabWidget
{
    Q_OBJECT

public:
    explicit DetachableTabWidget(QWidget *parent = nullptr);
    DetachableTabBar *detachableTabBar() const;
};

// A QTabBar that supports dragging a tab out of the bar to detach it into a new
// window, and dropping a dragged tab onto another window's tab bar to merge it.
// All moves are in-process (the live TabPage widget is re-parented between
// windows - see WindowManager); this bar only detects the gestures.
//
// Reordering within the bar uses QTabBar's built-in movable behaviour while the
// pointer stays inside the bar; once it leaves the bar far enough, a QDrag
// begins. If that drag is dropped on another DetachableTabBar, mergeRequested
// fires there; if it ends anywhere else (the desktop, a window body),
// detachRequested fires on the source.
class DetachableTabBar : public QTabBar
{
    Q_OBJECT

public:
    explicit DetachableTabBar(QWidget *parent = nullptr);

signals:
    // The dragged tab was released outside any tab bar: detach it to a new
    // window positioned near globalPos.
    void detachRequested(int index, const QPoint &globalPos);
    // A tab from `source` (at sourceIndex) was dropped onto this bar; insert it
    // at targetIndex. source may equal this bar (a reorder).
    void mergeRequested(mervin::DetachableTabBar *source, int sourceIndex, int targetIndex);

protected:
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dragMoveEvent(QDragMoveEvent *event) override;
    void dragLeaveEvent(QDragLeaveEvent *event) override;
    void dropEvent(QDropEvent *event) override;
    void paintEvent(QPaintEvent *event) override;

private:
    void startDetachDrag();
    int insertionIndexAt(int x) const; // where a drop at x would insert

    QPoint pressPos_;
    int pressIndex_ = -1;
    bool dragging_ = false;
    int dropIndicator_ = -1; // insertion index to paint, or -1

    // In-process drag state (only one tab drag happens at a time).
    static DetachableTabBar *s_source;
    static int s_sourceIndex;
};

} // namespace mervin
