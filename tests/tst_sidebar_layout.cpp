#include "ui/SidebarLayout.h"

#include <QAbstractButton>
#include <QAction>
#include <QApplication>
#include <QLabel>
#include <QSignalSpy>
#include <QTabBar>
#include <QTest>
#include <QToolBar>

#include <array>

namespace {

std::array<QDockWidget *, 3> makeSidebars(QMainWindow &window)
{
    window.setAttribute(Qt::WA_DontShowOnScreen);
    window.resize(1100, 820);
    window.setTabPosition(Qt::LeftDockWidgetArea, QTabWidget::North);
    window.setCentralWidget(new QLabel(QStringLiteral("Document")));
    auto *toolbar = window.addToolBar(QStringLiteral("Main"));
    toolbar->setObjectName(QStringLiteral("mainToolBar"));
    toolbar->setMovable(false);
    toolbar->addAction(QStringLiteral("Menu"));
    std::array<QDockWidget *, 3> docks{};
    const char *names[] = {"thumbnailDock", "outlineDock", "commentsDock"};
    for (size_t i = 0; i < docks.size(); ++i) {
        auto *dock = new QDockWidget(QLatin1String(names[i]), &window);
        dock->setObjectName(QLatin1String(names[i]));
        dock->setWidget(new QLabel(QStringLiteral("Sidebar")));
        window.addDockWidget(Qt::LeftDockWidgetArea, dock);
        docks[i] = dock;
    }
    window.tabifyDockWidget(docks[0], docks[1]);
    window.tabifyDockWidget(docks[1], docks[2]);
    return docks;
}

QTabBar *sidebarTabBar(QMainWindow &window, const QString &title)
{
    for (QTabBar *bar : window.findChildren<QTabBar *>()) {
        if (!bar->isVisible())
            continue;
        for (int index = 0; index < bar->count(); ++index) {
            if (bar->tabText(index) == title)
                return bar;
        }
    }
    return nullptr;
}

int sidebarTabIndex(QTabBar &bar, const QString &title)
{
    for (int index = 0; index < bar.count(); ++index) {
        if (bar.tabText(index) == title)
            return index;
    }
    return -1;
}

// Saved by Qt 6.12 with Outline and Thumbnails tabified in one floating
// window, and Comments docked on the left. Keep an actual saved state so the
// regression exercises startup restoration, including Qt's group ownership.
const QByteArray groupedState = QByteArray::fromBase64(
    "AAAA/wAAAAD9AAAAAQAAAAAAAAA0AAADFPwCAAAAA/sAAAAWAG8AdQB0AGwAaQBuAGUARABvAGMAawIAAAUU"
    "AAABkAAAAPoAAAKK+wAAABoAdABoAHUAbQBiAG4AYQBpAGwARABvAGMAawAAAAAgAAABAgAAAAAAAAAA+wAA"
    "ABgAYwBvAG0AbQBlAG4AdABzAEQAbwBjAGsBAAAAIAAAAxQAAAAkAP///wAABBIAAAMUAAAABAAAAAQAAAAI"
    "AAAACPkAAAUUAAABkAAABg0AAAQZ+gAAAAEBAAAAAvsAAAAWAG8AdQB0AGwAaQBuAGUARABvAGMAawUAAAAA"
    "/////wAAADQA////+wAAABoAdABoAHUAbQBiAG4AYQBpAGwARABvAGMAawUAAAAA/////wAAADQA/////AAAAAEA"
    "AAACAAAAAQAAABYAbQBhAGkAbgBUAG8AbwBsAEIAYQByAQAAAAD/////AAAAAAAAAAA=");

// Exact layouts reported after an update: the broken one hides Thumbnails
// in a floating window; the working one keeps all three sidebars docked.
const QByteArray reportedBrokenState = QByteArray::fromBase64(
    "AAAA/wAAAAD9AAAAAQAAAAAAAAEAAAAFFPwCAAAAAvwAAAAA/////wAAAAAA////+gAAAAACAAAAAvsA"
    "AAAWAG8AdQB0AGwAaQBuAGUARABvAGMAawAAAAAA/////wAAAAAAAAAA+wAAABgAYwBvAG0AbQBlAG4A"
    "dABzAEQAbwBjAGsAAAAAAP////8AAABiAP////sAAAAaAHQAaAB1AG0AYgBuAGEAaQBsAEQAbwBjAGsC"
    "AAAGuQAAAtEAAAEAAAACiQAAB34AAAUTAAAABAAAAAQAAAAIAAAACPwAAAABAAAAAgAAAAEAAAAWAG0A"
    "YQBpAG4AVABvAG8AbABCAGEAcgEAAAAA/////wAAAAAAAAAA");

const QByteArray reportedWorkingState = QByteArray::fromBase64(
    "AAAA/wAAAAD9AAAAAQAAAAAAAAAAAAAAAPwCAAAAAfwAAAAA/////wAAAAAA////+v////8CAAAAA/sA"
    "AAAaAHQAaAB1AG0AYgBuAGEAaQBsAEQAbwBjAGsAAAAAAP////8AAABiAP////sAAAAWAG8AdQB0AGwA"
    "aQBuAGUARABvAGMAawAAAAAA/////wAAAGIA////+wAAABgAYwBvAG0AbQBlAG4AdABzAEQAbwBjAGsA"
    "AAAAAP////8AAABiAP///wAAB34AAAUTAAAABAAAAAQAAAAIAAAACPwAAAABAAAAAgAAAAEAAAAWAG0A"
    "YQBpAG4AVABvAG8AbABCAGEAcgEAAAAA/////wAAAAAAAAAA");

} // namespace

