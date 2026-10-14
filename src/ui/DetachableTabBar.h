#pragma once

#include <QPointer>
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

// Detect in-process tab movement; WindowManager reparents live TabPages. Native reordering
// applies inside the bar. Dragging far enough outside starts QDrag: dropping on another bar
// emits mergeRequested there; other drops emit detachRequested on the source.
// Each tab's data holds the page it shows as a QObject *, which the owner sets. Only
// such a tab can be dragged out. The drag follows that page, not its position, since
// tabs can arrive or go while QDrag runs its own event loop.
class DetachableTabBar : public QTabBar
{
    Q_OBJECT

public:
    explicit DetachableTabBar(QWidget *parent = nullptr);

    // True while a tab is dragged out of any bar (QDrag::exec runs).
    static bool dragInProgress() { return s_source != nullptr; }

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
    QObject *pageAt(int index) const;     // the page a tab shows, or null
    int indexOfPage(const QObject *page) const; // -1 when no tab shows it

    QPoint pressPos_;
    QPointer<QObject> pressPage_; // page of the tab pressed last
    bool dragging_ = false;
    int dropIndicator_ = -1; // insertion index to paint, or -1

    // In-process drag state (only one tab drag happens at a time).
    static DetachableTabBar *s_source;
    static QPointer<QObject> s_sourcePage;
};

} // namespace mervin
