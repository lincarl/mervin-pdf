#include "ui/WindowPlacement.h"
#include "dialogs/PrintDialog.h"
#include "ui/Theme.h"
#include "render/RenderEngine.h"

#include <QApplication>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFontDatabase>
#include <QLabel>
#include <QMenu>
#include <QPrinter>
#include <QPushButton>
#include <QScreen>
#include <QScrollArea>
#include <QScrollBar>
#include <QTemporaryDir>
#include <QVBoxLayout>
#include <QtTest>

class TstWindowPlacement : public QObject
{
    Q_OBJECT

private slots:
    // Offscreen Windows needs an explicit font, just like the Print layout suite.
    void initTestCase()
    {
        const QString fontPath = qEnvironmentVariable("MERVIN_TEST_FONT");
        if (!fontPath.isEmpty()) {
            const QString latinPath = QFileInfo(fontPath).absoluteDir().filePath(
                QStringLiteral("NotoSans-Regular.ttf"));
            QVERIFY2(QFontDatabase::addApplicationFont(latinPath) >= 0,
                     qPrintable(QStringLiteral("Could not load %1. Run scripts/fetch-test-font.py.")
                                    .arg(latinPath)));
        }
        QVERIFY2(QFontDatabase::families().contains(QStringLiteral("Noto Sans")),
                 "Noto Sans is required. Set MERVIN_TEST_FONT after running scripts/fetch-test-font.py.");
        QApplication::setFont(QFont(QStringLiteral("Noto Sans"), 10));
    }

    // An oversized client and its native frame must both fit, even when the
    // dialog was explicitly positioned beyond the bottom-right screen edge.
    void oversizedDialogFits()
    {
        QDialog dialog;
        auto *layout = new QVBoxLayout(&dialog);
        layout->addWidget(new QLabel(QStringLiteral("Window content")));
        auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
        layout->addWidget(buttons);
        const QRect available = dialog.screen()->availableGeometry();
        dialog.resize(available.size() + QSize(200, 200));
        dialog.move(available.bottomRight() - QPoint(100, 100));
        dialog.show();
        QTRY_VERIFY(available.contains(dialog.frameGeometry()));
        for (QAbstractButton *button : buttons->buttons())
            QVERIFY(available.contains(QRect(button->mapToGlobal(QPoint()), button->size())));
        dialog.resize(dialog.width(), available.height() + 100);
        QTRY_VERIFY(available.contains(dialog.frameGeometry()));
    }

    // Child dialogs stay on the parent's monitor, including negative coordinates.
    void dialogUsesParentsScreen()
    {
        const auto screens = QGuiApplication::screens();
        QCOMPARE(screens.size(), 2);
        const QRect available = screens.at(1)->availableGeometry();
        QWidget parent;
        parent.resize(400, 300);
        parent.move(available.bottomRight() - QPoint(450, 350));
        parent.show();
        QTRY_COMPARE(parent.screen(), screens.at(1));
        QDialog dialog(&parent);
        dialog.resize(800, 700);
        dialog.show();
        QTRY_COMPARE(dialog.screen(), screens.at(1));
        QTest::qWait(50);
        qInfo() << "Parent monitor" << available << "dialog frame" << dialog.frameGeometry()
                << "minimum" << dialog.minimumSize() << "client" << dialog.geometry();
        QTRY_VERIFY(available.contains(dialog.frameGeometry()));
    }

    // A document's toolbar text can change without growing the window. Preserve
    // a user's deliberate placement across adjacent monitors in that case.
    void layoutChangesPreservePlacementAcrossScreens()
    {
        QWidget window;
        auto *layout = new QVBoxLayout(&window);
        auto *label = new QLabel(QStringLiteral("Page 1 of 2"));
        layout->addWidget(label);
        window.resize(400, 300);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        QTest::qWait(50);
        window.move(-200, 100);
        QTest::qWait(50);
        const QRect placed = window.frameGeometry();
        const QSize size = window.size();
        const auto screens = QGuiApplication::screens();
        QVERIFY(!screens.at(0)->availableGeometry().contains(placed));
        QVERIFY(!screens.at(1)->availableGeometry().contains(placed));
        QVERIFY((QRegion(screens.at(0)->availableGeometry())
                 | QRegion(screens.at(1)->availableGeometry())).contains(placed));

        label->setText(QStringLiteral("Page 10000 of 10000"));
        QCoreApplication::sendPostedEvents(nullptr, QEvent::LayoutRequest);
        QTest::qWait(50);
        QCOMPARE(window.size(), size);
        QCOMPARE(window.frameGeometry(), placed);

        window.resize(size - QSize(20, 20));
        QTest::qWait(50);
        // The offscreen platform can shift frame margins during resize. The
        // application must still leave this smaller window spanning both screens.
        QVERIFY(window.frameGeometry().left() < screens.at(0)->availableGeometry().left());
        QVERIFY(window.frameGeometry().right() >= screens.at(0)->availableGeometry().left());
    }

