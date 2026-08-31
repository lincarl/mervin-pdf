#pragma once

#include "render/AnnotTypes.h"

#include <QWidget>

#include <vector>

class QListWidget;
class QLabel;
class QStackedLayout;

namespace mervin {

// Collapsible panel listing every annotation in the document (peer of
// OutlineSidebar / ThumbnailSidebar). Each row shows the page, author, a colour
// chip, and the comment text (or the markup kind when there is no comment).
// Activating a row emits annotationActivated with the annotation's (page, id) so
// the window can jump to it and open its inline editor. A pure view - the window
// supplies the annotations and routes navigation.
class CommentsSidebar : public QWidget
{
    Q_OBJECT

public:
    explicit CommentsSidebar(QWidget *parent = nullptr);

    void setAnnotations(const std::vector<Annotation> &annots);

signals:
    void annotationActivated(int page, int id);

private:
    QListWidget *list_ = nullptr;
    QLabel *empty_ = nullptr;
    QStackedLayout *stack_ = nullptr;
};

} // namespace mervin
