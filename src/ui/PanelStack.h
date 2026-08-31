#pragma once

#include <QObject>
#include <QPoint>
#include <QPointer>
#include <QVector>

class QWidget;

namespace mervin {

// Coordinates a vertical "dock" of floating tool panels (MeasurePanel, the
// Comment panel) that overlap the viewer's viewport. The panels stay independent
// children of the viewport (no reparenting), but PanelStack owns their geometry:
//
//   - Visible panels are stacked top-to-bottom in registration order, anchored
//     near the viewport's top-right by default. Opening a second panel places it
//     directly below the first; hiding one reflows the rest up. Works for any
//     show/hide order.
//   - The whole stack drags as a group: a panel reports a drag delta via nudge()
//     and the entire stack moves together. Once dragged, the stack keeps the
//     user's anchor (it no longer re-pins to the top-right on resize), but is
//     always clamped back into view.
//
// Panels call relayout() whenever they show/hide or change size, and nudge() while
// being dragged. PanelStack watches the viewport for resize to re-anchor/clamp.
class PanelStack : public QObject
{
    Q_OBJECT

public:
    explicit PanelStack(QWidget *viewport, QObject *parent = nullptr);

    // Register a panel in stacking order (top first). The panel must be a child of
    // the viewport. Registering does not show it; visibility is the panel's own.
    void addPanel(QWidget *panel);

    // Reposition every visible panel from the current anchor. Safe to call often.
    void relayout();

    // Drag the whole stack by `delta` (viewport pixels); pins the user anchor.
    void nudge(const QPoint &delta);

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void anchorTopRight(); // recompute the default top-right anchor (widest panel)
    QVector<QPointer<QWidget>> panels_;
    QPointer<QWidget> viewport_;
    QPoint anchor_;          // top-left of the stack, in viewport coordinates
    bool userMoved_ = false; // once dragged, stop re-pinning to the top-right
};

} // namespace mervin