    // The real print settings can scroll while every footer action stays visible.
    void printFitsWithReachableSettings_data()
    {
        QTest::addColumn<int>("screenIndex");
        QTest::addColumn<int>("fontSize");
        QTest::newRow("baseline") << 0 << 10;
        QTest::newRow("baseline-large-font") << 0 << 15;
        QTest::newRow("compact") << 1 << 10;
        QTest::newRow("compact-large-font") << 1 << 15;
    }

    void printFitsWithReachableSettings()
    {
        QFETCH(int, screenIndex);
        QFETCH(int, fontSize);
        QApplication::setFont(QFont(QStringLiteral("Noto Sans"), fontSize));
        mervin::Theme::applyApp();
        QWidget parent;
        parent.resize(500, 300);
        const QRect available = QGuiApplication::screens().at(screenIndex)->availableGeometry();
        parent.move(available.right() - 520, available.bottom() - 320);
        parent.show();
        QTRY_COMPARE(parent.screen(), QGuiApplication::screens().at(screenIndex));
        QPrinter printer;
        printer.setOutputFormat(QPrinter::PdfFormat);
        mervin::RenderEngine engine;
        PrintDialog dialog(&printer, &engine, nullptr, 0, QPageLayout::Portrait, 1,
                           QStringLiteral("example"), &parent);
        dialog.resize(1000, available.height() + 200);
        dialog.show();
        QTest::qWait(50);
        qInfo() << "Print work area" << available << "frame" << dialog.frameGeometry()
                << "minimum" << dialog.minimumSize() << "client" << dialog.geometry()
                << "hint" << dialog.minimumSizeHint()
                << "height for minimum width" << dialog.layout()->totalHeightForWidth(dialog.minimumWidth());
        QTRY_VERIFY(available.contains(dialog.frameGeometry()));
        for (const char *name : {"printSystemDialog", "printCancel", "printConfirm"}) {
            auto *button = dialog.findChild<QPushButton *>(QLatin1String(name));
            QVERIFY(button);
            QVERIFY(available.contains(QRect(button->mapToGlobal(QPoint()), button->size())));
        }
        auto *scroll = dialog.findChild<QScrollArea *>(QStringLiteral("printSettingsScroll"));
        auto *last = dialog.findChild<QWidget *>(QStringLiteral("printTwoSided"));
        QVERIFY(scroll && last);
        scroll->ensureWidgetVisible(last);
        QTRY_VERIFY(scroll->viewport()->rect().contains(
            QRect(last->mapTo(scroll->viewport(), QPoint()), last->size())));
    }

    // Showing an ordinary window again must preserve a deliberate maximized state.
    void maximizedWindowKeepsItsState()
    {
        QWidget window;
        window.showMaximized();
        QTest::qWait(10);
        QVERIFY(window.isMaximized());
    }
};

// Two work areas with different sizes catch primary-screen assumptions. The
// desktop suite separately verifies real frames, taskbars and OS scaling.
int main(int argc, char **argv)
{
    const QString workingDirectory = QDir::currentPath();
    QTemporaryDir configuration;
    if (!configuration.isValid() || !QDir::setCurrent(configuration.path()))
        return 1;
    QFile file(QStringLiteral("screens.json"));
    if (!file.open(QIODevice::WriteOnly)
        || file.write(R"({"screens": [
            {"name": "baseline", "x": 0, "y": 0, "width": 1536, "height": 826},
            {"name": "compact", "x": -1024, "y": 0, "width": 1024, "height": 670}]})") < 0)
        return 1;
    file.close();
    qputenv("QT_QPA_PLATFORM", "offscreen:configfile=screens.json");
    QApplication app(argc, argv);
    mervin::keepWindowsOnScreen(app);
    if (!QDir::setCurrent(workingDirectory))
        return 1;
    TstWindowPlacement test;
    return QTest::qExec(&test, argc, argv);
}

#include "tst_window_placement.moc"
