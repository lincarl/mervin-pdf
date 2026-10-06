#include "ui/ThumbnailSidebar.h"

#include "render/Document.h"
#include "render/RenderEngine.h"

#include <QListWidget>
#include <QScrollBar>
#include <QTimer>
#include <QVBoxLayout>

namespace mervin {

namespace {
constexpr int kThumbW = 140; // device pixels
constexpr int kPageRole = Qt::UserRole;
} // namespace

ThumbnailSidebar::ThumbnailSidebar(RenderEngine *engine, QWidget *parent)
    : QWidget(parent)
    , engine_(engine)
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);

    list_ = new QListWidget(this);
    list_->setViewMode(QListView::IconMode);
    list_->setFlow(QListView::TopToBottom);
    list_->setWrapping(false);
    list_->setResizeMode(QListView::Adjust);
    list_->setMovement(QListView::Static);
    list_->setIconSize(QSize(kThumbW, kThumbW * 4 / 3));
    list_->setUniformItemSizes(true);
    list_->setSpacing(4);
    layout->addWidget(list_);

    renderTimer_ = new QTimer(this);
    renderTimer_->setSingleShot(true);
    renderTimer_->setInterval(20);
    connect(renderTimer_, &QTimer::timeout, this, &ThumbnailSidebar::renderVisible);

    connect(list_->verticalScrollBar(), &QScrollBar::valueChanged, this,
            [this] { renderTimer_->start(); });
    connect(list_, &QListWidget::itemClicked, this, [this](QListWidgetItem *item) {
        if (syncingSelection_)
            return;
        emit pageSelected(item->data(kPageRole).toInt());
    });
}

void ThumbnailSidebar::setDocument(Document *doc)
{
    doc_ = doc;
    cache_.clear();
    list_->clear();
    if (!doc_)
        return;

    const int n = doc_->pageCount();
    for (int i = 0; i < n; ++i) {
        auto *item = new QListWidgetItem(QString::number(i + 1), list_);
        item->setData(kPageRole, i);
        item->setSizeHint(QSize(kThumbW + 16, kThumbW * 4 / 3 + 24));
        item->setTextAlignment(Qt::AlignHCenter | Qt::AlignBottom);
    }
    renderTimer_->start();
}

void ThumbnailSidebar::setCurrentPage(int page)
{
    if (page < 0 || page >= list_->count())
        return;
    syncingSelection_ = true;
    list_->setCurrentRow(page);
    list_->scrollToItem(list_->item(page), QAbstractItemView::PositionAtCenter);
    syncingSelection_ = false;
    renderTimer_->start();
}

void ThumbnailSidebar::renderVisible()
{
    if (!doc_ || !engine_)
        return;
    const QRect vp = list_->viewport()->rect();
    for (int i = 0; i < list_->count(); ++i) {
        QListWidgetItem *item = list_->item(i);
        if (!item->icon().isNull())
            continue; // already rendered
        const QRect r = list_->visualItemRect(item);
        if (!r.intersects(vp))
            continue; // not visible yet
        const int page = item->data(kPageRole).toInt();
        if (cache_.contains(page)) {
            item->setIcon(cache_.get(page));
            continue;
        }
        const double wPts = doc_->pageSize(page).width();
        const double scale = wPts > 0 ? kThumbW / wPts : 0.2;
        const QImage img = engine_->renderPageImage(doc_, page, scale, 0);
        if (!img.isNull()) {
            const QPixmap pm = QPixmap::fromImage(img);
            cache_.put(page, pm);
            item->setIcon(pm);
        }
    }
}

} // namespace mervin