class TstSidebarLayout : public QObject
{
    Q_OBJECT

private slots:
    void sidebarDetachingIsBlocked_data();
    void sidebarDetachingIsBlocked();
    void sidebarsStillOpenCloseAndSwitch();
    void restoredSidebarsStayInMainWindow_data();
    void restoredSidebarsStayInMainWindow();
};

void TstSidebarLayout::sidebarDetachingIsBlocked_data()
{
    QTest::addColumn<int>("sidebar");
    QTest::addColumn<QString>("gesture");
    for (int sidebar = 0; sidebar < 3; ++sidebar) {
        for (const char *gesture : {"title-double-click", "title-drag", "tab-drag"}) {
            const QByteArray name = QByteArray::number(sidebar) + '-' + gesture;
            QTest::newRow(name.constData()) << sidebar << QString::fromLatin1(gesture);
        }
    }
}

void TstSidebarLayout::sidebarDetachingIsBlocked()
{
    QFETCH(int, sidebar);
    QFETCH(QString, gesture);

    QMainWindow window;
    const auto docks = makeSidebars(window);
    // Qt's grouped tab dragging can bypass individual docks' feature flags.
    // The application policy must turn it off as well as lock each title bar.
    window.setDockOptions(window.dockOptions() | QMainWindow::GroupedDragging);
    mervin::dockSidebarsOnLeft(window, {docks[0], docks[1], docks[2]});
    window.show();
    QDockWidget *dock = docks[static_cast<size_t>(sidebar)];
    dock->raise();
    QApplication::processEvents();
    QSignalSpy floated(dock, &QDockWidget::topLevelChanged);

    if (gesture == QLatin1String("tab-drag")) {
        QTabBar *bar = sidebarTabBar(window, dock->windowTitle());
        QVERIFY(bar);
        const int index = sidebarTabIndex(*bar, dock->windowTitle());
        QVERIFY(index >= 0);
        const QPoint start = bar->tabRect(index).center();
        const QPoint inBar = start + QPoint(QApplication::startDragDistance() + 1, 0);
        const QPoint outside = start + QPoint(0, bar->height() + 100);
        QTest::mousePress(bar, Qt::LeftButton, Qt::NoModifier, start);
        QTest::mouseMove(bar, inBar);
        QTest::mouseMove(bar, outside);
        QTest::mouseRelease(bar, Qt::LeftButton, Qt::NoModifier, outside);
    } else {
        // The content starts just below Qt's title bar. Use its center so the
        // gesture stays clear of the close button across styles and DPI scales.
        const int titleHeight = dock->widget()->geometry().top();
        QVERIFY(titleHeight > 0);
        const QPoint start(dock->width() / 2, titleHeight / 2);
        if (gesture == QLatin1String("title-double-click")) {
            QTest::mouseDClick(dock, Qt::LeftButton, Qt::NoModifier, start);
            QTest::mouseRelease(dock, Qt::LeftButton, Qt::NoModifier, start);
        } else {
            const QPoint outside = start + QPoint(250, 100);
            QTest::mousePress(dock, Qt::LeftButton, Qt::NoModifier, start);
            QTest::mouseMove(dock, outside);
            QTest::mouseRelease(dock, Qt::LeftButton, Qt::NoModifier, outside);
        }
    }
    QApplication::processEvents();
    QCOMPARE(floated.count(), 0);
    for (QDockWidget *sidebarDock : docks) {
        QVERIFY(!sidebarDock->isFloating());
        QCOMPARE(sidebarDock->window(), &window);
        QCOMPARE(window.dockWidgetArea(sidebarDock), Qt::LeftDockWidgetArea);
    }
}

