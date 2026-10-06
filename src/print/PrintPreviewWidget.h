#pragma once

#include "print/PrintLayout.h"

#include <QImage>
#include <QPageLayout>
#include <QTimer>
#include <QWidget>

namespace mervin {

class Document;
class RenderEngine;

// Shows one output sheet fitted to the widget. The caller keeps the engine and
// print document alive until this widget is destroyed. Rendering uses the
// engine's worker pool and ignores results from superseded requests.
class PrintPreviewWidget : public QWidget
{
    Q_OBJECT

public:
    PrintPreviewWidget(RenderEngine *engine, Document *document, int rotation,
                       QWidget *parent = nullptr);
    ~PrintPreviewWidget() override;

    void setPage(int zeroBasedPage, const QPageLayout &layout,
                 const printing::Settings &settings);
    void clear(const QString &message);
    bool isReady() const { return !image_.isNull() && message_.isEmpty(); }

signals:
    void previewReady();

protected:
    void paintEvent(QPaintEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;

private:
    QRectF sheetRect() const;
    void requestImage();

    RenderEngine *engine_;
    Document *document_;
    int rotation_;
    quint64 requester_;
    quint64 generation_ = 0;
    int page_ = -1;
    QSizeF pagePoints_;
    QPageLayout layout_;
    printing::Settings settings_;
    QImage image_;
    QString message_;
    double requestedScale_ = 0;
    QTimer renderTimer_;
};

} // namespace mervin
