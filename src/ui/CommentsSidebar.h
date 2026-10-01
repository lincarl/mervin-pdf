#pragma once

#include "render/AnnotTypes.h"

#include <QWidget>

#include <vector>

class QListWidget;
class QLabel;
class QStackedLayout;

namespace mervin {

// Annotation list with page, author, colour and comment/kind. The window supplies data and
// routes annotationActivated(page, id) to navigation/editor actions.
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
