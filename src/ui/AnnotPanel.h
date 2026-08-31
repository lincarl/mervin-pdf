#pragma once

#include "render/AnnotTypes.h"

#include <QPoint>
#include <QWidget>

class QButtonGroup;

namespace mervin {

class PanelStack;

// The floating "Comment" tool window shown over the page (peer of MeasurePanel),
// docked alongside it via a shared PanelStack. The two top buttons (Select /
// Comment, a vertical list) and the three markup-style buttons below (Highlight /
// Underline / Strike out) form one logical single-selection: picking a style is
// what arms the Markup sub-mode, so there is no separate "Highlight" mode button.
// While Select or Comment is active none of the style buttons are checked; while a
// style is active neither Select nor Comment is. New marks and notes take the
// default annotation colour (a Setting); each existing mark is recoloured
// individually from the swatches in its comment card, so the panel itself carries
// no colour picker. The active gesture is single and shared with the measuring
// tool, so the Select state also shows when Measure has taken the gesture over.
// Emits intent signals only; the viewer owns the state. Geometry / group-drag are
// owned by the PanelStack. (The Comment button maps to AnnotSubMode::Note; the
// style buttons map to AnnotSubMode::Markup with the chosen AnnotType.)
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
