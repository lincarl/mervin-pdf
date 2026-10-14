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
// Marker MIME type; the actual payload (source bar + page) is passed in-process
// via the static members, since this is never a cross-process drag.
const char *kMime = "application/x-mervin-tab";
// How far the pointer must leave the bar before a drag becomes a detach.
constexpr int kDetachMargin = 28;
} // namespace

DetachableTabBar *DetachableTabBar::s_source = nullptr;
QPointer<QObject> DetachableTabBar::s_sourcePage;

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
}

QObject *DetachableTabBar::pageAt(int index) const
{
    return tabData(index).value<QObject *>();
}

int DetachableTabBar::indexOfPage(const QObject *page) const
{
    if (!page)
        return -1;
    for (int i = 0; i < count(); ++i)
        if (pageAt(i) == page)
            return i;
    return -1;
}

void DetachableTabBar::mousePressEvent(QMouseEvent *event)
{
    // Keep the page, not its position. The built-in movable reorder can shuffle
    // tabs while the pointer is still inside the bar, before a drag out begins,
    // and a tab can arrive meanwhile.
    if (event->button() == Qt::LeftButton) {
        pressPage_ = pageAt(tabAt(event->pos()));
        pressPos_ = event->pos();
        dragging_ = false;
    }
    QTabBar::mousePressEvent(event);
}

void DetachableTabBar::mouseMoveEvent(QMouseEvent *event)
{
    if (!(event->buttons() & Qt::LeftButton) || !pressPage_ || dragging_) {
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
    const int index = indexOfPage(pressPage_);
    if (index < 0)
        return;
    dragging_ = true;
    s_source = this;
    s_sourcePage = pressPage_;

    auto *drag = new QDrag(this);
    auto *mime = new QMimeData;
    mime->setData(QString::fromLatin1(kMime), QByteArray("1"));
    drag->setMimeData(mime);

    // A small drag pixmap from the dragged tab for visual feedback.
    const QRect tr = tabRect(index);
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

    // If nothing accepted the drop, detach into a new window at the cursor. The
    // drag ran its own event loop, so find the dragged tab again.
    const int at = result == Qt::IgnoreAction && s_source == this ? indexOfPage(s_sourcePage) : -1;
    if (at >= 0)
        emit detachRequested(at, QCursor::pos());

    s_source = nullptr;
    s_sourcePage = nullptr;
    dragging_ = false;
    pressPage_ = nullptr;
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
        const int sourceIndex = source->indexOfPage(s_sourcePage);
        dropIndicator_ = -1;
        update();
        event->setDropAction(Qt::MoveAction);
        event->accept();
        if (sourceIndex >= 0) // the dragged tab can have gone during the drag
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
