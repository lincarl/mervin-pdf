#include "app/WindowManager.h"
#include "config/ConfigPaths.h"
#include "config/Settings.h"
#include "dialogs/ExtractDialog.h"
#include "dialogs/SettingsDialog.h"
#include "render/AnnotModel.h"
#include "render/MeasureContent.h"
#include "render/RenderEngine.h"
#include "security/MeasureExport.h"
#include "ui/MainWindow.h"
#include "ui/TabPage.h"
#include "ui/ThemeTokens.h"
#include "ui/ViewerWidget.h"

#include <QAction>
#include <QComboBox>
#include <QContextMenuEvent>
#include <QFile>
#include <QLineEdit>
#include <QMap>
#include <QMenu>
#include <QMessageBox>
#include <QPushButton>
#include <QScopeGuard>
#include <QTabBar>
#include <QTemporaryDir>
#include <QTimer>
#include <QToolButton>
#include <QtTest>

#include <functional>
#include <memory>

using namespace mervin;

namespace {

// Counts the events of one type sent to qApp while it is installed as a filter.
class EventCounter : public QObject
{
public:
    EventCounter(QEvent::Type type, int *count) : type_(type), count_(count) {}

    bool eventFilter(QObject *watched, QEvent *event) override
    {
        if (event->type() == type_ && watched == qApp)
            ++*count_;
        return false;
    }

private:
    QEvent::Type type_;
    int *count_;
};

} // namespace

