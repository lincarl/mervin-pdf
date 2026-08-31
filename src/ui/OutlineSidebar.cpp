#include "ui/OutlineSidebar.h"

#include <QTreeWidget>
#include <QVBoxLayout>

namespace mervin {

namespace {
constexpr int kPageRole = Qt::UserRole; // stores the 0-based page (or -1)
}

OutlineSidebar::OutlineSidebar(QWidget *parent)
    : QWidget(parent)
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    tree_ = new QTreeWidget(this);
    tree_->setHeaderHidden(true);
    tree_->setExpandsOnDoubleClick(true);
    connect(tree_, &QTreeWidget::itemClicked, this, [this](QTreeWidgetItem *item, int) {
        const int page = item->data(0, kPageRole).toInt();
        if (page >= 0)
            emit pageSelected(page);
    });
    layout->addWidget(tree_);
}

void OutlineSidebar::addItems(QTreeWidgetItem *parent, const std::vector<OutlineItem> &items)
{
    for (const OutlineItem &it : items) {
        auto *node = parent ? new QTreeWidgetItem(parent) : new QTreeWidgetItem(tree_);
        node->setText(0, it.title);
        node->setData(0, kPageRole, it.page);
        if (it.page >= 0)
            node->setToolTip(0, tr("Page %1").arg(it.page + 1));
        if (!it.children.empty())
            addItems(node, it.children);
    }
}

void OutlineSidebar::setOutline(const std::vector<OutlineItem> &items)
{
    tree_->clear();
    if (items.empty()) {
        auto *empty = new QTreeWidgetItem(tree_);
        empty->setText(0, tr("(no outline)"));
        empty->setData(0, kPageRole, -1);
        empty->setDisabled(true);
        return;
    }
    addItems(nullptr, items);
    tree_->expandToDepth(0); // show the top level expanded
}

} // namespace mervin