void TstSidebarLayout::sidebarsStillOpenCloseAndSwitch()
{
    QMainWindow window;
    const auto docks = makeSidebars(window);
    mervin::dockSidebarsOnLeft(window, {docks[0], docks[1], docks[2]});
    window.show();
    QApplication::processEvents();

    for (QDockWidget *dock : docks) {
        QTabBar *bar = sidebarTabBar(window, dock->windowTitle());
        QVERIFY(bar);
        const int index = sidebarTabIndex(*bar, dock->windowTitle());
        QVERIFY(index >= 0);
        QTest::mouseClick(bar, Qt::LeftButton, Qt::NoModifier, bar->tabRect(index).center());
        QCOMPARE(bar->currentIndex(), index);
        QVERIFY(!dock->visibleRegion().isEmpty());

        auto *close = dock->findChild<QAbstractButton *>(QStringLiteral("qt_dockwidget_closebutton"));
        QVERIFY(close);
        QVERIFY(close->isVisible());
        QTest::mouseClick(close, Qt::LeftButton);
        QVERIFY(dock->isHidden());
        QVERIFY(!dock->toggleViewAction()->isChecked());

        dock->toggleViewAction()->trigger();
        QVERIFY(!dock->isHidden());
        QVERIFY(dock->toggleViewAction()->isChecked());
        QVERIFY(!dock->isFloating());
        QCOMPARE(dock->window(), &window);
        QApplication::processEvents();
    }
}

void TstSidebarLayout::restoredSidebarsStayInMainWindow_data()
{
    QTest::addColumn<QByteArray>("state");
    QTest::addColumn<bool>("thumbnailHidden");
    QTest::addColumn<bool>("needsMigration");

    for (bool hidden : {false, true}) {
        QMainWindow previous;
        const auto docks = makeSidebars(previous);
        previous.show();
        docks[0]->setFloating(true);
        docks[0]->setVisible(!hidden);
        QTest::newRow(hidden ? "hidden-floating-sidebar" : "floating-sidebar")
            << previous.saveState() << hidden << true;
    }
    QTest::newRow("floating-tab-group") << groupedState << false << true;
    QTest::newRow("reported-broken-layout") << reportedBrokenState << true << true;
    QTest::newRow("reported-working-layout") << reportedWorkingState << true << false;
}

void TstSidebarLayout::restoredSidebarsStayInMainWindow()
{
    QFETCH(QByteArray, state);
    QFETCH(bool, thumbnailHidden);
    QFETCH(bool, needsMigration);

    QMainWindow window;
    const auto docks = makeSidebars(window);
    for (QDockWidget *dock : docks) {
        dock->setAllowedAreas(Qt::LeftDockWidgetArea);
        dock->setFeatures(QDockWidget::DockWidgetClosable);
        dock->hide();
    }
    QVERIFY(window.restoreState(state));
    QCOMPARE(docks[0]->window() != &window, needsMigration);

    mervin::dockSidebarsOnLeft(window, {docks[0], docks[1], docks[2]});
    window.show();

    for (QDockWidget *dock : docks) {
        QVERIFY(!dock->isFloating());
        QCOMPARE(dock->window(), &window);
        QCOMPARE(window.dockWidgetArea(dock), Qt::LeftDockWidgetArea);
    }
    QCOMPARE(docks[0]->isHidden(), thumbnailHidden);

    // Group windows are cleaned up with deferred deletes. Once startup settles,
    // no extra window should remain visible next to the document.
    const auto hasExtraWindow = [&window] {
        for (QWidget *top : QApplication::topLevelWidgets())
            if (top != &window && top->isVisible())
                return true;
        return false;
    };
    QTRY_VERIFY(!hasExtraWindow());

    // The broken restoration also left a spare dock tab bar over the menu.
    // Check hit testing as well as extra windows, including the known-good
    // saved state which must keep its normal toolbar behavior.
    auto *toolbar = window.findChild<QToolBar *>(QStringLiteral("mainToolBar"));
    QWidget *menu = toolbar->widgetForAction(toolbar->actions().first());
    QVERIFY(menu);
    QTRY_COMPARE(window.childAt(menu->mapTo(&window, menu->rect().center())), menu);

    // Persist the repaired layout and restore it without a second repair: the
    // next launch must not recreate the detached windows from the old state.
    QMainWindow nextWindow;
    const auto nextDocks = makeSidebars(nextWindow);
    QVERIFY(nextWindow.restoreState(window.saveState()));
    for (QDockWidget *dock : nextDocks) {
        QVERIFY(!dock->isFloating());
        QCOMPARE(dock->window(), &nextWindow);
        QCOMPARE(nextWindow.dockWidgetArea(dock), Qt::LeftDockWidgetArea);
    }
    QCOMPARE(nextDocks[0]->isHidden(), thumbnailHidden);
}

QTEST_MAIN(TstSidebarLayout)
#include "tst_sidebar_layout.moc"