class TstDocumentWorkflow : public QObject
{
    Q_OBJECT
public:
    TstDocumentWorkflow()
    {
        connect(&dialogs_, &QTimer::timeout, this, [this] {
            auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
            if (!box)
                return;
            if (answers_.isEmpty()) {
                unexpectedDialogs_.append(box->windowTitle() + ": " + box->text());
                box->reject();
            } else if (auto *button = box->button(answers_.takeFirst())) {
                button->click();
            } else {
                unexpectedDialogs_.append("Missing expected button: " + box->text());
                box->reject();
            }
        });
    }

private slots:
    void init()
    {
        ConfigPaths::setOverrideDir(profile_.path());
        answers_.clear();
        unexpectedDialogs_.clear();
        dialogs_.start(10);
    }
    void cleanup()
    {
        dialogs_.stop();
        ConfigPaths::setOverrideDir({});
        QVERIFY2(unexpectedDialogs_.isEmpty(), qPrintable(unexpectedDialogs_.join('\n')));
        QVERIFY(answers_.isEmpty());
    }
    // An OS accent change arrives as QEvent::ThemeChange, with no scheme change.
    // The manager reapplies the theme, so the new accent reaches the stylesheet.
    void themeChangeReappliesTheTheme()
    {
        WindowManager manager;
        const auto clearSheet = qScopeGuard([] { qApp->setStyleSheet(QString()); });
        QVERIFY(!qApp->styleSheet().isEmpty());
        qApp->setStyleSheet(QString());
        QEvent change(QEvent::ThemeChange);
        QCoreApplication::sendEvent(qApp, &change);
        QTRY_VERIFY(!qApp->styleSheet().isEmpty());
    }
    // Plasma's platform theme reports an accent change by replacing the whole
    // application palette. The manager puts its own palette back, once.
    void foreignPaletteIsReplaced()
    {
        WindowManager manager;
        const auto clearSheet = qScopeGuard([] { qApp->setStyleSheet(QString()); });
        const QPalette applied = qApp->palette();
        const QColor foreign = theme::isDark(applied) ? QColor(0x20, 0x20, 0x20) : QColor(0xee, 0xee, 0xee);
        qApp->setPalette(QPalette(foreign));
        QTRY_COMPARE(qApp->palette().color(QPalette::Window), applied.color(QPalette::Window));

        // Its own palette changes must not schedule another round.
        int changes = 0;
        const auto counter = std::make_unique<EventCounter>(QEvent::ApplicationPaletteChange, &changes);
        qApp->installEventFilter(counter.get());
        QTest::qWait(300);
        QCOMPARE(changes, 0);
    }
    // About and Keyboard Shortcuts open Settings on their own page, and OK only
    // re-applies the viewing defaults that changed: OK on About must not throw
    // away the zoom the user set on the open document.
    void settingsOkKeepsTheViewUnlessItsDefaultsChanged()
    {
        RenderEngine engine;
        MainWindow window(&engine, nullptr);
        QVERIFY(window.openFile(QStringLiteral(MERVIN_FIXTURE_PDF)));
        ViewerWidget *viewer = window.findChild<TabPage *>()->viewer();
        viewer->setScale(1.75);
        QCOMPARE(viewer->zoomMode(), ViewerWidget::ZoomMode::Custom);

        SettingsDialog::Page seen = SettingsDialog::Page::General;
        auto acceptSettings = [&](std::function<void(SettingsDialog *)> edit) {
            QTimer::singleShot(0, this, [&seen, edit] {
                auto *dialog = qobject_cast<SettingsDialog *>(QApplication::activeModalWidget());
                QVERIFY(dialog);
                seen = dialog->currentPage();
                edit(dialog);
                dialog->accept();
            });
        };

        acceptSettings([](SettingsDialog *) {});
        QVERIFY(QMetaObject::invokeMethod(&window, "showAbout"));
        QCOMPARE(seen, SettingsDialog::Page::About);
        QCOMPARE(viewer->zoomMode(), ViewerWidget::ZoomMode::Custom);
        QCOMPARE(viewer->scale(), 1.75);

        acceptSettings([](SettingsDialog *) {});
        QVERIFY(QMetaObject::invokeMethod(&window, "showShortcuts"));
        QCOMPARE(seen, SettingsDialog::Page::Shortcuts);
        QCOMPARE(viewer->scale(), 1.75);

        // A changed default zoom does reach the open document.
        acceptSettings([](SettingsDialog *dialog) {
            for (QComboBox *combo : dialog->findChildren<QComboBox *>())
                if (const int fitPage = combo->findData(QStringLiteral("fit-page")); fitPage >= 0)
                    combo->setCurrentIndex(fitPage);
        });
        QVERIFY(QMetaObject::invokeMethod(&window, "openSettings"));
        QCOMPARE(seen, SettingsDialog::Page::General);
        QCOMPARE(viewer->zoomMode(), ViewerWidget::ZoomMode::FitPage);
        QCOMPARE(Settings::load().defaultZoom, QStringLiteral("fit-page"));
    }

