#pragma once

#include "render/ThumbnailCache.h"

#include <QWidget>

class QListWidget;
class QTimer;

namespace mervin {

class RenderEngine;
class Document;

// Collapsible page-thumbnails panel. Thumbnails are rendered lazily (only for
// rows scrolled into view) via the shared RenderEngine and cached, so even a
// thousand-page document opens instantly. Activating a thumbnail emits
// pageSelected; the window keeps the highlighted row in sync with the viewer.
class ThumbnailSidebar : public QWidget
{
    Q_OBJECT

public:
    explicit ThumbnailSidebar(RenderEngine *engine, QWidget *parent = nullptr);

    void setDocument(Document *doc); // rebuild for a new document (or clear)
    void setCurrentPage(int page);   // highlight without emitting

signals:
    void pageSelected(int page);

private slots:
    void renderVisible(); // render thumbnails for currently-visible rows

private:
    RenderEngine *engine_ = nullptr;
    Document *doc_ = nullptr;
    QListWidget *list_ = nullptr;
    QTimer *renderTimer_ = nullptr;
    ThumbnailCache cache_;
    bool syncingSelection_ = false;
};

} // namespace mervin
