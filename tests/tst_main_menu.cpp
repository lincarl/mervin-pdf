// The toolbar's main menu button and its menu, on a desktop of two monitors side by
// side. main() sets up the offscreen platform with those monitors.

#include "config/ConfigPaths.h"
#include "config/Settings.h"
#include "io/AppFiles.h"
#include "render/RenderEngine.h"
#include "ui/MainWindow.h"

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QKeySequence>
#include <QMenu>
#include <QPointer>
#include <QScopeGuard>
#include <QScreen>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QToolBar>
#include <QToolButton>

#include <algorithm>
#include <memory>
#include <utility>

using namespace mervin;

namespace {

QToolBar *toolbarOf(MainWindow &window)
{
    return window.findChild<QToolBar *>(QStringLiteral("mainToolBar"));
}

} // namespace

class TstMainMenu : public QObject
{
    Q_OBJECT

private slots:
    void init()
    {
        profile_ = std::make_unique<QTemporaryDir>();
        QVERIFY(profile_->isValid());
        ConfigPaths::setOverrideDir(profile_->path());
        Settings settings;
        settings.autoUpdate = false;
        settings.closeToTray = false;
        QVERIFY(settings.save());
    }

    void cleanup()
    {
        QVERIFY(appfiles::flush(10000));
        ConfigPaths::setOverrideDir({});
        profile_.reset();
    }

    // The menu button ends the toolbar. The menu repeats no toolbar command, and the
    // commands that left it keep their shortcuts on the window, which still work
    // while the toolbar is hidden.
    void leavesToolbarCommandsOut()
    {
        RenderEngine engine;
        MainWindow window(&engine, nullptr);
        QToolBar *toolbar = toolbarOf(window);
        QVERIFY(toolbar);
        auto *button = toolbar->findChild<QToolButton *>(QStringLiteral("menuButton"));
        QVERIFY(button && button->menu());
        QCOMPARE(toolbar->widgetForAction(toolbar->actions().constLast()), button);

        QList<QAction *> commands;
        for (QToolButton *tool : toolbar->findChildren<QToolButton *>())
            if (tool->defaultAction())
                commands.append(tool->defaultAction());
        for (QAction *command : std::as_const(commands))
            QVERIFY2(!button->menu()->actions().contains(command), qPrintable(command->text()));

        // Print, Fit Page, Fit Width, Comment, OCR Selection and Measure.
        for (const QKeySequence key : {QKeySequence(QKeySequence::Print),
                                       QKeySequence(Qt::CTRL | Qt::Key_1),
                                       QKeySequence(Qt::CTRL | Qt::Key_2),
                                       QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_N),
                                       QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_O),
                                       QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_M)}) {
            const auto found = std::find_if(commands.cbegin(), commands.cend(), [&key](QAction *a) {
                return a->shortcuts().contains(key);
            });
            QVERIFY2(found != commands.cend(), qPrintable(key.toString()));
            QVERIFY2(window.actions().contains(*found), qPrintable(key.toString()));
        }
    }

    // Most of the window is on the left monitor, so Qt assigns the window to it, but
    // the menu button at its right end is on the right monitor. The menu opens there,
    // its right edge lined up with the button's.
    void opensOnTheButtonsScreen()
    {
        const QList<QScreen *> screens = QGuiApplication::screens();
        QCOMPARE(screens.size(), 2);
        RenderEngine engine;
        MainWindow window(&engine, nullptr);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        const QRect right = screens.at(1)->availableGeometry();
        window.move(right.left() + 400 - window.frameGeometry().width(), right.top() + 50);
        QTRY_COMPARE(window.screen(), screens.at(0));
        auto *button = toolbarOf(window)->findChild<QToolButton *>(QStringLiteral("menuButton"));
        QVERIFY(button);
        const QRect buttonRect(button->mapToGlobal(QPoint()), button->size());
        QVERIFY(right.contains(buttonRect));

        bool inspected = false;
        QTimer inspect;
        inspect.setSingleShot(true);
        connect(&inspect, &QTimer::timeout, this, [&] {
            QPointer<QMenu> popup = qobject_cast<QMenu *>(QApplication::activePopupWidget());
            const auto close = qScopeGuard([popup] { if (popup) popup->close(); });
            QVERIFY(popup && popup == button->menu());
            // Room for the menu beside the button, or the screen edge would place it.
            QVERIFY(popup->width() < 400);
            QVERIFY(right.contains(popup->geometry()));
            QCOMPARE(popup->geometry().right(), buttonRect.right());
            inspected = true;
        });
        inspect.start(0);
        button->showMenu();
        QVERIFY(inspected);
    }

private:
    std::unique_ptr<QTemporaryDir> profile_;
};

// Two 1600 x 900 offscreen monitors side by side, without window frames. Qt splits
// the platform argument at colons, so the configuration file is named relative to
// the working directory, not by a path that can start with a drive letter.
int main(int argc, char **argv)
{
    const QString workingDirectory = QDir::currentPath();
    QTemporaryDir config;
    if (!config.isValid() || !QDir::setCurrent(config.path()))
        return 1;
    QFile file(QStringLiteral("screens.json"));
    if (!file.open(QIODevice::WriteOnly)
        || file.write(R"({"windowFrameMargins": false, "screens": [
            {"name": "left", "x": 0, "y": 0, "width": 1600, "height": 900},
            {"name": "right", "x": 1600, "y": 0, "width": 1600, "height": 900}]})") < 0)
        return 1;
    file.close();
    qputenv("QT_QPA_PLATFORM", "offscreen:configfile=screens.json");
    QApplication app(argc, argv);
    // The platform has read the file. Windows cannot remove the working directory.
    if (!QDir::setCurrent(workingDirectory))
        return 1;
    TstMainMenu test;
    return QTest::qExec(&test, argc, argv);
}

#include "tst_main_menu.moc"