    // Each fit button always selects its own preset. Home keeps the keyboard toggle.
    void fitButtonsSelectTheirOwnModes()
    {
        RenderEngine engine;
        MainWindow window(&engine, nullptr);
        window.setAttribute(Qt::WA_DontShowOnScreen);
        window.resize(1000, 800);
        window.show();

        const auto actionFor = [&window](QKeySequence shortcut) -> QAction * {
            for (QAction *action : window.findChildren<QAction *>())
                if (action->shortcut() == shortcut)
                    return action;
            return nullptr;
        };
        const auto buttonFor = [&window](QAction *action) -> QToolButton * {
            for (QToolButton *button : window.findChildren<QToolButton *>())
                if (button->defaultAction() == action)
                    return button;
            return nullptr;
        };
        QAction *page = actionFor(QKeySequence(Qt::CTRL | Qt::Key_1));
        QAction *width = actionFor(QKeySequence(Qt::CTRL | Qt::Key_2));
        QAction *toggle = actionFor(QKeySequence(Qt::Key_Home));
        QVERIFY(page);
        QVERIFY(width);
        QVERIFY(toggle);
        QToolButton *pageButton = buttonFor(page);
        QToolButton *widthButton = buttonFor(width);
        QVERIFY(pageButton);
        QVERIFY(widthButton);
        QVERIFY(pageButton != widthButton);
        QVERIFY(!buttonFor(toggle));
        for (QAction *action : {page, width, toggle}) {
            QVERIFY(window.actions().contains(action));
            QVERIFY(!action->isEnabled());
        }
        QVERIFY(!pageButton->isEnabled());
        QVERIFY(!widthButton->isEnabled());

        QVERIFY(window.openFile(QStringLiteral(MERVIN_FIXTURE_PDF)));
        auto *tab = window.findChild<TabPage *>();
        QVERIFY(tab);
        auto *viewer = tab->viewer();
        viewer->setZoomEaseMs(0);
        for (QToolButton *button : {pageButton, widthButton}) {
            QVERIFY(button->isEnabled());
            viewer->setScale(1.75);
            QCOMPARE(viewer->zoomMode(), ViewerWidget::ZoomMode::Custom);
            const auto mode = button == pageButton ? ViewerWidget::ZoomMode::FitPage
                                                   : ViewerWidget::ZoomMode::FitWidth;
            QTest::mouseClick(button, Qt::LeftButton);
            QCOMPARE(viewer->zoomMode(), mode);
            const double scale = viewer->scale();
            QTest::mouseClick(button, Qt::LeftButton);
            QCOMPARE(viewer->zoomMode(), mode);
            QCOMPARE(viewer->scale(), scale);
        }
        toggle->trigger();
        QCOMPARE(viewer->zoomMode(), ViewerWidget::ZoomMode::FitPage);
        toggle->trigger();
        QCOMPARE(viewer->zoomMode(), ViewerWidget::ZoomMode::FitWidth);

        auto *recent = window.findChild<QPushButton *>(QStringLiteral("recentPillBtn"));
        auto *bar = window.findChild<QTabBar *>(QStringLiteral("docTabBar"));
        QVERIFY(recent);
        QVERIFY(bar);
        QTest::mouseClick(recent, Qt::LeftButton);
        QVERIFY(recent->property("recentActive").toBool());
        for (QAction *action : {page, width, toggle}) {
            QVERIFY(!action->isEnabled());
            action->trigger();
            QCOMPARE(viewer->zoomMode(), ViewerWidget::ZoomMode::FitWidth);
        }
        QVERIFY(!pageButton->isEnabled());
        QVERIFY(!widthButton->isEnabled());
        QTest::mouseClick(bar, Qt::LeftButton, Qt::NoModifier, bar->tabRect(0).center());
        QVERIFY(!recent->property("recentActive").toBool());
        QVERIFY(pageButton->isEnabled());
        QVERIFY(widthButton->isEnabled());
        QVERIFY(toggle->isEnabled());
    }

