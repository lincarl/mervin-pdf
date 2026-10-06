#pragma once

#include <QObject>
#include <QPoint>
#include <QPointer>
#include <QVector>

class QWidget;

namespace mervin {

// Own floating tool-panel geometry without reparenting them. Visible panels stack in
// registration order near the viewport's top-right. nudge() moves the group and preserves that
// anchor on resize; all positions remain clamped to the viewport. Panels call relayout() after
// visibility/size changes; viewport resizes are watched here.
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
