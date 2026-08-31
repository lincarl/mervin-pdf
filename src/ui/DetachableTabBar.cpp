#include "ui/DetachableTabBar.h"

#include <QApplication>
#include <QDrag>
#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QMimeData>
#include <QPainter>
#include <QPixmap>

namespace mervin {

namespace {
// Marker MIME type; the actual payload (source bar + index) is passed in-process
// via the static members, since this is never a cross-process drag.
const char *kMime = "application/x-mervin-tab";
// How far the pointer must leave the bar before a drag becomes a detach.
constexpr int kDetachMargin = 28;
} // namespace

DetachableTabBar *DetachableTabBar::s_source = nullptr;
int DetachableTabBar::s_sourceIndex = -1;

DetachableTabWidget::DetachableTabWidget(QWidget *parent)
    : QTabWidget(parent)
{
    setTabBar(new DetachableTabBar(this)); // setTabBar is protected; reach it here
}

DetachableTabBar *DetachableTabWidget::detachableTabBar() const
{
    return qobject_cast<DetachableTabBar *>(tabBar());
}

DetachableTabBar::DetachableTabBar(QWidget *parent)
    : QTabBar(parent)
{
    setAcceptDrops(true);
    setMovable(true); // in-bar reordering while the pointer stays inside
    setElideMode(Qt::ElideRight);

    // The built-in movable reorder can shuffle tabs while the pointer is still
    // inside the bar, BEFORE a detach/merge drag begins. pressIndex_ is captured
    // at mouse-press, so follow the pressed tab through those moves - otherwise
    // a reorder-then-drag-out would detach/merge the wrong tab.
    connect(this, &QTabBar::tabMoved, this, [this](int from, int to) {
        if (pressIndex_ < 0)
            return;
        if (from == pressIndex_)
            pressIndex_ = to;
        else if (from < pressIndex_ && to >= pressIndex_)
            --pressIndex_;
        else if (from > pressIndex_ && to <= pressIndex_)
            ++pressIndex_;
    });
}

void DetachableTabBar::mousePressEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton) {
        pressIndex_ = tabAt(event->pos());
        pressPos_ = event->pos();
        dragging_ = false;
    }
    QTabBar::mousePressEvent(event);
}

void DetachableTabBar::mouseMoveEvent(QMouseEvent *event)
{
    if (!(event->buttons() & Qt::LeftButton) || pressIndex_ < 0 || dragging_) {
        QTabBar::mouseMoveEvent(event);
        return;
    }
    if ((event->pos() - pressPos_).manhattanLength() < QApplication::startDragDistance()) {
        QTabBar::mouseMoveEvent(event);
        return;
    }

    // Once the pointer has moved well outside the bar, switch from in-bar
    // reordering to a detach/merge drag.
    const QRect r = rect().adjusted(-kDetachMargin, -kDetachMargin, kDetachMargin, kDetachMargin);
    if (r.contains(event->pos())) {
        QTabBar::mouseMoveEvent(event); // still inside: let QTabBar reorder
        return;
    }
    startDetachDrag();
}

void DetachableTabBar::startDetachDrag()
{
    if (pressIndex_ < 0 || pressIndex_ >= count())
        return;
    dragging_ = true;
    s_source = this;
    s_sourceIndex = pressIndex_;

    auto *drag = new QDrag(this);
    auto *mime = new QMimeData;
    mime->setData(QString::fromLatin1(kMime), QByteArray("1"));
    drag->setMimeData(mime);

    // A small drag pixmap from the dragged tab for visual feedback.
    const QRect tr = tabRect(pressIndex_);
    if (tr.isValid()) {
        QPixmap pm(tr.size());
        pm.fill(Qt::transparent);
        QPainter p(&pm);
        p.translate(-tr.topLeft());
        // Render just this tab region of the bar.
        render(&p, QPoint(), QRegion(tr));
        p.end();
        drag->setPixmap(pm);
        drag->setHotSpot(pressPos_ - tr.topLeft());
    }

    const Qt::DropAction result = drag->exec(Qt::MoveAction);

    // If nothing accepted the drop, detach into a new window at the cursor.
    if (result == Qt::IgnoreAction && s_source == this)
        emit detachRequested(s_sourceIndex, QCursor::pos());

    s_source = nullptr;
    s_sourceIndex = -1;
    dragging_ = false;
    pressIndex_ = -1;
    dropIndicator_ = -1;
    update();
}

int DetachableTabBar::insertionIndexAt(int x) const
{
    for (int i = 0; i < count(); ++i) {
        const QRect r = tabRect(i);
        if (x < r.center().x())
            return i;
    }
    return count();
}

void DetachableTabBar::dragEnterEvent(QDragEnterEvent *event)
{
    if (event->mimeData()->hasFormat(QString::fromLatin1(kMime)) && s_source) {
        event->setDropAction(Qt::MoveAction);
        event->accept();
    } else {
        QTabBar::dragEnterEvent(event);
    }
}

void DetachableTabBar::dragMoveEvent(QDragMoveEvent *event)
{
    if (event->mimeData()->hasFormat(QString::fromLatin1(kMime)) && s_source) {
        dropIndicator_ = insertionIndexAt(event->position().toPoint().x());
        update();
        event->setDropAction(Qt::MoveAction);
        event->accept();
    } else {
        QTabBar::dragMoveEvent(event);
    }
}

void DetachableTabBar::dragLeaveEvent(QDragLeaveEvent *event)
{
    dropIndicator_ = -1;
    update();
    QTabBar::dragLeaveEvent(event);
}

void DetachableTabBar::dropEvent(QDropEvent *event)
{
    if (event->mimeData()->hasFormat(QString::fromLatin1(kMime)) && s_source) {
        const int target = insertionIndexAt(event->position().toPoint().x());
        DetachableTabBar *source = s_source;
        const int sourceIndex = s_sourceIndex;
        dropIndicator_ = -1;
        update();
        event->setDropAction(Qt::MoveAction);
        event->accept();
        emit mergeRequested(source, sourceIndex, target);
    } else {
        QTabBar::dropEvent(event);
    }
}

void DetachableTabBar::paintEvent(QPaintEvent *event)
{
    QTabBar::paintEvent(event);
    if (dropIndicator_ < 0)
        return;
    // Draw a vertical insertion line at the drop position.
    int x = 0;
    if (dropIndicator_ < count())
        x = tabRect(dropIndicator_).left();
    else if (count() > 0)
        x = tabRect(count() - 1).right();
    QPainter p(this);
    QPen pen(palette().color(QPalette::Highlight));
    pen.setWidth(2);
    p.setPen(pen);
    p.drawLine(x, 0, x, height());
}

} // namespace mervin