    // Save Page As in a page's right-click menu opens Extract Pages with the
    // clicked page as its one row. The gaps between pages offer no such item.
    void savePageAsSeedsExtractWithTheClickedPage()
    {
        RenderEngine engine;
        MainWindow window(&engine, nullptr);
        window.setAttribute(Qt::WA_DontShowOnScreen);
        window.resize(900, 700);
        window.show(); // laid out, but never on screen
        QVERIFY(window.openFile(QStringLiteral(MERVIN_FIXTURE_PDF))); // 4 pages
        ViewerWidget *viewer = window.findChild<TabPage *>()->viewer();
        QTRY_VERIFY(viewer->viewport()->height() > 200);
        viewer->setScale(0.3); // several pages on screen at once

        // A point on a page other than the current one, and one in the gap
        // around the pages.
        QMap<int, QPoint> pointOn;
        QPoint offPage(-1, -1);
        const QSize area = viewer->viewport()->size();
        for (int y = 0; y < area.height(); y += 4)
            for (int x = 0; x < area.width(); x += 4) {
                const int page = viewer->pageUnder({x, y});
                if (page >= 0 && !pointOn.contains(page))
                    pointOn.insert(page, {x, y});
                if (page < 0 && offPage.x() < 0)
                    offPage = {x, y};
            }
        QVERIFY(offPage.x() >= 0);
        int target = -1;
        for (auto it = pointOn.cbegin(); it != pointOn.cend(); ++it)
            if (it.key() != viewer->currentPage())
                target = it.key();
        QVERIFY2(target >= 0, "only the current page is on screen");

        // Answer the menu, then the Extract dialog, as they appear.
        QStringList menuItems;
        QString seeded;
        bool choose = false;
        QTimer driver;
        connect(&driver, &QTimer::timeout, this, [&] {
            if (auto *menu = qobject_cast<QMenu *>(QApplication::activePopupWidget())) {
                menuItems.clear();
                QAction *savePage = nullptr;
                for (QAction *a : menu->actions()) {
                    menuItems << a->text();
                    if (a->text() == QStringLiteral("Save Page As"))
                        savePage = a;
                }
                if (choose && savePage) {
                    menu->setActiveAction(savePage);
                    QTest::keyClick(menu, Qt::Key_Return);
                } else {
                    menu->close();
                }
            } else if (auto *dialog = qobject_cast<ExtractDialog *>(QApplication::activeModalWidget())) {
                const auto rows = dialog->findChildren<QLineEdit *>(QStringLiteral("extractRowSpec"));
                seeded = rows.size() == 1 ? rows.first()->text() : QStringLiteral("rows: %1").arg(rows.size());
                dialog->reject();
            }
        });
        driver.start(10);

        auto rightClick = [&](QPoint at) {
            QContextMenuEvent event(QContextMenuEvent::Mouse, at, viewer->viewport()->mapToGlobal(at));
            QApplication::sendEvent(viewer->viewport(), &event);
        };

        rightClick(offPage);
        QVERIFY(!menuItems.isEmpty());
        QVERIFY(!menuItems.contains(QStringLiteral("Save Page As")));

        // The row holds the clicked page, not the one the toolbar shows.
        choose = true;
        rightClick(pointOn.value(target));
        QVERIFY(menuItems.contains(QStringLiteral("Save Page As")));
        QTRY_COMPARE(seeded, QString::number(target + 1));
        driver.stop();
    }

    void closingDirtyTabCanBeCanceled()
    {
        RenderEngine engine;
        MainWindow window(&engine, nullptr);
        QVERIFY(window.openFile(QStringLiteral(MERVIN_FIXTURE_PDF)));
        auto *tab = window.findChild<TabPage *>();
        QVERIFY(tab);
        auto *model = tab->viewer()->annotModel();
        QVERIFY(model->addTextNote(0, {50, 50}, Qt::yellow, {}, "pending") >= 0);
        answers_.append(QMessageBox::Cancel);
        QVERIFY(QMetaObject::invokeMethod(&window, "closeTab", Q_ARG(int, 0)));
        QCOMPARE(window.tabCount(), 1);
        QVERIFY(tab->viewer()->hasUnsavedEdits());
        answers_.append(QMessageBox::Discard);
        QVERIFY(QMetaObject::invokeMethod(&window, "closeTab", Q_ARG(int, 0)));
        QCOMPARE(window.tabCount(), 0);
    }

