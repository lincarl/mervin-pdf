#include "dialogs/ExtractStrip.h"

#include "render/Document.h"
#include "render/RenderEngine.h"
#include "ui/Icons.h"
#include "ui/ThemeTokens.h"

#include <QAbstractListModel>
#include <QCursor>
#include <QHelpEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QScrollBar>
#include <QStyledItemDelegate>
#include <QTimer>
#include <QToolTip>

#include <utility>

namespace mervin {

namespace {
// Strip geometry. Heights are in item coordinates: the viewport is 168 px tall,
// its bottom 16 px the scrollbar's slot.
constexpr int kOuterHeight = 174;      // 168 viewport + 1 px border + 2 px padding, each side
constexpr int kItemHeight = 152;       // the viewport minus the scrollbar slot
constexpr int kThumbTop = 8;
constexpr int kThumbH = 124;
constexpr int kThumbW = 89;            // 124 x 0.72, a portrait page
constexpr int kCaptionTop = 136;       // kThumbTop + kThumbH + 4
constexpr int kBandH = 16;             // the caption row
constexpr int kWashTop = 4;            // hover / current highlight...
constexpr int kWashBottom = 156;       // ...running 4 px into the scrollbar slot
constexpr int kFoldTileW = 40;
constexpr int kFoldGlyph = 24;         // FileText in the fold tile; glyph() paints 24 natively
constexpr int kCloseSize = 16;         // the ✕ hit square; the glyph is 14 px inside it
constexpr int kPad = 6;                // thumbnail inset inside a cell
constexpr int kRenderDebounceMs = 150; // typing 4, 40, 400 renders nothing in between

// `shape` scaled to fit inside `box`, centred. The whole box when shape is empty.
QRect fitted(const QRect &box, const QSizeF &shape)
{
    if (shape.isEmpty())
        return box;
    QRect r(QPoint(), shape.scaled(QSizeF(box.size()), Qt::KeepAspectRatio).toSize());
    r.moveCenter(box.center());
    return r;
}

QRectF halfPixel(const QRect &r)
{
    return QRectF(r).adjusted(0.5, 0.5, -0.5, -0.5); // crisp 1 px antialiased outlines
}
} // namespace

// The cells as a flat list model. Rebuilt wholesale by setContent(), like
// MergeDialog's rows, so a row index can never outlive the plan it came from.
class ExtractCellModel : public QAbstractListModel
{
public:
    using QAbstractListModel::QAbstractListModel;

    void reset(const ExtractStrip::Content &content, int itemHeight, int trailing)
    {
        beginResetModel();
        content_ = content;
        itemHeight_ = itemHeight;
        trailing_ = trailing;
        endResetModel();
    }
    const ExtractStrip::Content &content() const { return content_; }

    int rowCount(const QModelIndex &parent = QModelIndex()) const override
    {
        return parent.isValid() ? 0 : int(content_.cells.size());
    }

