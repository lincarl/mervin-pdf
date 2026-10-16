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
        // Only the latest request can match the generation, so it describes the image.
        image_ = result.image;
        imageScale_ = result.scale;
        imageOrigin_ = requestedSource_.topLeft();
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
        image_ = converted_ = prepared_ = {};
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
    image_ = converted_ = prepared_ = {};
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
    if (sheet.isEmpty() || paper.isEmpty())
        return;

    // Colour previews need only screen detail. Black and white renders like printing,
    // at the print quality on paper and without antialiasing, before its threshold.
    // Thresholding screen-sized pixels would turn thin lines and small text white.
    // Like printing, the raster covers only the part of the page on the sheet.
    const bool monochrome = settings_.colourMode == printing::ColourMode::BlackAndWhite;
    const double qualityDpi = std::max(1, settings_.qualityDpi);
    const double screenDpi = 72.0 * sheet.width() / paper.width() * devicePixelRatioF();
    const double dpi = monochrome ? qualityDpi : std::min(screenDpi, qualityDpi);
    const QRectF printable = layout_.paintRect(QPageLayout::Point);
    printing::RasterPlan plan = printing::rasterPlan(pagePoints_, printable, settings_, dpi);
    // Bound either raster so huge sheets cannot exhaust memory in preview.
    const double limit = monochrome ? 4096.0 : 2048.0;
    const int longest = std::max(plan.source.width(), plan.source.height());
    if (longest > limit)
        plan = printing::rasterPlan(pagePoints_, printable, settings_, dpi * limit / longest);
    if (plan.isEmpty())
        return;
    if (qFuzzyCompare(plan.scale, requestedScale_) && plan.source == requestedSource_
        && monochrome == requestedMonochrome_)
        return;

    engine_->cancelRequests(requester_);
    requestedScale_ = plan.scale;
    requestedSource_ = plan.source;
    requestedMonochrome_ = monochrome;
    RenderRequest request;
    request.document = document_;
    request.requester = requester_;
    request.pageNo = page_;
    request.rotation = rotation_;
    request.scale = plan.scale;
    request.clip = plan.source;
    // Black and white thresholds colours without antialiasing, as printing does.
    request.antialias = !monochrome;
    request.epoch = ++generation_;
    engine_->submit(request);
}

const QImage &PrintPreviewWidget::preparedImage(const QSize &deviceSize)
{
    // Exposes and resizes repaint often. Convert only for a new render or colour
    // mode, and reduce only for a new conversion or device size.
    if (convertedKey_ != image_.cacheKey() || convertedMode_ != settings_.colourMode) {
        converted_ = printing::convertColour(image_, settings_.colourMode);
        convertedKey_ = image_.cacheKey();
        convertedMode_ = settings_.colourMode;
        ++conversions_;
    }
    if (preparedKey_ != converted_.cacheKey() || preparedSize_ != deviceSize) {
        prepared_ = printing::reduceToDevice(converted_, deviceSize);
        preparedKey_ = converted_.cacheKey();
        preparedSize_ = deviceSize;
    }
    return prepared_;
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
    // An image from earlier settings keeps its page area until its replacement arrives.
    const QRectF target = printing::rasterTarget(
        printing::destinationRect(pagePoints_, printable, settings_), pagePoints_, imageScale_,
        QRect(imageOrigin_, image_.size()));
    const QSize deviceSize = painter.deviceTransform().mapRect(target).size().toSize();
    printing::drawImage(painter, preparedImage(deviceSize), target, printable,
                        settings_.colourMode, true);
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