    void canceledQuitPreservesSuspendedDocumentsAcrossWindows()
    {
        QTemporaryDir files;
        const QString firstPath = files.filePath(QStringLiteral("first.pdf"));
        const QString secondPath = files.filePath(QStringLiteral("second.pdf"));
        QVERIFY(QFile::copy(QStringLiteral(MERVIN_FIXTURE_PDF), firstPath));
        QVERIFY(QFile::copy(QStringLiteral(MERVIN_FIXTURE_PDF), secondPath));
        WindowManager manager;
        MainWindow *first = manager.createWindow();
        MainWindow *second = manager.createWindow();
        QVERIFY(first->openFile(firstPath));
        QVERIFY(second->openFile(secondPath));
        auto *firstTab = first->findChild<TabPage *>();
        auto *secondTab = second->findChild<TabPage *>();
        QVERIFY(firstTab);
        QVERIFY(secondTab);
        QVERIFY(firstTab->viewer()->annotModel()->addTextNote(
                    0, {40, 40}, Qt::yellow, {}, QStringLiteral("First pending edit")) >= 0);
        QVERIFY(secondTab->viewer()->annotModel()->addTextNote(
                    0, {40, 40}, Qt::yellow, {}, QStringLiteral("Second pending edit")) >= 0);
        first->hide();
        second->hide();
        QVERIFY(firstTab->suspend());
        QVERIFY(secondTab->suspend());

        answers_ << QMessageBox::Discard << QMessageBox::Cancel;
        QVERIFY(!manager.closeAllForQuit());
        QCOMPARE(manager.openTabPaths().size(), 2);
        QVERIFY(firstTab->isSuspended());
        QVERIFY(secondTab->isSuspended());
        QVERIFY(firstTab->hasUnsavedEdits());
        QVERIFY(secondTab->hasUnsavedEdits());

        answers_ << QMessageBox::Save << QMessageBox::Cancel;
        QVERIFY(!manager.closeAllForQuit());
        QCOMPARE(manager.openTabPaths().size(), 2);
        QVERIFY(!firstTab->hasUnsavedEdits());
        QVERIFY(secondTab->isSuspended());
        QVERIFY(secondTab->hasUnsavedEdits());
        auto saved = manager.engine()->openDocument(firstPath);
        QVERIFY(saved);
        const auto notes = AnnotModel(*saved).pageAnnots(0);
        QCOMPARE(notes.size(), 1u);
        QCOMPARE(notes.front().contents, QStringLiteral("First pending edit"));
    }

    void savingSuspendedEditsRequiresConfirmingExternalChanges()
    {
        QTemporaryDir files;
        const QString path = files.filePath(QStringLiteral("changed.pdf"));
        QVERIFY(QFile::copy(QStringLiteral(MERVIN_FIXTURE_PDF), path));
        RenderEngine engine;
        MainWindow window(&engine, nullptr);
        QVERIFY(window.openFile(path));
        auto *tab = window.findChild<TabPage *>();
        QVERIFY(tab);
        QVERIFY(tab->viewer()->annotModel()->addTextNote(
                    0, {40, 40}, Qt::yellow, {}, QStringLiteral("Preserved edit")) >= 0);
        QVERIFY(tab->suspend());
        QFile changed(path);
        QVERIFY(changed.open(QIODevice::Append));
        QVERIFY(changed.write("\n% External change\n") > 0);
        changed.close();
        QVERIFY(changed.open(QIODevice::ReadOnly));
        const QByteArray externalVersion = changed.readAll();
        changed.close();

        answers_.append(QMessageBox::Cancel);
        QVERIFY(QMetaObject::invokeMethod(&window, "saveMeasurements"));
        QVERIFY(tab->hasUnsavedEdits());
        QVERIFY(changed.open(QIODevice::ReadOnly));
        QCOMPARE(changed.readAll(), externalVersion);
        changed.close();

        answers_.append(QMessageBox::Yes);
        QVERIFY(QMetaObject::invokeMethod(&window, "saveMeasurements"));
        QVERIFY(!tab->hasUnsavedEdits());
        auto saved = engine.openDocument(path);
        QVERIFY(saved);
        const auto notes = AnnotModel(*saved).pageAnnots(0);
        QCOMPARE(notes.size(), 1u);
        QCOMPARE(notes.front().contents, QStringLiteral("Preserved edit"));
    }

