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
    // How many times painting converted a render's colour mode, and how many times it
    // reduced a conversion to the device size, for tests.
    int conversions() const { return conversions_; }
    int reductions() const { return reductions_; }

signals:
    void previewReady();

protected:
    void paintEvent(QPaintEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;

private:
    QRectF sheetRect() const;
    void requestImage();
    const QImage &preparedImage(const QSize &deviceSize);

    RenderEngine *engine_;
    Document *document_;
    int rotation_;
    quint64 requester_;
    quint64 generation_ = 0;
    int page_ = -1;
    QSizeF pagePoints_;
    QPageLayout layout_;
    printing::Settings settings_;
    QImage image_;       // pixels of the page's full raster at imageScale_
    double imageScale_ = 0;
    QPoint imageOrigin_; // image_'s top-left pixel in that full raster
    QString message_;
    double requestedScale_ = 0;
    QRect requestedSource_;
    bool requestedMonochrome_ = false;
    QTimer renderTimer_;
    // The prepare step's results for image_, kept between paints: the colour
    // conversion, then its reduction to the device size.
    QImage converted_;
    qint64 convertedKey_ = 0;
    printing::ColourMode convertedMode_ = printing::ColourMode::Colour;
    QImage prepared_;
    qint64 preparedKey_ = 0;
    QSize preparedSize_;
    int conversions_ = 0;
    int reductions_ = 0;
};

} // namespace mervin
