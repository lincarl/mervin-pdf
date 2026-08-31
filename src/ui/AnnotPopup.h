#pragma once

#include "render/AnnotTypes.h"

#include <QColor>
#include <QList>
#include <QWidget>

class QLabel;
class QPlainTextEdit;
class QToolButton;

namespace mervin {

// A small frameless editor that floats over a clicked annotation to read/edit its
// comment, recolour it, or delete it (peer of the form inline editors and the
// MeasurePanel - a child of the viewer's viewport). It shows the author/date,
// a multi-line comment field, the colour swatches, and a delete button. It edits
// whichever annotation the viewer most recently opened it for; it emits intent
// signals and the viewer routes them to the AnnotModel.
class AnnotPopup : public QWidget
{
    Q_OBJECT

public:
    explicit AnnotPopup(QWidget *parent = nullptr);

    // Populate from an annotation and show. The card is editable only when
    // `allowEdit` is true AND the annotation is one Mervin manages; otherwise it is
    // a read-only viewer (no colour swatches, no delete button, no text editing).
    // Used both for foreign, non-Mervin annotations and for any annotation opened
    // while the Comment tool is closed.
    void showFor(const Annotation &a, bool allowEdit = true);
    // Position the popup just below `annotWidgetRect` (viewport coords), clamped
    // into the parent. Called on open and on scroll/zoom reposition.
    void positionNear(const QRect &annotWidgetRect);
    // Commit any pending comment edit (emits commentEdited if it changed). Safe to
    // call repeatedly. Called before the viewer hides/replaces the popup.
    void commit();

    void focusComment();

signals:
    void commentEdited(const QString &text);
    void colorPicked(const QColor &color);
    void deleteRequested();
    void dismissed();

protected:
    void hideEvent(QHideEvent *event) override;

private:
    void refreshSwatchChecks(const QColor &color);

    QLabel *headerLabel_ = nullptr;
    QPlainTextEdit *comment_ = nullptr;
    QWidget *swatchRow_ = nullptr;
    QToolButton *deleteBtn_ = nullptr;
    QList<QToolButton *> swatches_;
    QList<QColor> swatchColors_;

    QString committedText_; // last value pushed via commentEdited (de-dupe)
    QRect anchorRect_;       // last annotation rect we positioned against
};

} // namespace mervin
