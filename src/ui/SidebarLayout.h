#pragma once

#include <QDockWidget>
#include <QMainWindow>

#include <initializer_list>

namespace mervin {

// Keep sidebars fixed on the left, both during interaction and when restoring
// or saving layouts. The order determines the tabs when a layout needs repair.
inline void dockSidebarsOnLeft(QMainWindow &window,
                               std::initializer_list<QDockWidget *> sidebars)
{
    // Qt can drag a tabbed group independently of individual dock features.
    window.setDockOptions(window.dockOptions() & ~QMainWindow::GroupedDragging);
    bool migrated = false;
    for (QDockWidget *dock : sidebars) {
        dock->setAllowedAreas(Qt::LeftDockWidgetArea);
        dock->setFeatures(QDockWidget::DockWidgetClosable);
        if (dock->window() != &window
            || window.dockWidgetArea(dock) != Qt::LeftDockWidgetArea) {
            window.addDockWidget(Qt::LeftDockWidgetArea, dock);
            // addDockWidget changes the dock area but does not clear floating
            // state. Do this after reparenting: removing a member of a floating
            // tab group can make Qt float the last remaining member as well.
            dock->setFloating(false);
            migrated = true;
        }
    }
    if (migrated) {
        QDockWidget *previous = nullptr;
        for (QDockWidget *dock : sidebars) {
            if (previous)
                window.tabifyDockWidget(previous, dock);
            previous = dock;
        }
    }
}

} // namespace mervin
