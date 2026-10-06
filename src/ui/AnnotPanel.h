#pragma once

#include "render/AnnotTypes.h"

#include <QPoint>
#include <QWidget>

class QButtonGroup;

namespace mervin {

class PanelStack;

// Comment panel sharing a PanelStack with MeasurePanel. Select, Note and markup styles form one
// exclusive gesture choice; selecting a style arms Markup. Measure taking the gesture restores
// Select. New marks use the configured colour; existing marks are recoloured in their cards.
// Emits intent only: the viewer owns state, PanelStack owns geometry and dragging.
class AnnotPanel : public QWidget
{
    Q_OBJECT

public:
    explicit AnnotPanel(QWidget *parent = nullptr);

    void setStack(PanelStack *stack) { stack_ = stack; }

    // Reflect state without emitting: seed from settings, and sync the mode to
    // Select when another tool (Measure) takes over the single active gesture.
    void setMode(AnnotSubMode mode);
    void setHighlightStyle(AnnotType type);

signals:
    void modeChanged(AnnotSubMode mode);
    void highlightStyleChanged(AnnotType type);
    void closeRequested();

protected:
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void showEvent(QShowEvent *event) override;

private:
    QButtonGroup *modeGroup_ = nullptr;  // Select / Comment (none checked in Markup)
    QButtonGroup *styleGroup_ = nullptr; // Highlight / Underline / Strike out

    // Reflected viewer state: the sub-mode the panel currently shows, and the
    // markup style to re-check when Markup is (re-)entered from Select/Comment.
    AnnotSubMode currentMode_ = AnnotSubMode::Markup;
    AnnotType activeStyle_ = AnnotType::Highlight;

    PanelStack *stack_ = nullptr; // owns geometry (non-owning back-ptr)
    bool dragging_ = false;
    QPoint dragLastGlobal_;
};

} // namespace mervin
