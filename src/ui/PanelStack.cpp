#include "ui/PanelStack.h"

#include <QEvent>
#include <QWidget>

#include <algorithm>

namespace mervin {

namespace {
constexpr int kMargin = 12; // gap from the viewport edges
constexpr int kGap = 8;     // vertical gap between stacked panels
} // namespace

PanelStack::PanelStack(QWidget *viewport, QObject *parent)
    : QObject(parent), viewport_(viewport)
{
    if (viewport_)
        viewport_->installEventFilter(this); // re-anchor / clamp on viewport resize
}

void PanelStack::addPanel(QWidget *panel)
{
    if (panel && !panels_.contains(panel))
        panels_.append(panel);
}

void PanelStack::anchorTopRight()
{
    if (viewport_)
        anchor_ = QPoint(viewport_->width() - kMargin, kMargin);
}

void PanelStack::relayout()
{
    if (!viewport_)
        return;
    // Default placement re-pins to the top-right (so the stack tracks a resizing
    // window); a user drag switches to a fixed anchor that we only clamp.
    if (!userMoved_)
        anchorTopRight();

    // Total height of the visible stack, to clamp the whole group into view.
    int totalH = 0;
    int count = 0;
    for (const QPointer<QWidget> &p : panels_) {
        if (!p || !p->isVisible())
            continue;
        p->adjustSize();
        totalH += p->height();
        ++count;
    }
    if (count > 0)
        totalH += (count - 1) * kGap;

    const int vw = viewport_->width();
    const int vh = viewport_->height();
    int x = anchor_.x(); // the stack's right edge
    int y = anchor_.y();
    // Keep the stack on-screen: clamp the anchor so the group fits (or, if it's
    // taller/wider than the viewport, pin to the top-left margin).
    int maxW = 0;
    for (const QPointer<QWidget> &p : panels_)
        if (p && p->isVisible())
            maxW = std::max(maxW, p->width());
    x = std::clamp(x, kMargin + maxW, std::max(kMargin + maxW, vw - kMargin));
    y = std::clamp(y, kMargin, std::max(kMargin, vh - totalH - kMargin));
    anchor_ = QPoint(x, y);

    // Right-aligned, so a panel keeps its place when a wider one opens above it.
    int cy = y;
    for (const QPointer<QWidget> &p : panels_) {
        if (!p || !p->isVisible())
            continue;
        p->move(x - p->width(), cy);
        p->raise();
        cy += p->height() + kGap;
    }
}

void PanelStack::nudge(const QPoint &delta)
{
    userMoved_ = true;
    anchor_ += delta;
    relayout();
}

bool PanelStack::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == viewport_ && event->type() == QEvent::Resize)
        relayout();
    return QObject::eventFilter(watched, event);
}

} // namespace mervin