    void saveDeletionOfLastMeasurement()
    {
        QTemporaryDir files;
        const QString path = files.filePath("measured.pdf");
        MeasureDoc data;
        data.measurements.push_back({0, MeasureKind::Distance, {{10, 10}, {80, 10}}});
        QCOMPARE(MeasureExport::embedMervin(QStringLiteral(MERVIN_FIXTURE_PDF), path, data, {}),
                 MeasureExport::Status::Ok);
        RenderEngine engine;
        MainWindow window(&engine, nullptr);
        QVERIFY(window.openFile(path));
        auto *tab = window.findChild<TabPage *>();
        QVERIFY(tab);
        auto *viewer = tab->viewer();
        QVERIFY(!viewer->hasMeasurementEdits());
        viewer->clearMeasurements();
        QVERIFY(viewer->hasMeasurementEdits());
        QVERIFY(QMetaObject::invokeMethod(&window, "saveMeasurements"));
        QVERIFY(!viewer->hasUnsavedEdits());
        auto blob = MeasureExport::readMervinBlob(path);
        QVERIFY(blob);
        MeasureDoc saved;
        QVERIFY(parseMeasurements(*blob, &saved));
        QVERIFY(saved.measurements.empty());
        QVERIFY(saved.pageScales.empty());
        QVERIFY(QMetaObject::invokeMethod(&window, "closeTab", Q_ARG(int, 0)));
    }

    void failedSaveRetainsEditableSnapshot()
    {
        QTemporaryDir files;
        const QString path = files.filePath("read-only.pdf");
        QVERIFY(QFile::copy(QStringLiteral(MERVIN_FIXTURE_PDF), path));
        RenderEngine engine;
        MainWindow window(&engine, nullptr);
        QVERIFY(window.openFile(path));
        auto *tab = window.findChild<TabPage *>();
        QVERIFY(tab);
        QVERIFY(tab->viewer()->annotModel()->addTextNote(0, {40, 40}, Qt::yellow, {}, "retained") >= 0);
        const auto permissions = QFile::permissions(path);
        QVERIFY(QFile::setPermissions(path, QFileDevice::ReadOwner | QFileDevice::ReadUser
                                            | QFileDevice::ReadGroup | QFileDevice::ReadOther));
        // Permission changes also update the source change time on some platforms.
        if (tab->sourceChangedOnDisk())
            answers_.append(QMessageBox::Yes);
        answers_.append(QMessageBox::Ok);
        QVERIFY(QMetaObject::invokeMethod(&window, "saveMeasurements"));
        QVERIFY(tab->hasRecoverySnapshot());
        QCOMPARE(tab->path(), path);
        QVERIFY(tab->viewer()->document());
        QCOMPARE(tab->viewer()->annotModel()->pageAnnots(0).size(), 1u);
        QVERIFY(QFile::setPermissions(path, permissions));
        if (tab->sourceChangedOnDisk())
            answers_.append(QMessageBox::Yes);
        QVERIFY(QMetaObject::invokeMethod(&window, "saveMeasurements"));
        QVERIFY(!tab->hasRecoverySnapshot());
        auto saved = engine.openDocument(path);
        QVERIFY(saved);
        QCOMPARE(AnnotModel(*saved).pageAnnots(0)[0].contents, QStringLiteral("retained"));
    }

    void documentSearchReplacedAndClosed()
    {
        RenderEngine engine;
        auto doc = engine.openDocument(QStringLiteral(MERVIN_FIXTURE_PDF));
        QVERIFY(doc);
        ViewerWidget viewer(&engine);
        viewer.setDocument(doc.get());
        viewer.startFind("STANDARD", false, false);
        viewer.startFind("absent", false, false);
        QTest::qWait(100);
        QCOMPARE(viewer.matchCount(), 0);
        viewer.startFind("STANDARD", false, false);
        QTRY_COMPARE(viewer.matchCount(), doc->pageCount());
        viewer.startFind("The", false, false);
        viewer.setDocument(nullptr);
        doc.reset();
        QTest::qWait(100);
        QCOMPARE(viewer.matchCount(), 0);
    }

private:
    QTemporaryDir profile_;
    QTimer dialogs_;
    QList<QMessageBox::StandardButton> answers_;
    QStringList unexpectedDialogs_;
};

QTEST_MAIN(TstDocumentWorkflow)
#include "tst_document_workflow.moc"
