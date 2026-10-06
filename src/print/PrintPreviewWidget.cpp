#include "print/PrintPreviewWidget.h"

#include "render/Document.h"
#include "render/RenderEngine.h"
#include "ui/ThemeTokens.h"

#include <QPainter>
#include <QResizeEvent>

#include <algorithm>
#include <cmath>

namespace mervin {

namespace {

// Widgets are created on the UI thread. Keep preview IDs separate from viewer
// IDs and never reuse one when a closed dialog's worker finishes late.
quint64 nextPreviewId()
{
    static quint64 id = quint64(1) << 63;
    return ++id;
}

} // namespace

PrintPreviewWidget::PrintPreviewWidget(RenderEngine *engine, Document *document, int rotation,
                                       QWidget *parent)
    : QWidget(parent), engine_(engine), document_(document), rotation_(rotation)
    , requester_(nextPreviewId())
{
    setMinimumSize(240, 220);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    setAccessibleName(tr("Print preview"));
    renderTimer_.setSingleShot(true);
    renderTimer_.setInterval(100);
    connect(&renderTimer_, &QTimer::timeout, this, &PrintPreviewWidget::requestImage);
    connect(engine_, &RenderEngine::resultReady, this, [this](const RenderResult &result) {
        if (result.requester != requester_ || result.epoch != generation_ || result.pageNo != page_)
            return;
        image_ = result.image;
        message_ = result.ok && !image_.isNull() ? QString() : tr("Could not render this page.");
        update();
        emit previewReady();
    });
}

PrintPreviewWidget::~PrintPreviewWidget()
{
    engine_->cancelRequests(requester_);
}

void PrintPreviewWidget::setPage(int zeroBasedPage, const QPageLayout &layout,
                               const printing::Settings &settings)
{
    if (!document_ || zeroBasedPage < 0 || zeroBasedPage >= document_->pageCount()
        || !layout.isValid()) {
        clear(tr("No page to preview."));
        return;
    }
    const bool newPage = page_ != zeroBasedPage;
    if (newPage) {
        engine_->cancelRequests(requester_);
        ++generation_;
        image_ = {};
        requestedScale_ = 0;
        message_ = tr("Loading preview…");
    }
    page_ = zeroBasedPage;
    pagePoints_ = document_->pageSize(page_);
    if (rotation_ == 90 || rotation_ == 270)
        pagePoints_.transpose();
    layout_ = layout;
    settings_ = settings;
    if (!image_.isNull())
        message_.clear();
    renderTimer_.start();
    update();
}

void PrintPreviewWidget::clear(const QString &message)
{
    renderTimer_.stop();
    engine_->cancelRequests(requester_);
    ++generation_;
    page_ = -1;
    image_ = {};
    requestedScale_ = 0;
    message_ = message;
    update();
}

QRectF PrintPreviewWidget::sheetRect() const
{
    const QSizeF paper = layout_.fullRect(QPageLayout::Point).size();
    const QRectF available = QRectF(rect()).adjusted(20, 20, -20, -20);
    if (paper.isEmpty() || available.isEmpty())
        return {};
    const QSizeF fitted = paper.scaled(available.size(), Qt::KeepAspectRatio);
    return {available.center() - QPointF(fitted.width() / 2, fitted.height() / 2), fitted};
}

void PrintPreviewWidget::requestImage()
{
    if (page_ < 0 || pagePoints_.isEmpty())
        return;
    const QRectF sheet = sheetRect();
    const QRectF paper = layout_.fullRect(QPageLayout::Point);
    const QRectF destination = printing::destinationRect(
        pagePoints_, layout_.paintRect(QPageLayout::Point), settings_);
    if (sheet.isEmpty() || destination.isEmpty())
        return;

    // Colour previews need only screen detail. Monochrome must threshold a
    // higher-resolution source before fitting it, otherwise thin lines and
    // small text turn white solely because their screen pixels are antialiased.
    // Bound either raster so huge drawings cannot exhaust memory in preview.
    const double screenScale = sheet.width() / paper.width() * devicePixelRatioF()
                               * destination.width() / pagePoints_.width();
    const bool monochrome = settings_.colourMode == printing::ColourMode::BlackAndWhite;
    const double qualityScale = std::max(1, settings_.qualityDpi) / 72.0;
    const double scale = std::min({monochrome ? qualityScale : screenScale, qualityScale,
                                  (monochrome ? 4096.0 : 2048.0)
                                      / std::max(pagePoints_.width(), pagePoints_.height())});
    if (scale <= 0 || !std::isfinite(scale))
        return;
    if (qFuzzyCompare(scale, requestedScale_))
        return;

    engine_->cancelRequests(requester_);
    requestedScale_ = scale;
    RenderRequest request;
    request.document = document_;
    request.requester = requester_;
    request.pageNo = page_;
    request.rotation = rotation_;
    request.scale = scale;
    request.epoch = ++generation_;
    engine_->submit(request);
}

void PrintPreviewWidget::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    const auto colours = theme::chrome(palette());
    painter.fillRect(rect(), colours.well);
    if (page_ < 0 || !message_.isEmpty()) {
        painter.setPen(colours.inkBody);
        painter.drawText(rect().adjusted(20, 20, -20, -20),
                         Qt::AlignCenter | Qt::TextWordWrap, message_);
        return;
    }
    const QRectF sheet = sheetRect();
    if (sheet.isEmpty())
        return;
    painter.fillRect(sheet, Qt::white);
    painter.save();
    painter.translate(sheet.topLeft());
    const double scale = sheet.width() / layout_.fullRect(QPageLayout::Point).width();
    painter.scale(scale, scale);
    const QRectF printable = layout_.paintRect(QPageLayout::Point);
    printing::paintPage(painter, image_, pagePoints_, printable, settings_, true);
    QPen marginPen(QColor(160, 164, 170), 1, Qt::DashLine);
    marginPen.setCosmetic(true);
    painter.setPen(marginPen);
    painter.setBrush(Qt::NoBrush);
    painter.drawRect(printable);
    painter.restore();
}

void PrintPreviewWidget::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    if (page_ >= 0)
        renderTimer_.start();
}

} // namespace mervin
