#pragma once

#include "render/Document.h"

#include <QWidget>

#include <vector>

class QTreeWidget;
class QTreeWidgetItem;

namespace mervin {

// Collapsible document-outline / bookmarks panel. Populated from a document's
// OutlineItem tree; activating an entry emits pageSelected with its 0-based
// page. A pure view - the window supplies the outline and routes navigation.
class OutlineSidebar : public QWidget
{
    Q_OBJECT

public:
    explicit OutlineSidebar(QWidget *parent = nullptr);

    void setOutline(const std::vector<OutlineItem> &items);

signals:
    void pageSelected(int page);

private:
    void addItems(QTreeWidgetItem *parent, const std::vector<OutlineItem> &items);

    QTreeWidget *tree_ = nullptr;
};

} // namespace mervin