    QVariant data(const QModelIndex &index, int role) const override
    {
        const int row = index.row();
        if (!index.isValid() || row >= content_.cells.size())
            return {};
        const ExtractPlan::Cell &c = content_.cells.at(row);
        switch (role) {
        case Qt::DisplayRole:
            return content_.captions.value(row);
        case Qt::ToolTipRole:
            return content_.toolTips.value(row);
        case Qt::SizeHintRole: {
            // The last cell also carries the lead kept free after it.
            const bool last = row == content_.cells.size() - 1;
            return QSize(c.width + (last ? trailing_ : 0), itemHeight_);
        }
        case ExtractStrip::KindRole:
            return int(c.kind);
        default:
            return {};
        }
    }

private:
    ExtractStrip::Content content_;
    int itemHeight_ = 0;
    int trailing_ = 0;
};

// Hands every item to ExtractStrip::paintCell, which knows the bands, the hover
// state and the thumbnail cache.
class ExtractCellDelegate : public QStyledItemDelegate
{
public:
    explicit ExtractCellDelegate(ExtractStrip *strip)
        : QStyledItemDelegate(strip)
        , strip_(strip)
    {
    }
    void paint(QPainter *p, const QStyleOptionViewItem &, const QModelIndex &index) const override
    {
        strip_->paintCell(p, index.row());
    }

private:
    ExtractStrip *strip_;
};

ExtractStrip::ExtractStrip(QWidget *parent)
    : QListView(parent)
    , model_(new ExtractCellModel(this))
{
    setModel(model_);
    setItemDelegate(new ExtractCellDelegate(this));
    setViewMode(QListView::ListMode);
    setFlow(QListView::LeftToRight);
    setWrapping(false);
    setMovement(QListView::Static);
    setUniformItemSizes(false);
    setSpacing(0);
    setHorizontalScrollMode(QAbstractItemView::ScrollPerPixel);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setSelectionMode(QAbstractItemView::SingleSelection); // "which cell is current", never the plan
    setEditTriggers(QAbstractItemView::NoEditTriggers);
    setContextMenuPolicy(Qt::ActionsContextMenu); // the dialog adds the actions
    setFixedHeight(kOuterHeight);
    viewport()->setMouseTracking(true); // hover wash and the ✕

    // The only way pump() runs, so there is never more than one render pending.
    renderTimer_ = new QTimer(this);
    renderTimer_->setSingleShot(true);
    connect(renderTimer_, &QTimer::timeout, this, &ExtractStrip::pump);
    // A scroll brings new cells into view and slides others under a still pointer.
    connect(horizontalScrollBar(), &QScrollBar::valueChanged, this, [this] {
        syncHoverToCursor();
        renderTimer_->start(kRenderDebounceMs);
    });
}

void ExtractStrip::setSource(Document *doc, RenderEngine *engine)
{
    doc_ = doc;
    engine_ = engine;
    cache_.clear();
    renderTimer_->start(kRenderDebounceMs);
}

ExtractPlan::Metrics ExtractStrip::metrics()
{
    ExtractPlan::Metrics m;
    m.page = kThumbW + 2 * kPad;
    m.fold = kFoldTileW + 2 * kPad;
    return m;
}

void ExtractStrip::setContent(const Content &content, int currentFlat)
{
    settingContent_ = true;
    model_->reset(content, kItemHeight, metrics().lead);
    int row = int(content.cells.size()) - 1;
    for (int i = 0; i < content.cells.size(); ++i) {
        if (content.cells.at(i).lastFlat >= currentFlat) {
            row = i;
            break;
        }
    }
    // Without autoscroll: a refill keeps the user's scroll position, and the
    // dialog reveals what it wants shown (ensureRowVisible).
    const bool autoScroll = hasAutoScroll();
    setAutoScroll(false);
    if (row >= 0)
        setCurrentIndex(model_->index(row));
    setAutoScroll(autoScroll);
    settingContent_ = false;
    // The cell under the pointer may have changed (a ✕ click slides the next cell
    // under it).
    syncHoverToCursor();
    viewport()->update();
    emit currentCellChanged();
    renderTimer_->start(kRenderDebounceMs);
}

const ExtractPlan::Cell *ExtractStrip::currentCell() const
{
    const int row = currentIndex().row();
    const QList<ExtractPlan::Cell> &cells = model_->content().cells;
    return row >= 0 && row < cells.size() ? &cells.at(row) : nullptr;
}

void ExtractStrip::setHighlightedRow(int row)
{
    if (row != highlightedRow_) {
        highlightedRow_ = row;
        viewport()->update();
    }
}

void ExtractStrip::ensureRowVisible(int row)
{
    const QList<ExtractPlan::Cell> &cells = model_->content().cells;
    int first = -1;
    int last = -1;
    for (int i = 0; i < cells.size(); ++i) {
        if (cells.at(i).row != row)
            continue;
        if (visualRect(model_->index(i)).intersects(viewport()->rect()))
            return; // already in view: leave the user's scroll position alone
        first = first < 0 ? i : first;
        last = i;
    }
    // Last cell first, then the first: scrollTo moves only as far as it must, so
    // the second call is a no-op unless the row is wider than the viewport.
    for (const int i : {last, first})
        if (i >= 0)
            scrollTo(model_->index(i));
}

QRect ExtractStrip::contentRect(int row) const
{
    const ExtractPlan::Cell &c = model_->content().cells.at(row);
    const QRect r = visualRect(model_->index(row));
    return QRect(r.left() + c.gapBefore, r.top() + kWashTop, c.width - c.gapBefore,
                 kWashBottom - kWashTop);
}

int ExtractStrip::cellAt(const QPoint &pos) const
{
    const int row = indexAt(pos).row();
    return row >= 0 && contentRect(row).contains(pos) ? row : -1; // a gap is not the cell
}

QRect ExtractStrip::closeRect(int row) const
{
    // Right end of the caption row, level with the thumbnail's right edge: on the
    // dark well, not on the paper.
    const QRect cr = contentRect(row);
    const int top = visualRect(model_->index(row)).top() + kCaptionTop;
    return QRect(cr.right() - kPad - kCloseSize + 1, top, kCloseSize, kCloseSize);
}

void ExtractStrip::syncHoverToCursor()
{
    const int old = std::exchange(hoverRow_, -1);
    if (viewport()->underMouse()) {
        hoverPos_ = viewport()->mapFromGlobal(QCursor::pos());
        hoverRow_ = cellAt(hoverPos_);
    }
    updateRow(old);
    updateRow(hoverRow_);
}

void ExtractStrip::updateRow(int row)
{
    if (row < 0 || row >= model_->content().cells.size())
        return;
    // The wash runs 4 px past the item's bottom edge, into the scrollbar slot.
    viewport()->update(visualRect(model_->index(row)).united(contentRect(row)));
}

void ExtractStrip::paintCell(QPainter *p, int row) const
{
    const Content &content = model_->content();
    if (row < 0 || row >= content.cells.size())
        return;
    const ExtractPlan::Cell &c = content.cells.at(row);
    const theme::Chrome t = theme::chrome(palette());
    const QRect cr = contentRect(row);
    const int top = visualRect(model_->index(row)).top();
    const QRect box(cr.left() + kPad, top + kThumbTop, kThumbW, kThumbH);
    const QRect close = closeRect(row);
    const bool hovered = row == hoverRow_;

    p->save();
    p->setRenderHint(QPainter::Antialiasing);
    if (hasFocus() && currentIndex().row() == row) {
        p->setPen(t.accent);
        p->setBrush(t.accentWash);
        p->drawRoundedRect(halfPixel(cr), 4, 4);
    } else if (hovered || c.row == highlightedRow_) {
        p->setPen(Qt::NoPen);
        p->setBrush(hovered ? t.hover : t.accentWash);
        p->drawRoundedRect(cr, 4, 4);
    }
    p->setBrush(Qt::NoBrush);

    // While hovered the caption keeps clear of the ✕: symmetrically on a page or
    // bad cell so the number stays centred, to its left on a narrow fold.
    QRect captionR(cr.left() + 2, top + kCaptionTop, cr.width() - 4, kBandH);
    if (hovered) {
        if (c.kind == ExtractPlan::Cell::Kind::Fold)
            captionR.setRight(close.left() - 1);
        else
            captionR.adjust(kCloseSize, 0, -kCloseSize, 0);
    }
    QColor captionInk = t.inkBody;

    switch (c.kind) {
    case ExtractPlan::Cell::Kind::Page: {
        const QPixmap pm = cache_.get(c.page);
        if (!pm.isNull()) {
            p->setRenderHint(QPainter::SmoothPixmapTransform);
            p->drawPixmap(fitted(box, pm.size()), pm);
        } else {
            // Pending: the paper colour at 35%, which reads as "not here yet"
            // rather than as a blank page. No spinner.
            p->setOpacity(0.35);
            p->fillRect(fitted(box, doc_ ? doc_->pageSize(c.page) : QSizeF()),
                        theme::doc().paperNormal);
            p->setOpacity(1.0);
        }
        break;
    }
    case ExtractPlan::Cell::Kind::Fold: {
        // The pages left out, as a glyph; the caption and tooltip say which.
        const QRect tile(cr.left() + (cr.width() - kFoldTileW) / 2, box.top(), kFoldTileW,
                         kThumbH);
        p->setPen(t.borderStrong);
        p->drawRoundedRect(halfPixel(tile), 3, 3);
        QRect glyph(0, 0, kFoldGlyph, kFoldGlyph);
        glyph.moveCenter(tile.center());
        // Built once per ink, not per paint: glyph() renders five pixmaps.
        if (foldGlyphInk_ != t.inkBody) {
            foldGlyphInk_ = t.inkBody;
            foldGlyph_ = icons::glyph(icons::Glyph::FileText, t.inkBody);
        }
        foldGlyph_.paint(p, glyph);
        captionInk = t.inkSoft;
        break;
    }
    case ExtractPlan::Cell::Kind::Bad: {
        p->setPen(t.inkDanger);
        p->drawRoundedRect(halfPixel(box), 2, 2);
        QFont mark = font();
        mark.setPixelSize(15);
        mark.setWeight(QFont::DemiBold);
        p->setFont(mark);
        p->drawText(box, Qt::AlignCenter, QStringLiteral("?"));
        captionInk = t.inkDanger;
        break;
    }
    }

    p->setFont(font());
    p->setPen(captionInk);
    p->drawText(captionR, Qt::AlignCenter,
                p->fontMetrics().elidedText(content.captions.value(row), Qt::ElideRight,
                                            captionR.width()));

    if (hovered) {
        const bool onClose = close.contains(hoverPos_);
        if (onClose) {
            p->setPen(Qt::NoPen);
            p->setBrush(t.hover);
            p->drawRoundedRect(close, 3, 3);
        }
        icons::glyph(icons::Glyph::Close, onClose ? t.inkPrimary : t.inkSoft)
            .paint(p, close.adjusted(1, 1, -1, -1));
    }
    p->restore();
}

bool ExtractStrip::event(QEvent *event)
{
    // Moving to a screen with another scale keeps the logical size, so no resize
    // restarts the pump; its 10% growth check then re-renders at the new ratio.
    if (renderTimer_
        && (event->type() == QEvent::DevicePixelRatioChange
            || event->type() == QEvent::ScreenChangeInternal))
        renderTimer_->start(kRenderDebounceMs);
    return QListView::event(event);
}

void ExtractStrip::resizeEvent(QResizeEvent *event)
{
    QListView::resizeEvent(event);
    // The viewport's width with no scrollbar showing, so the strip does not refold
    // when the horizontal scrollbar appears in its reserved slot.
    const int inner = maximumViewportSize().width();
    if (inner == innerWidth_)
        return;
    innerWidth_ = inner;
    renderTimer_->start(kRenderDebounceMs);
    emit viewportResized();
}

bool ExtractStrip::viewportEvent(QEvent *event)
{
    switch (event->type()) {
    case QEvent::Leave: {
        const int old = std::exchange(hoverRow_, -1);
        updateRow(old);
        break;
    }
    case QEvent::ToolTip: {
        auto *he = static_cast<QHelpEvent *>(event);
        const int row = indexAt(he->pos()).row();
        if (row >= 0 && row == hoverRow_ && closeRect(row).contains(he->pos())) {
            //: Tooltip of the close button on a page in the strip: takes it out of
            //: the extract.
            QToolTip::showText(he->globalPos(), tr("Remove"), viewport(), closeRect(row));
            return true;
        }
        break;
    }
    default:
        break;
    }
    return QListView::viewportEvent(event);
}

void ExtractStrip::mouseMoveEvent(QMouseEvent *event)
{
    QListView::mouseMoveEvent(event);
    const QPoint pos = event->position().toPoint();
    const int row = cellAt(pos);
    const bool wasOnClose = hoverRow_ >= 0 && closeRect(hoverRow_).contains(hoverPos_);
    hoverPos_ = pos;
    if (row != hoverRow_) {
        const int old = std::exchange(hoverRow_, row);
        updateRow(old);
        updateRow(row);
    } else if (row >= 0 && wasOnClose != closeRect(row).contains(pos)) {
        updateRow(row);
    }
}

void ExtractStrip::mousePressEvent(QMouseEvent *event)
{
    const QPoint pos = event->position().toPoint();
    pressRow_ = event->button() == Qt::LeftButton ? cellAt(pos) : -1;
    pressOnClose_ = pressRow_ >= 0 && closeRect(pressRow_).contains(pos);
    QListView::mousePressEvent(event); // selects, for the right button too
}

void ExtractStrip::mouseReleaseEvent(QMouseEvent *event)
{
    QListView::mouseReleaseEvent(event);
    const int row = std::exchange(pressRow_, -1);
    if (event->button() != Qt::LeftButton || row < 0 || row != cellAt(event->position().toPoint()))
        return;
    const ExtractPlan::Cell c = model_->content().cells.at(row);
    // Queued: the dialog rebuilds the model in response, which must not happen
    // inside the view's own mouse handling.
    if (pressOnClose_ && closeRect(row).contains(event->position().toPoint())) {
        QMetaObject::invokeMethod(this, [this, c] { emit removeClicked(c); }, Qt::QueuedConnection);
        return;
    }
    emit rowPicked(c.row); // a click on the current cell changes nothing else
    if (c.kind == ExtractPlan::Cell::Kind::Fold)
        QMetaObject::invokeMethod(
            this, [this, c] { emit foldActivated(c.run); }, Qt::QueuedConnection);
}

void ExtractStrip::focusInEvent(QFocusEvent *event)
{
    QListView::focusInEvent(event);
    viewport()->update(); // the current-cell frame shows only while focused
}

void ExtractStrip::focusOutEvent(QFocusEvent *event)
{
    QListView::focusOutEvent(event);
    viewport()->update();
}

void ExtractStrip::currentChanged(const QModelIndex &current, const QModelIndex &previous)
{
    QListView::currentChanged(current, previous);
    updateRow(previous.row());
    updateRow(current.row());
    emit currentCellChanged();
    if (!settingContent_) {
        if (const ExtractPlan::Cell *c = currentCell())
            emit rowPicked(c->row);
    }
}

void ExtractStrip::pump()
{
    if (!doc_ || !engine_)
        return;
    const QList<ExtractPlan::Cell> &cells = model_->content().cells;
    const QRect vp = viewport()->rect();
    const qreal dpr = devicePixelRatioF();
    for (int row = 0; row < cells.size(); ++row) {
        const ExtractPlan::Cell &c = cells.at(row);
        if (c.kind != ExtractPlan::Cell::Kind::Page
            || !visualRect(model_->index(row)).intersects(vp))
            continue;
        const QSizeF pts = doc_->pageSize(c.page);
        if (pts.isEmpty())
            continue;
        const double scale = qMin(kThumbW / pts.width(), kThumbH / pts.height()) * dpr;
        // A cached render drawn smaller is fine; only growth past 10% re-renders.
        if (cache_.contains(c.page) && cache_.get(c.page).width() >= 0.9 * pts.width() * scale)
            continue;
        const QImage img = engine_->renderPageImage(doc_, c.page, scale, 0);
        if (img.isNull())
            continue;
        cache_.put(c.page, QPixmap::fromImage(img));
        updateRow(row);
        // One page per event-loop turn, so the worst stall is a single render. A
        // scroll, resize or edit restarts the debounce, which pauses the chain.
        renderTimer_->start(0);
        return;
    }
}

} // namespace mervin
