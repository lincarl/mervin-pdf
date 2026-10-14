#include "TestWaits.h"
#include "app/WindowManager.h"
#include "config/ConfigPaths.h"
#include "config/Settings.h"
#include "io/AppFiles.h"
#include "ocr/TessdataManager.h"
#include "dialogs/ExtractDialog.h"
#include "dialogs/SettingsDialog.h"
#include "render/AnnotModel.h"
#include "render/MeasureContent.h"
#include "recent/RecentStore.h"
#include "render/RenderEngine.h"
#include "security/MeasureExport.h"
#include "session/SessionStore.h"
#include "ui/DetachableTabBar.h"
#include "ui/MainWindow.h"
#include "ui/TabPage.h"
#include "ui/ThemeTokens.h"
#include "ui/ViewerWidget.h"

#include <QAction>
#include <QComboBox>
#include <QContextMenuEvent>
#include <QDateTime>
#include <QDialog>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMap>
#include <QMenu>
#include <QMessageBox>
#include <QPointer>
#include <QPushButton>
#include <QScopeGuard>
#include <QSignalSpy>
#include <QTabBar>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#include <QToolButton>
#include <QWindow>
#include <QtTest>

#include <atomic>
#include <functional>
#include <memory>
#include <optional>

using namespace mervin;

namespace {

// Loads what the app saved, once its background writes are done.
bool loadSaved(SessionStore &store)
{
    return mervin::appfiles::flush(10000) && store.load();
}

bool loadSaved(RecentStore &store, const QString &file)
{
    return mervin::appfiles::flush(10000) && store.load(file);
}

// The files of a window's tabs, in tab order.
QStringList tabFiles(const MainWindow &window)
{
    QStringList files;
    for (const QString &path : window.tabPaths())
        files << QFileInfo(path).fileName();
    return files;
}

// Unload every tab of `window` except the one on screen, as after half an hour
// in the background.
void unloadBackgroundTabs(MainWindow &window)
{
    window.updateDocumentActivity(0, 30);
    window.updateDocumentActivity(31 * 60 * 1000, 30);
}

// The documents WindowManager::restoreSession decides to reopen, once it calls back.
QStringList restoredSession(WindowManager &manager)
{
    std::optional<QStringList> restored;
    manager.restoreSession([&restored](const QStringList &paths) { restored = paths; });
    // A share that does not answer is given up on after kRestoreWaitMs.
    if (!QTest::qWaitFor([&] { return restored.has_value(); }, 30000))
        return {QStringLiteral("restoreSession did not call back")};
    return *restored;
}

// The outcome of a Ctrl+Shift+T press, once its file check has answered.
WindowManager::Reopen reopened(WindowManager &manager, MainWindow *into)
{
    std::optional<WindowManager::Reopen> outcome;
    manager.reopenClosedTab(into, [&outcome](WindowManager::Reopen r) { outcome = r; });
    if (!QTest::qWaitFor([&] { return outcome.has_value(); }, 30000))
        qWarning("reopenClosedTab did not answer");
    return outcome.value_or(WindowManager::Reopen::Failed);
}

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
        // Native file dialogs cannot be driven from a test, and on Windows they do
        // not run its timers.
        QApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
        connect(&dialogs_, &QTimer::timeout, this, [this] {
            auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
            if (!box)
                return;
            if (answers_.isEmpty()) {
                unexpectedDialogs_.append(box->windowTitle() + ": " + box->text());
                box->reject();
            } else if (auto *button = box->button(answers_.takeFirst())) {
                clicked_.append(button->text());
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
        QVERIFY(mervin::appfiles::flush(10000)); // the previous test's writes land first
        ConfigPaths::setOverrideDir(profile_.path());
        answers_.clear();
        clicked_.clear();
        unexpectedDialogs_.clear();
        dialogs_.start(10);
    }
    void cleanup()
    {
        dialogs_.stop();
        QVERIFY(mervin::appfiles::flush(10000));
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
        QVERIFY(mervin::testing::openAndWait(window, QStringLiteral(MERVIN_FIXTURE_PDF)));
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

        QVERIFY(mervin::testing::openAndWait(window, QStringLiteral(MERVIN_FIXTURE_PDF)));
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
        QVERIFY(mervin::testing::openAndWait(window, QStringLiteral(MERVIN_FIXTURE_PDF))); // 4 pages
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

    // A page's right-click menu closes when another document takes the view while
    // it is open, as when another instance opens a file. Its rows would act on the
    // document on screen, and Save Page As would save a page of that document.
    void pageMenuClosesWhenAnotherDocumentTakesTheView()
    {
        QTemporaryDir files;
        const QDir root(QFileInfo(files.path()).canonicalFilePath());
        const QString clicked = root.filePath(QStringLiteral("clicked.pdf"));
        const QString other = root.filePath(QStringLiteral("other.pdf"));
        for (const QString &path : {clicked, other})
            QVERIFY(QFile::copy(QStringLiteral(MERVIN_FIXTURE_PDF), path));
        RenderEngine engine;
        MainWindow window(&engine, nullptr);
        window.setAttribute(Qt::WA_DontShowOnScreen);
        window.resize(900, 700);
        window.show(); // laid out, but never on screen
        QVERIFY(mervin::testing::openAndWait(window, clicked));
        ViewerWidget *viewer = window.findChild<TabPage *>()->viewer();
        QTRY_VERIFY(viewer->viewport()->height() > 200);
        QPoint onPage(-1, -1);
        const QSize area = viewer->viewport()->size();
        for (int y = 0; y < area.height() && onPage.x() < 0; y += 4)
            for (int x = 0; x < area.width() && onPage.x() < 0; x += 4)
                if (viewer->pageUnder({x, y}) >= 0)
                    onPage = {x, y};
        QVERIFY(onPage.x() >= 0);

        // Open the other file while the menu is up, then choose Save Page As if the
        // menu is still there.
        bool chooseFirst = false;
        bool opened = false;
        bool menuStayed = false;
        QString extractFor;
        const auto choose = [](QMenu *menu) {
            for (QAction *a : menu->actions())
                if (a->text() == QStringLiteral("Save Page As"))
                    menu->setActiveAction(a);
            QTest::keyClick(menu, Qt::Key_Return);
        };
        QTimer driver;
        connect(&driver, &QTimer::timeout, this, [&] {
            if (auto *dialog = qobject_cast<ExtractDialog *>(QApplication::activeModalWidget())) {
                extractFor = QFileInfo(window.currentTabPath()).fileName();
                dialog->reject();
                return;
            }
            auto *menu = qobject_cast<QMenu *>(QApplication::activePopupWidget());
            if (!menu || opened)
                return;
            if (chooseFirst)
                choose(menu);
            opened = window.openFile(other);
            menu = qobject_cast<QMenu *>(QApplication::activePopupWidget());
            menuStayed = menu != nullptr;
            if (menu)
                choose(menu);
        });
        driver.start(10);
        const auto rightClick = [&] {
            QContextMenuEvent event(QContextMenuEvent::Mouse, onPage, viewer->viewport()->mapToGlobal(onPage));
            QApplication::sendEvent(viewer->viewport(), &event);
        };
        rightClick();
        QVERIFY(opened);
        QVERIFY(!menuStayed);
        QCOMPARE(extractFor, QString());
        QCOMPARE(QFileInfo(window.currentTabPath()).fileName(), QStringLiteral("other.pdf"));

        // The other document can also take the view after Save Page As is chosen,
        // before the menu's event loop has ended.
        QVERIFY(window.focusTabIfOpen(clicked));
        chooseFirst = true;
        opened = false;
        rightClick();
        driver.stop();
        QVERIFY(opened);
        QCOMPARE(extractFor, QString());
        QCOMPARE(QFileInfo(window.currentTabPath()).fileName(), QStringLiteral("other.pdf"));
    }

    // Extract and Delete copy pages from the file on disk. Their window says that
    // unsaved changes are left out, also for edits that a document kept while it
    // was unloaded, which the viewer no longer counts once it is reloaded.
    void pageToolsNoteEditsKeptWhileUnloaded()
    {
        QTemporaryDir files;
        const QString path = files.filePath(QStringLiteral("edited.pdf"));
        QVERIFY(QFile::copy(QStringLiteral(MERVIN_FIXTURE_PDF), path));
        RenderEngine engine;
        MainWindow window(&engine, nullptr);
        QVERIFY(mervin::testing::openAndWait(window, path));
        auto *tab = window.findChild<TabPage *>();
        QVERIFY(tab);

        QStringList summaries;
        QTimer driver;
        connect(&driver, &QTimer::timeout, this, [&] {
            if (auto *dialog = qobject_cast<ExtractDialog *>(QApplication::activeModalWidget())) {
                summaries << dialog->findChild<QLabel *>(QStringLiteral("extractSummary"))->text();
                dialog->reject();
            }
        });
        driver.start(10);
        const QString note = QStringLiteral("Unsaved changes are not included.");
        QVERIFY(QMetaObject::invokeMethod(&window, "extractPagesOp"));
        QCOMPARE(summaries.size(), 1);
        QVERIFY(!summaries.last().contains(note));

        QVERIFY(tab->viewer()->annotModel()->addTextNote(
                    0, {40, 40}, Qt::yellow, {}, QStringLiteral("Kept edit")) >= 0);
        QVERIFY(tab->suspend());
        QString error;
        QVERIFY2(tab->resume(&error), qPrintable(error));
        QVERIFY(!tab->viewer()->hasUnsavedEdits());
        QVERIFY(tab->hasUnsavedEdits());
        for (const char *op : {"extractPagesOp", "deletePagesOp"}) {
            QVERIFY(QMetaObject::invokeMethod(&window, op));
            QVERIFY2(summaries.last().contains(note), qPrintable(summaries.last()));
        }
        QCOMPARE(summaries.size(), 3);
        driver.stop();
    }

    // A tab whose original was deleted reloads its edits from the recovery copy.
    // Save as Copy rescues them under a new name, and still refuses the
    // original's own name, also when it is spelled through a linked folder.
    void saveAsCopyWorksAfterTheOriginalIsDeleted()
    {
        QTemporaryDir files;
        const QDir root(QFileInfo(files.path()).canonicalFilePath());
        QVERIFY(root.mkpath(QStringLiteral("real")));
#ifdef Q_OS_WIN
        const QString folder = root.filePath(QStringLiteral("real"));
#else
        const QString folder = root.filePath(QStringLiteral("link"));
        QVERIFY(QFile::link(root.filePath(QStringLiteral("real")), folder));
#endif
        const QString original = folder + QStringLiteral("/attachment.pdf"); // as opened
        const QString resolved = root.filePath(QStringLiteral("real/attachment.pdf"));
        const QString copy = root.filePath(QStringLiteral("rescued.pdf"));
        QVERIFY(QFile::copy(QStringLiteral(MERVIN_FIXTURE_PDF), resolved));
        RenderEngine engine;
        MainWindow window(&engine, nullptr);
        QVERIFY(mervin::testing::openAndWait(window, original));
        auto *tab = window.findChild<TabPage *>();
        QVERIFY(tab);
        QVERIFY(tab->viewer()->annotModel()->addTextNote(
                    0, {40, 40}, Qt::yellow, {}, QStringLiteral("Rescued edit")) >= 0);
        QVERIFY(tab->suspend());
        QVERIFY(QFile::remove(original));
        QString error;
        QVERIFY2(tab->resume(&error), qPrintable(error));

        // Type the name into the save dialog, as a user would.
        QString chosen;
        QTimer picker;
        connect(&picker, &QTimer::timeout, this, [&] {
            auto *dialog = qobject_cast<QFileDialog *>(QApplication::activeModalWidget());
            auto *name = dialog ? dialog->findChild<QLineEdit *>(QStringLiteral("fileNameEdit")) : nullptr;
            if (!name)
                return;
            name->setText(QDir::toNativeSeparators(chosen));
            QMetaObject::invokeMethod(dialog, "accept", Qt::DirectConnection);
        });
        picker.start(10);

        chosen = copy;
        answers_.append(QMessageBox::No); // "Open it now?"
        QVERIFY(QMetaObject::invokeMethod(&window, "saveAsCopy"));
        auto saved = engine.openDocument(copy);
        QVERIFY(saved);
        const auto notes = AnnotModel(*saved).pageAnnots(0);
        QCOMPARE(notes.size(), 1u);
        QCOMPARE(notes.front().contents, QStringLiteral("Rescued edit"));

        for (const QString &name : {original, resolved}) {
            chosen = name;
            answers_.append(QMessageBox::Ok); // "Choose a different file name"
            QVERIFY(QMetaObject::invokeMethod(&window, "saveAsCopy"));
            QVERIFY2(!QFileInfo::exists(resolved), qPrintable(name));
        }
        picker.stop();
        QCOMPARE(clicked_.size(), 3);
    }

    // Document actions and their shortcuts, such as Ctrl+S, Ctrl+P and Ctrl+A, are
    // off on the Recent view, and Ctrl+S also on a tab whose document is not loaded.
    // Saving such a tab without edits does nothing, even when its file is gone.
    void documentShortcutsNeedTheDocumentOnScreen()
    {
        QTemporaryDir files;
        const QString path = files.filePath(QStringLiteral("attachment.pdf"));
        QVERIFY(QFile::copy(QStringLiteral(MERVIN_FIXTURE_PDF), path));
        RenderEngine engine;
        MainWindow window(&engine, nullptr);
        window.setAttribute(Qt::WA_DontShowOnScreen);
        window.show();
        QVERIFY(mervin::testing::openAndWait(window, path));
        auto *tab = window.findChild<TabPage *>();
        QVERIFY(tab);
        const auto action = [&](QKeySequence::StandardKey key) -> QAction * {
            for (QAction *a : window.findChildren<QAction *>())
                if (a->shortcuts().contains(QKeySequence(key)))
                    return a;
            return nullptr;
        };
        QAction *save = action(QKeySequence::Save);
        QAction *print = action(QKeySequence::Print);
        QAction *selectAll = action(QKeySequence::SelectAll);
        QAction *measure = nullptr;
        QAction *singlePage = nullptr;
        for (QAction *a : window.findChildren<QAction *>()) {
            if (a->shortcut() == QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_M))
                measure = a;
            if (a->text().remove(QLatin1Char('&')) == QStringLiteral("Single Page Scroll"))
                singlePage = a;
        }
        const QList<QAction *> documentActions{save, print, selectAll, measure, singlePage};
        for (QAction *a : documentActions) {
            QVERIFY(a);
            QVERIFY2(a->isEnabled(), qPrintable(a->text()));
        }

        auto *recent = window.findChild<QPushButton *>(QStringLiteral("recentPillBtn"));
        auto *bar = window.findChild<QTabBar *>(QStringLiteral("docTabBar"));
        QVERIFY(recent);
        QVERIFY(bar);
        QTest::mouseClick(recent, Qt::LeftButton);
        for (QAction *a : documentActions)
            QVERIFY2(!a->isEnabled(), qPrintable(a->text()));
        // A layout change of the hidden document, as from Settings, keeps them off.
        tab->viewer()->setLayoutMode({ViewLayout::Scroll::Single, true});
        QVERIFY(!singlePage->isEnabled());
        QTest::mouseClick(bar, Qt::LeftButton, Qt::NoModifier, bar->tabRect(0).center());
        for (QAction *a : documentActions)
            QVERIFY2(a->isEnabled(), qPrintable(a->text()));

        QVERIFY(tab->suspend());
        QVERIFY(QFile::remove(path));
        QSignalSpy resumed(tab, &TabPage::resumeFinished);
        tab->resumeAsync();
        QTRY_COMPARE_WITH_TIMEOUT(resumed.size(), 1, 10000);
        QVERIFY(tab->isSuspended());
        QVERIFY(!save->isEnabled());
        // No "cannot open" box, which the dialog handler would report.
        QVERIFY(QMetaObject::invokeMethod(&window, "saveMeasurements"));
        QVERIFY(tab->isSuspended());
    }

    // OCR goes on once the installed languages are listed, but not after the user
    // switched to the Recent view meanwhile. With no language installed, it would
    // ask to add one, which the dialog handler reports.
    void ocrDoesNotGoOnOverTheRecentView()
    {
        QTemporaryDir files;
        const QString path = files.filePath(QStringLiteral("ocr.pdf"));
        QVERIFY(QFile::copy(QStringLiteral(MERVIN_FIXTURE_PDF), path));
        RenderEngine engine;
        MainWindow window(&engine, nullptr);
        window.setAttribute(Qt::WA_DontShowOnScreen);
        window.show();
        QVERIFY(mervin::testing::openAndWait(window, path));
        std::atomic<bool> held{true};
        mervin::TessdataManager::runInFolder([&held] {
            for (int i = 0; held.load() && i < 2000; ++i)
                QThread::msleep(5);
        });
        QVERIFY(QMetaObject::invokeMethod(&window, "onOcrRegionSelected", Q_ARG(int, 0),
                                          Q_ARG(QRectF, QRectF(10, 10, 100, 40))));
        auto *recent = window.findChild<QPushButton *>(QStringLiteral("recentPillBtn"));
        QVERIFY(recent);
        QTest::mouseClick(recent, Qt::LeftButton);
        held = false;
        QTRY_VERIFY(!mervin::TessdataManager::busy());
        QTest::qWait(50);
    }

    void closingDirtyTabCanBeCanceled()
    {
        RenderEngine engine;
        MainWindow window(&engine, nullptr);
        QVERIFY(mervin::testing::openAndWait(window, QStringLiteral(MERVIN_FIXTURE_PDF)));
        auto *tab = window.findChild<TabPage *>();
        QVERIFY(tab);
        auto *model = tab->viewer()->annotModel();
        QVERIFY(model->addTextNote(0, {50, 50}, Qt::yellow, {}, "pending") >= 0);
        answers_.append(QMessageBox::Cancel);
        QVERIFY(QMetaObject::invokeMethod(&window, "closeTab", Q_ARG(int, 0)));
        QTRY_VERIFY(answers_.isEmpty());
        QCOMPARE(window.tabCount(), 1);
        QVERIFY(tab->viewer()->hasUnsavedEdits());
        answers_.append(QMessageBox::Discard);
        QVERIFY(QMetaObject::invokeMethod(&window, "closeTab", Q_ARG(int, 0)));
        QTRY_COMPARE(window.tabCount(), 0);
        // The app's own text, not the platform's "Discard", so every catalog has it.
        QCOMPARE(clicked_.last(), QStringLiteral("Don't save"));
    }

    // The unsaved-changes question runs its own event loop, in which a session
    // restore can insert tabs before the one being closed. The answer still
    // closes the tab it was about.
    void closingAfterTheQuestionClosesTheSameTab()
    {
        QTemporaryDir files;
        const QDir root(QFileInfo(files.path()).canonicalFilePath());
        const QString edited = root.filePath(QStringLiteral("edited.pdf"));
        const QString other = root.filePath(QStringLiteral("other.pdf"));
        for (const QString &path : {edited, other})
            QVERIFY(QFile::copy(QStringLiteral(MERVIN_FIXTURE_PDF), path));
        RenderEngine engine;
        MainWindow window(&engine, nullptr);
        QVERIFY(mervin::testing::openAndWait(window, edited));
        auto *tab = window.findChild<TabPage *>();
        QVERIFY(tab);
        QVERIFY(tab->viewer()->annotModel()->addTextNote(0, {50, 50}, Qt::yellow, {}, "pending") >= 0);

        // Answer the question here, after a tab arrives in front of the edited one.
        dialogs_.stop();
        bool inserted = false;
        QTimer answer;
        connect(&answer, &QTimer::timeout, this, [&] {
            auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
            if (!box || inserted)
                return;
            inserted = window.openFile(other, false, 0, /*makeCurrent=*/false);
            box->button(QMessageBox::Discard)->click();
        });
        answer.start(5);
        QVERIFY(QMetaObject::invokeMethod(&window, "closeTab", Q_ARG(int, 0)));
        QTRY_COMPARE(tabFiles(window), QStringList{QStringLiteral("other.pdf")});
        answer.stop();
        QVERIFY(inserted);
        QCOMPARE(tabFiles(window), QStringList{QStringLiteral("other.pdf")});
    }

    // Close all tabs closes every tab it recorded, also when a session restore
    // inserts a tab during the first question. The tab that arrived stays, since
    // the user did not ask to close it.
    void closeAllTabsClosesEveryRecordedTab()
    {
        QTemporaryDir files;
        const QDir root(QFileInfo(files.path()).canonicalFilePath());
        const QString edited = root.filePath(QStringLiteral("edited.pdf"));
        const QString second = root.filePath(QStringLiteral("second.pdf"));
        const QString third = root.filePath(QStringLiteral("third.pdf"));
        const QString arrived = root.filePath(QStringLiteral("arrived.pdf"));
        for (const QString &path : {edited, second, third, arrived})
            QVERIFY(QFile::copy(QStringLiteral(MERVIN_FIXTURE_PDF), path));
        RenderEngine engine;
        MainWindow window(&engine, nullptr);
        QVERIFY(mervin::testing::openAndWait(window, edited));
        auto *tab = window.findChild<TabPage *>();
        QVERIFY(tab);
        QVERIFY(tab->viewer()->annotModel()->addTextNote(0, {50, 50}, Qt::yellow, {}, "pending") >= 0);
        QVERIFY(mervin::testing::openAndWait(window, second));
        QVERIFY(mervin::testing::openAndWait(window, third));

        // A staged restore opens its tabs lazily, in their saved place.
        dialogs_.stop();
        int questions = 0;
        bool inserted = false;
        QTimer answer;
        connect(&answer, &QTimer::timeout, this, [&] {
            auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
            if (!box)
                return;
            if (++questions == 1)
                inserted = window.openFile(arrived, false, 0, /*makeCurrent=*/false, {}, /*lazy=*/true);
            box->button(QMessageBox::Discard)->click();
        });
        answer.start(5);
        QVERIFY(QMetaObject::invokeMethod(&window, "closeAllTabs"));
        QTRY_COMPARE(tabFiles(window), QStringList{QStringLiteral("arrived.pdf")});
        answer.stop();
        QVERIFY(inserted);
        QCOMPARE(questions, 1);
        QCOMPARE(tabFiles(window), QStringList{QStringLiteral("arrived.pdf")});
    }

    // Closing a window asks about each tab once, also when a tab arrives in front
    // of the one asked about during its question. Don't save leaves the edits in
    // the tab, so asking by position would ask about it again.
    void closingAWindowAsksAboutEachTabOnce()
    {
        QTemporaryDir files;
        const QDir root(QFileInfo(files.path()).canonicalFilePath());
        const QString edited = root.filePath(QStringLiteral("edited.pdf"));
        const QString other = root.filePath(QStringLiteral("other.pdf"));
        const QString arrived = root.filePath(QStringLiteral("arrived.pdf"));
        for (const QString &path : {edited, other, arrived})
            QVERIFY(QFile::copy(QStringLiteral(MERVIN_FIXTURE_PDF), path));
        RenderEngine engine;
        MainWindow window(&engine, nullptr);
        QVERIFY(mervin::testing::openAndWait(window, edited));
        auto *tab = window.findChild<TabPage *>();
        QVERIFY(tab);
        QVERIFY(tab->viewer()->annotModel()->addTextNote(0, {50, 50}, Qt::yellow, {}, "pending") >= 0);
        QVERIFY(mervin::testing::openAndWait(window, other));

        dialogs_.stop();
        QStringList asked;
        bool inserted = false;
        QTimer answer;
        connect(&answer, &QTimer::timeout, this, [&] {
            auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
            if (!box)
                return;
            asked << box->text();
            if (!inserted)
                inserted = window.openFile(arrived, false, 0, /*makeCurrent=*/false, {}, /*lazy=*/true);
            box->button(QMessageBox::Discard)->click();
        });
        answer.start(5);
        QVERIFY(!window.close());
        QTRY_VERIFY(inserted && !tab->isClosing());
        answer.stop();
        QVERIFY(inserted);
        QCOMPARE(asked, QStringList{QStringLiteral("Save changes to edited.pdf?")});
    }

    // Move to new window in a tab's menu moves the tab that was right-clicked,
    // also when a session restore inserts a tab in front of it while the menu is
    // open.
    void moveToNewWindowMovesTheClickedTab()
    {
        QTemporaryDir files;
        const QDir root(QFileInfo(files.path()).canonicalFilePath());
        const QString first = root.filePath(QStringLiteral("first.pdf"));
        const QString clicked = root.filePath(QStringLiteral("clicked.pdf"));
        const QString arrived = root.filePath(QStringLiteral("arrived.pdf"));
        for (const QString &path : {first, clicked, arrived})
            QVERIFY(QFile::copy(QStringLiteral(MERVIN_FIXTURE_PDF), path));
        WindowManager manager;
        MainWindow *window = manager.createWindow();
        QVERIFY(mervin::testing::openAndWait(*window, first));
        QVERIFY(mervin::testing::openAndWait(*window, clicked));
        auto *bar = window->findChild<QTabBar *>(QStringLiteral("docTabBar"));
        QVERIFY(bar);
        QCOMPARE(bar->tabText(1), QStringLiteral("clicked.pdf"));

        bool inserted = false;
        QTimer driver;
        connect(&driver, &QTimer::timeout, this, [&] {
            auto *menu = qobject_cast<QMenu *>(QApplication::activePopupWidget());
            if (!menu || inserted)
                return;
            inserted = window->openFile(arrived, false, 0, /*makeCurrent=*/false, {}, /*lazy=*/true);
            for (QAction *action : menu->actions())
                if (action->text() == QStringLiteral("Move to new window"))
                    menu->setActiveAction(action);
            QTest::keyClick(menu, Qt::Key_Return);
        });
        driver.start(10);
        const QPoint at = bar->tabRect(1).center();
        QContextMenuEvent event(QContextMenuEvent::Mouse, at, bar->mapToGlobal(at));
        QApplication::sendEvent(bar, &event);
        driver.stop();
        QVERIFY(inserted);

        QCOMPARE(tabFiles(*window),
                 (QStringList{QStringLiteral("arrived.pdf"), QStringLiteral("first.pdf")}));
        MainWindow *moved = nullptr;
        for (QWidget *w : QApplication::topLevelWidgets())
            if (auto *candidate = qobject_cast<MainWindow *>(w); candidate && candidate != window)
                moved = candidate;
        QVERIFY(moved);
        QCOMPARE(tabFiles(*moved), QStringList{QStringLiteral("clicked.pdf")});
    }

    // Dragging a tab out of the bar moves the tab that was pressed, also when a
    // session restore inserts a tab in front of it while the button is held. Tab
    // changes wait until the drag has ended, also once the button is up.
    void draggingATabOutMovesThePressedTab()
    {
        QTemporaryDir files;
        const QDir root(QFileInfo(files.path()).canonicalFilePath());
        const QString first = root.filePath(QStringLiteral("first.pdf"));
        const QString dragged = root.filePath(QStringLiteral("dragged.pdf"));
        const QString arrived = root.filePath(QStringLiteral("arrived.pdf"));
        for (const QString &path : {first, dragged, arrived})
            QVERIFY(QFile::copy(QStringLiteral(MERVIN_FIXTURE_PDF), path));
        WindowManager manager;
        MainWindow *window = manager.createWindow();
        QVERIFY(mervin::testing::openAndWait(*window, first));
        QVERIFY(mervin::testing::openAndWait(*window, dragged));
        QVERIFY(QTest::qWaitForWindowExposed(window));
        auto *bar = window->findChild<DetachableTabBar *>(QStringLiteral("docTabBar"));
        QVERIFY(bar);
        QWindow *handle = window->windowHandle();
        const QPoint at = bar->mapTo(window, bar->tabRect(1).center());
        const QPoint below = at + QPoint(0, 200); // well outside the bar

        // The offscreen platform ends a drag at once, as dropped on nothing. On a
        // platform whose drag runs its own event loop, release the button there.
        QTimer release;
        connect(&release, &QTimer::timeout, this, [&] {
            if (DetachableTabBar::dragInProgress())
                QTest::mouseRelease(handle, Qt::LeftButton, {}, below);
        });
        release.start(5);
        // The drag still runs when the bar asks to detach the tab.
        std::optional<bool> couldChangeTabs;
        connect(bar, &DetachableTabBar::detachRequested, this, [&] {
            if (QApplication::mouseButtons() != Qt::NoButton)
                QTest::mouseRelease(handle, Qt::LeftButton, {}, below);
            couldChangeTabs = window->canChangeTabs();
        });
        QTest::mousePress(handle, Qt::LeftButton, {}, at);
        QVERIFY(window->openFile(arrived, false, 0, /*makeCurrent=*/false, {}, /*lazy=*/true));
        QTest::mouseMove(handle, below);
        release.stop();
        if (QApplication::mouseButtons() != Qt::NoButton)
            QTest::mouseRelease(handle, Qt::LeftButton, {}, below);
        QVERIFY(couldChangeTabs.has_value());
        QVERIFY(!*couldChangeTabs);
        QVERIFY(window->canChangeTabs());

        QCOMPARE(tabFiles(*window),
                 (QStringList{QStringLiteral("arrived.pdf"), QStringLiteral("first.pdf")}));
        MainWindow *moved = nullptr;
        for (QWidget *w : QApplication::topLevelWidgets())
            if (auto *candidate = qobject_cast<MainWindow *>(w); candidate && candidate != window)
                moved = candidate;
        QVERIFY(moved);
        QCOMPARE(tabFiles(*moved), QStringList{QStringLiteral("dragged.pdf")});
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
        QVERIFY(mervin::testing::openAndWait(*first, firstPath));
        QVERIFY(mervin::testing::openAndWait(*second, secondPath));
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
        manager.closeAllForQuit();
        QTRY_VERIFY(answers_.isEmpty());
        QTRY_VERIFY(!firstTab->isClosing() && !secondTab->isClosing());
        QCOMPARE(manager.openTabPaths().size(), 2);
        QVERIFY(firstTab->isSuspended());
        QVERIFY(secondTab->isSuspended());
        QVERIFY(firstTab->hasUnsavedEdits());
        QVERIFY(secondTab->hasUnsavedEdits());

        answers_ << QMessageBox::Save << QMessageBox::Cancel;
        manager.closeAllForQuit();
        QTRY_VERIFY(answers_.isEmpty());
        QTRY_VERIFY(!firstTab->isClosing() && !secondTab->isClosing());
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

    // A tray restore brings back every hidden window, the one hidden last in front.
    // Creation order must not decide: a window opened later, and hidden first, would
    // otherwise come back over the window the user closed last.
    void trayRestoresTheWindowHiddenLastInFront()
    {
        QTemporaryDir files;
        const QString path = files.filePath(QStringLiteral("doc.pdf"));
        QVERIFY(QFile::copy(QStringLiteral(MERVIN_FIXTURE_PDF), path));
        WindowManager manager;
        MainWindow *documents = manager.createWindow();
        MainWindow *later = manager.createWindow();
        QVERIFY(mervin::testing::openAndWait(*documents, path));
        QVERIFY(QTest::qWaitForWindowExposed(documents));
        QVERIFY(QTest::qWaitForWindowExposed(later));

        later->hideToTray();
        documents->hideToTray();
        QVERIFY(later->trayOrder() < documents->trayOrder());
        manager.restoreFromTray();
        QVERIFY(documents->isVisible() && !documents->isInTray());
        QVERIFY(later->isVisible() && !later->isInTray());
        QTRY_COMPARE(QApplication::activeWindow(), documents);

        // Hidden the other way round, the other window comes back in front.
        documents->hideToTray();
        later->hideToTray();
        manager.restoreFromTray();
        QTRY_COMPARE(QApplication::activeWindow(), later);
    }

    // A session document on an unavailable drive stays in the saved session through
    // later saves until the user opens it again. Then it is an ordinary tab.
    void sessionKeepsDocumentsOnUnavailableDrives()
    {
        QTemporaryDir files;
        const QDir root(QFileInfo(files.path()).canonicalFilePath());
        const QString open = root.filePath(QStringLiteral("open.pdf"));
        const QString other = root.filePath(QStringLiteral("other.pdf"));
#ifdef Q_OS_WIN
        const QString away = QStringLiteral("//mervin-test-no-such-server/share/plan.pdf");
#else
        const QString away = QStringLiteral("/mnt/mervin-test-absent/plan.pdf");
#endif
        QVERIFY(QFile::copy(QStringLiteral(MERVIN_FIXTURE_PDF), open));
        QVERIFY(QFile::copy(QStringLiteral(MERVIN_FIXTURE_PDF), other));
        SessionStore saved;
        saved.setPaths({away, open});
        QVERIFY(saved.save());

        WindowManager manager;
        MainWindow *window = manager.createWindow();
        QCOMPARE(restoredSession(manager), QStringList{open});
        QVERIFY(mervin::testing::openAndWait(*window, open));
        SessionStore onDisk;
        QVERIFY(loadSaved(onDisk));
        QCOMPARE(onDisk.paths(), QStringList{open});
        QCOMPARE(onDisk.activePath(), open);
        QCOMPARE(onDisk.unavailable().size(), 1);
        QCOMPARE(onDisk.unavailable().first().path, away);
        QCOMPARE(onDisk.unavailable().first().since, QDate::currentDate());

        // Every later save keeps it: opening and closing other documents.
        QVERIFY(mervin::testing::openAndWait(*window, other));
        QVERIFY(QMetaObject::invokeMethod(window, "closeTab", Q_ARG(int, 1)));
        QTRY_COMPARE(window->tabCount(), 1);
        QVERIFY(loadSaved(onDisk));
        QCOMPARE(onDisk.paths(), QStringList{open});
        QCOMPARE(onDisk.unavailable().size(), 1);
        QCOMPARE(onDisk.unavailable().first().path, away);
    }

    // A share mounted by hand on a folder, outside fstab, leaves the folder empty
    // once it is unmounted. The share's mount was recorded when the document was
    // opened, so the session keeps the document instead of dropping it as deleted.
    void sessionKeepsDocumentsWhoseRecordedMountIsGone()
    {
#ifndef Q_OS_WIN
        QTemporaryDir files;
        const QDir root(QFileInfo(files.path()).canonicalFilePath());
        QVERIFY(root.mkpath(QStringLiteral("share")));
        const QString share = root.filePath(QStringLiteral("share"));
        const QString open = root.filePath(QStringLiteral("open.pdf"));
        // One mount is in the session only, as after Recent trimmed the document.
        // The other is in Recent only, as a session saved by an older version has it.
        const QString inSession = root.filePath(QStringLiteral("share/plan.pdf"));
        const QString inRecent = root.filePath(QStringLiteral("share/older.pdf"));
        QVERIFY(QFile::copy(QStringLiteral(MERVIN_FIXTURE_PDF), open));
        const QString recentFile = RecentStore::defaultFile();
        RecentStore recent;
        recent.load(recentFile);
        const RecentStore before = recent;
        const auto restoreRecent = qScopeGuard([&] {
            mervin::appfiles::flush(10000); // the test's own writes first
            before.save(recentFile);
        });
        recent.add(inRecent, QDateTime::currentMSecsSinceEpoch(), 0, share);
        QVERIFY(recent.save(recentFile));
        SessionStore saved;
        saved.setMount(inSession, share);
        saved.setPaths({inSession, inRecent, open});
        QVERIFY(saved.save());

        WindowManager manager;
        MainWindow *window = manager.createWindow();
        QCOMPARE(restoredSession(manager), QStringList{open});
        QVERIFY(mervin::testing::openAndWait(*window, open));
        // The file system the document is on is recorded once its check answers.
        const auto recentMount = [&manager](const QString &file) {
            for (const RecentEntry &e : manager.recentEntries())
                if (e.path == file)
                    return e.mount;
            return QStringLiteral("no entry");
        };
        QTRY_COMPARE(recentMount(open), RecentStore::mountOf(open));
        // ...and saved in the session by then, without another save.
        SessionStore onDisk;
        QTRY_VERIFY(loadSaved(onDisk) && onDisk.mount(open) == RecentStore::mountOf(open));
        QCOMPARE(onDisk.paths(), QStringList{open});
        QCOMPARE(onDisk.unavailable().size(), 2);
        QCOMPARE(onDisk.mount(inSession), share);
        QCOMPARE(onDisk.mount(inRecent), share);
        // Opening a document records the file system it is on, in Recent and in
        // the session.
        QVERIFY(!RecentStore::mountOf(open).isEmpty());
        QCOMPARE(onDisk.mount(open), RecentStore::mountOf(open));
        RecentStore reloaded;
        QVERIFY(loadSaved(reloaded, recentFile));
        QCOMPARE(reloaded.recordedMount(open), RecentStore::mountOf(open));
#endif
    }

    // Recording a document that is not there, as a crash recovery tab opened lazily
    // or a tab focused again after its share went away does, keeps the mount it
    // was opened from. Once the file is back, its mount is recorded afresh.
    void recordingAMissingFileKeepsItsMount()
    {
        QTemporaryDir files;
        const QDir root(QFileInfo(files.path()).canonicalFilePath());
        const QString path = root.filePath(QStringLiteral("plan.pdf"));
        const QString share = root.filePath(QStringLiteral("share"));
        const QString recentFile = RecentStore::defaultFile();
        RecentStore recent;
        recent.load(recentFile);
        const RecentStore before = recent;
        const auto restoreRecent = qScopeGuard([&] {
            mervin::appfiles::flush(10000); // the test's own writes first
            before.save(recentFile);
        });
        recent.add(path, QDateTime::currentMSecsSinceEpoch(), 0, share);
        QVERIFY(recent.save(recentFile));
        const auto mountOf = [](const WindowManager &manager, const QString &file) {
            for (const RecentEntry &e : manager.recentEntries())
                if (e.path == file)
                    return e.mount;
            return QStringLiteral("no entry");
        };

        WindowManager manager;
        manager.recordOpen(path, false);
        // The check finds no file and leaves the mount alone.
        QTRY_VERIFY(manager.fileProbe()->facts({path, share}).has_value());
        QCOMPARE(mountOf(manager, path), share);
        QVERIFY(QFile::copy(QStringLiteral(MERVIN_FIXTURE_PDF), path));
        manager.recordOpen(path, false);
        QTRY_COMPARE(mountOf(manager, path), RecentStore::mountOf(path));
    }

    // An unloaded tab whose file was deleted, such as a mail attachment's temporary
    // copy, closes without a question and without entering the reopen history.
    // Other tabs stay: a file that is there, a file whose folder is gone too, as
    // when another volume took the drive letter, a file on a drive that is away,
    // and the document on screen. The window keeps showing Recent while tabs go.
    void unloadedTabsOfRemovedFilesClose()
    {
        QTemporaryDir files;
        const QDir root(QFileInfo(files.path()).canonicalFilePath());
        const QString gone = root.filePath(QStringLiteral("attachment.pdf"));
        const QString kept = root.filePath(QStringLiteral("kept.pdf"));
        const QString shown = root.filePath(QStringLiteral("shown.pdf"));
        const QString inFolder = root.filePath(QStringLiteral("folder/inside.pdf"));
        QVERIFY(root.mkpath(QStringLiteral("folder")));
#ifdef Q_OS_WIN
        const QString away = QStringLiteral("//mervin-test-no-such-server/share/plan.pdf");
#else
        const QString away = QStringLiteral("/mnt/mervin-test-absent/plan.pdf");
#endif
        for (const QString &path : {gone, kept, shown, inFolder})
            QVERIFY(QFile::copy(QStringLiteral(MERVIN_FIXTURE_PDF), path));

        WindowManager manager;
        MainWindow *window = manager.createWindow();
        QVERIFY(mervin::testing::openAndWait(*window, gone));
        QVERIFY(mervin::testing::openAndWait(*window, gone, /*allowDuplicate=*/true)); // a second view
        QVERIFY(mervin::testing::openAndWait(*window, kept));
        QVERIFY(mervin::testing::openAndWait(*window, inFolder));
        QVERIFY(window->openFile(away, false, -1, /*makeCurrent=*/false, {}, /*lazy=*/true));
        QVERIFY(mervin::testing::openAndWait(*window, shown));
        unloadBackgroundTabs(*window);
        const QStringList before{QStringLiteral("attachment.pdf"), QStringLiteral("attachment.pdf"),
                                 QStringLiteral("kept.pdf"), QStringLiteral("inside.pdf"),
                                 QStringLiteral("plan.pdf"), QStringLiteral("shown.pdf")};
        QCOMPARE(tabFiles(*window), before);
        QCOMPARE(window->unloadedTabs().size(), 5);
        auto *recent = window->findChild<QPushButton *>(QStringLiteral("recentPillBtn"));
        QVERIFY(recent);
        QTest::mouseClick(recent, Qt::LeftButton);
        QVERIFY(recent->property("recentActive").toBool());

        QVERIFY(QFile::remove(gone));
        QVERIFY(QDir(root.filePath(QStringLiteral("folder"))).removeRecursively());
        manager.closeRemovedTabs();
        const QStringList after{QStringLiteral("kept.pdf"), QStringLiteral("inside.pdf"),
                                QStringLiteral("plan.pdf"), QStringLiteral("shown.pdf")};
        // Checking the share that is away can take a while on Windows.
        QTRY_COMPARE_WITH_TIMEOUT(tabFiles(*window), after, 30000);
        QVERIFY(recent->property("recentActive").toBool());
        SessionStore onDisk;
        QVERIFY(loadSaved(onDisk));
        QVERIFY(!onDisk.paths().contains(gone));
        QCOMPARE(onDisk.paths().size(), 4);

        // The file coming back does not make Ctrl+Shift+T offer the tab.
        QVERIFY(QFile::copy(QStringLiteral(MERVIN_FIXTURE_PDF), gone));
        QCOMPARE(reopened(manager, window), WindowManager::Reopen::NothingToReopen);
        QCOMPARE(tabFiles(*window), after);
    }

    // A tab of a file on a share mounted by hand keeps Retry while the share is
    // unmounted: its folder is still there, but not the mount it was opened from.
    // A deleted file closing in the same check shows that the check ran.
    void tabOnAnUnmountedShareStays()
    {
#ifndef Q_OS_WIN
        QTemporaryDir files;
        const QDir root(QFileInfo(files.path()).canonicalFilePath());
        QVERIFY(root.mkpath(QStringLiteral("share")));
        const QString share = root.filePath(QStringLiteral("share"));
        const QString away = root.filePath(QStringLiteral("share/plan.pdf"));
        const QString gone = root.filePath(QStringLiteral("attachment.pdf"));
        QVERIFY(QFile::copy(QStringLiteral(MERVIN_FIXTURE_PDF), gone));
        const QString recentFile = RecentStore::defaultFile();
        RecentStore recent;
        recent.load(recentFile);
        const RecentStore before = recent;
        const auto restoreRecent = qScopeGuard([&] {
            mervin::appfiles::flush(10000); // the test's own writes first
            before.save(recentFile);
        });
        recent.add(away, QDateTime::currentMSecsSinceEpoch(), 0, share);
        QVERIFY(recent.save(recentFile));

        WindowManager manager;
        MainWindow *window = manager.createWindow();
        QVERIFY(window->openFile(away, false, -1, true, {}, /*lazy=*/true));
        QVERIFY(mervin::testing::openAndWait(*window, gone));
        QVERIFY(mervin::testing::openAndWait(*window, QStringLiteral(MERVIN_FIXTURE_PDF)));
        unloadBackgroundTabs(*window);
        QVERIFY(QFile::remove(gone));
        manager.closeRemovedTabs();
        QTRY_COMPARE(tabFiles(*window),
                     (QStringList{QStringLiteral("plan.pdf"), QStringLiteral("properties.pdf")}));
#endif
    }

    // A file opened while startup holds the session, as a command-line file is, is
    // saved once the hold ends, even when there is no session to restore.
    void releasingTheHoldSavesTheSession()
    {
        QTemporaryDir files;
        const QDir root(QFileInfo(files.path()).canonicalFilePath());
        const QString path = root.filePath(QStringLiteral("plan.pdf"));
        QVERIFY(QFile::copy(QStringLiteral(MERVIN_FIXTURE_PDF), path));
        SessionStore empty;
        QVERIFY(empty.save());
        WindowManager manager;
        MainWindow *window = manager.createWindow();
        manager.holdSession(true);
        QVERIFY(mervin::testing::openAndWait(*window, path));
        SessionStore onDisk;
        QVERIFY(loadSaved(onDisk));
        QVERIFY(onDisk.paths().isEmpty());
        manager.holdSession(false);
        QVERIFY(loadSaved(onDisk));
        QCOMPARE(onDisk.paths(), QStringList{path});
    }

#ifndef Q_OS_WIN // Windows records no mounts, so the answer saves nothing there
    // A check answers after the last window has closed, as one on a slow share can.
    // The session the window left behind stays: there is nothing left to record.
    void lateAnswerKeepsTheSessionOfTheLastWindow()
    {
        QTemporaryDir files;
        const QDir root(QFileInfo(files.path()).canonicalFilePath());
        const QString path = root.filePath(QStringLiteral("plan.pdf"));
        QVERIFY(QFile::copy(QStringLiteral(MERVIN_FIXTURE_PDF), path));
        WindowManager manager;
        QPointer<MainWindow> window = manager.createWindow();
        QVERIFY(mervin::testing::openAndWait(*window, path));
        // The file's mount is recorded once its check answers, which saves the session.
        QVERIFY(manager.fileProbe()->isChecking({path, {}}));
        delete window.data();
        QTRY_VERIFY(!manager.fileProbe()->isChecking({path, {}}));
        SessionStore onDisk;
        QVERIFY(loadSaved(onDisk));
        QCOMPARE(onDisk.paths(), QStringList{path});
    }
#endif

    // Moving a tab to another window keeps the mount the session recorded for its
    // file, which tells a share that is away from a deleted file.
    void movingATabKeepsItsRecordedMount()
    {
        QTemporaryDir files;
        const QDir root(QFileInfo(files.path()).canonicalFilePath());
        const QString moved = root.filePath(QStringLiteral("moved.pdf"));
        const QString other = root.filePath(QStringLiteral("other.pdf"));
        for (const QString &path : {moved, other})
            QVERIFY(QFile::copy(QStringLiteral(MERVIN_FIXTURE_PDF), path));
        WindowManager manager;
        MainWindow *window = manager.createWindow();
        QVERIFY(mervin::testing::openAndWait(*window, moved));
        QVERIFY(mervin::testing::openAndWait(*window, other));
        SessionStore onDisk;
#ifndef Q_OS_WIN
        // Windows records no mounts. Elsewhere the session saves the mount once the
        // file's check has found it.
        QTRY_VERIFY(loadSaved(onDisk) && !onDisk.mount(moved).isEmpty());
#endif
        QVERIFY(loadSaved(onDisk));
        const QString mount = onDisk.mount(moved);

        manager.detachTab(window, 0, {});
        QCOMPARE(window->tabPaths(), QStringList{other});
        QVERIFY(loadSaved(onDisk));
        QCOMPARE(onDisk.mount(moved), mount);
        MainWindow *detached = nullptr;
        for (QWidget *w : QApplication::topLevelWidgets())
            if (auto *candidate = qobject_cast<MainWindow *>(w); candidate && candidate != window)
                detached = candidate;
        QVERIFY(detached);

        manager.mergeTab(detached, 0, window, 0);
        QCOMPARE(window->tabPaths(), (QStringList{moved, other}));
        QVERIFY(loadSaved(onDisk));
        QCOMPARE(onDisk.mount(moved), mount);
        QCOMPARE(onDisk.paths().size(), 2);
    }

    // Coming back to Mervin is when it notices a deleted file. The tab goes without
    // being selected.
    void activatingAWindowClosesRemovedTabs()
    {
        QTemporaryDir files;
        const QDir root(QFileInfo(files.path()).canonicalFilePath());
        const QString gone = root.filePath(QStringLiteral("attachment.pdf"));
        QVERIFY(QFile::copy(QStringLiteral(MERVIN_FIXTURE_PDF), gone));

        WindowManager manager;
        MainWindow *documents = manager.createWindow();
        QVERIFY(mervin::testing::openAndWait(*documents, gone));
        QVERIFY(mervin::testing::openAndWait(*documents, QStringLiteral(MERVIN_FIXTURE_PDF)));
        unloadBackgroundTabs(*documents);
        MainWindow *other = manager.createWindow();
        other->activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(other));
        QTest::qWait(200); // let the activation's own check finish
        QVERIFY(QFile::remove(gone));
        QCOMPARE(documents->tabCount(), 2);

        documents->activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(documents));
        QTRY_COMPARE(tabFiles(*documents), QStringList{QStringLiteral("properties.pdf")});
    }

    // Selecting an unloaded tab whose file was deleted closes it, instead of
    // leaving the load error with Retry on screen.
    void failedReloadOfARemovedFileClosesTheTab()
    {
        QTemporaryDir files;
        const QDir root(QFileInfo(files.path()).canonicalFilePath());
        const QString gone = root.filePath(QStringLiteral("attachment.pdf"));
        QVERIFY(QFile::copy(QStringLiteral(MERVIN_FIXTURE_PDF), gone));

        WindowManager manager;
        MainWindow *window = manager.createWindow();
        QVERIFY(mervin::testing::openAndWait(*window, gone));
        QVERIFY(mervin::testing::openAndWait(*window, QStringLiteral(MERVIN_FIXTURE_PDF)));
        unloadBackgroundTabs(*window);
        auto *tabs = window->findChild<QTabWidget *>();
        QVERIFY(tabs);
        QPointer<TabPage> tab = qobject_cast<TabPage *>(tabs->widget(0));
        QVERIFY(tab);
        QVERIFY(tab->isSuspended());
        QVERIFY(QFile::remove(gone));

        QSignalSpy resumed(tab.data(), &TabPage::resumeFinished);
        tabs->setCurrentIndex(0);
        QTRY_COMPARE_WITH_TIMEOUT(resumed.size(), 1, 10000);
        QVERIFY(!resumed.first().at(0).toBool());
        QTRY_VERIFY(tab.isNull());
        QCOMPARE(tabFiles(*window), QStringList{QStringLiteral("properties.pdf")});
        QVERIFY(window->findChild<TabPage *>()->isLoaded());
    }

    // A tab with unsaved edits reloads from its recovery copy, so it stays when
    // its original is deleted. A clean tab of a deleted file closing in the same
    // check shows that the check ran.
    void editedTabOfARemovedFileStays()
    {
        QTemporaryDir files;
        const QDir root(QFileInfo(files.path()).canonicalFilePath());
        const QString edited = root.filePath(QStringLiteral("edited.pdf"));
        const QString clean = root.filePath(QStringLiteral("clean.pdf"));
        for (const QString &path : {edited, clean})
            QVERIFY(QFile::copy(QStringLiteral(MERVIN_FIXTURE_PDF), path));

        WindowManager manager;
        MainWindow *window = manager.createWindow();
        QVERIFY(mervin::testing::openAndWait(*window, edited));
        auto *editedTab = window->findChild<TabPage *>();
        QVERIFY(editedTab);
        QVERIFY(editedTab->viewer()->annotModel()->addTextNote(
                    0, {40, 40}, Qt::yellow, {}, QStringLiteral("Kept edit")) >= 0);
        QVERIFY(mervin::testing::openAndWait(*window, clean));
        QVERIFY(mervin::testing::openAndWait(*window, QStringLiteral(MERVIN_FIXTURE_PDF)));
        unloadBackgroundTabs(*window);
        QVERIFY(editedTab->isSuspended());
        QVERIFY(editedTab->hasUnsavedEdits());
        QVERIFY(QFile::remove(edited));
        QVERIFY(QFile::remove(clean));

        manager.closeRemovedTabs();
        QTRY_COMPARE(tabFiles(*window),
                     (QStringList{QStringLiteral("edited.pdf"), QStringLiteral("properties.pdf")}));
        auto *tabs = window->findChild<QTabWidget *>();
        QVERIFY(tabs);
        tabs->setCurrentIndex(0);
        QTRY_VERIFY_WITH_TIMEOUT(editedTab->isLoaded(), 10000);
        QCOMPARE(editedTab->viewer()->annotModel()->pageAnnots(0).front().contents,
                 QStringLiteral("Kept edit"));
        // Closing it still asks about the edits.
        answers_.append(QMessageBox::Discard);
        QVERIFY(QMetaObject::invokeMethod(window, "closeTab", Q_ARG(int, 0)));
        QTRY_COMPARE(window->tabCount(), 1);
    }

    // While a dialog is open the window keeps the tab, since the dialog may depend
    // on the tab bar. The tab closes once the dialog is gone, after a fresh check
    // that spares a file which came back meanwhile.
    void busyWindowClosesARemovedTabLater()
    {
        QTemporaryDir files;
        const QDir root(QFileInfo(files.path()).canonicalFilePath());
        const QString gone = root.filePath(QStringLiteral("attachment.pdf"));
        const QString back = root.filePath(QStringLiteral("restored.pdf"));
        for (const QString &path : {gone, back})
            QVERIFY(QFile::copy(QStringLiteral(MERVIN_FIXTURE_PDF), path));

        WindowManager manager;
        MainWindow *window = manager.createWindow();
        QVERIFY(mervin::testing::openAndWait(*window, gone));
        QVERIFY(mervin::testing::openAndWait(*window, back));
        QVERIFY(mervin::testing::openAndWait(*window, QStringLiteral(MERVIN_FIXTURE_PDF)));
        unloadBackgroundTabs(*window);
        QVERIFY(QFile::remove(gone));
        QVERIFY(QFile::remove(back));

        QDialog dialog(window);
        dialog.setModal(true);
        dialog.show();
        QTRY_COMPARE(QApplication::activeModalWidget(), &dialog);
        manager.closeRemovedTabs();
        QTest::qWait(1500); // the check, and an activity tick, which must keep the tab
        QCOMPARE(window->tabCount(), 3);
        QVERIFY(QFile::copy(QStringLiteral(MERVIN_FIXTURE_PDF), back));
        dialog.close();
        QTRY_COMPARE_WITH_TIMEOUT(tabFiles(*window),
                                  (QStringList{QStringLiteral("restored.pdf"), QStringLiteral("properties.pdf")}),
                                  5000);
    }

    // A tab change waits while a menu, a modal dialog or a held mouse button may
    // hold a tab position, then runs, in the order asked. It never runs inside
    // the call that asks for it, so a tab's own button can ask to close the tab.
    // It runs on the next turn of the event loop once the dialog has closed or the
    // button is up, without waiting for the activity tick.
    void tabChangesWaitWhileTabPositionsMayBeHeld()
    {
        WindowManager manager;
        MainWindow *window = manager.createWindow();
        QVERIFY(mervin::testing::openAndWait(*window, QStringLiteral(MERVIN_FIXTURE_PDF)));
        QVERIFY(QTest::qWaitForWindowExposed(window));
        QStringList ran;
        const auto ask = [&](const QString &name) {
            window->whenTabsCanChange([&ran, name] { ran << name; });
        };
        // Asks while something holds positions, then runs the queue as the posted
        // run and the activity tick would meanwhile.
        const auto stillWaits = [&](const QString &name) {
            ask(name);
            QCoreApplication::processEvents();
            window->runTabChanges();
            return !window->canChangeTabs() && ran.isEmpty();
        };

        QVERIFY(window->canChangeTabs());
        ask(QStringLiteral("first"));
        ask(QStringLiteral("second"));
        QVERIFY(ran.isEmpty());
        QCoreApplication::processEvents();
        QCOMPARE(ran, (QStringList{QStringLiteral("first"), QStringLiteral("second")}));

        ran.clear();
        bool waited = false;
        QMenu menu(window);
        menu.addAction(QStringLiteral("Item"));
        QTimer::singleShot(0, &menu, [&] {
            waited = stillWaits(QStringLiteral("menu"));
            menu.close();
        });
        menu.exec(window->mapToGlobal(QPoint(20, 20)));
        QVERIFY(waited);
        // Nothing but the activity tick follows a menu closed without a click.
        QTRY_COMPARE(ran, QStringList{QStringLiteral("menu")});

        ran.clear();
        waited = false;
        QDialog dialog(window);
        QTimer::singleShot(0, &dialog, [&] {
            waited = stillWaits(QStringLiteral("dialog"));
            dialog.reject();
        });
        dialog.exec();
        QVERIFY(waited);
        QCoreApplication::processEvents();
        QCOMPARE(ran, QStringList{QStringLiteral("dialog")});

        // A press on a tab, as when the user starts to move it.
        ran.clear();
        auto *bar = window->findChild<QTabBar *>(QStringLiteral("docTabBar"));
        QVERIFY(bar);
        const QPoint at = bar->mapTo(window, bar->tabRect(0).center());
        QTest::mousePress(window->windowHandle(), Qt::LeftButton, {}, at);
        QVERIFY(stillWaits(QStringLiteral("mouse")));
        QTest::mouseRelease(window->windowHandle(), Qt::LeftButton, {}, at);
        QCoreApplication::processEvents();
        QCOMPARE(ran, QStringList{QStringLiteral("mouse")});
    }

    // A tab change asked for during a document operation runs once the operation
    // ends. A window without a manager has no release, closed modal window or
    // activity tick to run its queue, so only the end of the operation runs it.
    void tabChangesRunWhenADocumentOperationEnds()
    {
        RenderEngine engine;
        MainWindow window(&engine, nullptr);
        QVERIFY(mervin::testing::openAndWait(window, QStringLiteral(MERVIN_FIXTURE_PDF)));
        auto *tab = window.findChild<TabPage *>();
        QVERIFY(tab);
        QVERIFY(tab->viewer()->annotModel()->addTextNote(0, {50, 50}, Qt::yellow, {}, "pending") >= 0);

        // Ask during the unsaved-changes question, which runs inside the close's
        // document operation, then keep the tab.
        dialogs_.stop();
        QStringList ran;
        bool asked = false;
        bool waited = false;
        QTimer answer;
        connect(&answer, &QTimer::timeout, this, [&] {
            auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
            if (!box || asked)
                return;
            asked = true;
            window.whenTabsCanChange([&ran] { ran << QStringLiteral("operation"); });
            QCoreApplication::processEvents();
            window.runTabChanges();
            waited = ran.isEmpty();
            box->button(QMessageBox::Cancel)->click();
        });
        answer.start(5);
        QVERIFY(QMetaObject::invokeMethod(&window, "closeTab", Q_ARG(int, 0)));
        QTRY_VERIFY(asked);
        answer.stop();
        QVERIFY(asked);
        QVERIFY(waited);
        QCOMPARE(window.tabCount(), 1);
        QCoreApplication::processEvents();
        QCOMPARE(ran, QStringList{QStringLiteral("operation")});
    }

    // A removed file's tab closes only after the tab changes asked for before it,
    // as a window changes its tabs in the order asked.
    void removedFileTabClosesAfterEarlierTabChanges()
    {
        QTemporaryDir files;
        const QDir root(QFileInfo(files.path()).canonicalFilePath());
        const QString gone = root.filePath(QStringLiteral("attachment.pdf"));
        QVERIFY(QFile::copy(QStringLiteral(MERVIN_FIXTURE_PDF), gone));
        RenderEngine engine;
        MainWindow window(&engine, nullptr);
        QVERIFY(mervin::testing::openAndWait(window, QStringLiteral(MERVIN_FIXTURE_PDF)));
        QVERIFY(window.openFile(gone, false, -1, /*makeCurrent=*/false, {}, /*lazy=*/true));
        QVERIFY(QFile::remove(gone));

        QStringList seen;
        window.whenTabsCanChange([&] { seen = tabFiles(window); });
        QVERIFY(window.canChangeTabs());
        QVERIFY(!window.closeRemovedTabs(gone));
        QCoreApplication::processEvents();
        QCOMPARE(seen, (QStringList{QStringLiteral("properties.pdf"), QStringLiteral("attachment.pdf")}));
        QVERIFY(window.closeRemovedTabs(gone));
        QCOMPARE(tabFiles(window), QStringList{QStringLiteral("properties.pdf")});
    }

    // A failed Duplicate closes its empty window without a modal warning.
    void failedDuplicateLeavesNoEmptyWindow()
    {
        QTemporaryDir files;
        const QString path = files.filePath(QStringLiteral("attachment.pdf"));
        QVERIFY(QFile::copy(QStringLiteral(MERVIN_FIXTURE_PDF), path));
        const auto windows = [] {
            int count = 0;
            for (QWidget *w : QApplication::topLevelWidgets())
                count += qobject_cast<MainWindow *>(w) != nullptr;
            return count;
        };
        WindowManager manager;
        MainWindow *window = manager.createWindow();
        QVERIFY(mervin::testing::openAndWait(*window, path));
        manager.duplicateToNewWindow(path, {}, {}, {});
        QTRY_COMPARE(windows(), 2);
        QPointer<TabPage> duplicate;
        for (QWidget *widget : QApplication::topLevelWidgets())
            if (auto *other = qobject_cast<MainWindow *>(widget); other && other != window)
                duplicate = other->findChild<TabPage *>();
        QVERIFY(duplicate);
        QTRY_VERIFY_WITH_TIMEOUT(duplicate && duplicate->isLoaded(), 10000);

        // A path with no file, as for an unloaded tab whose file was deleted. The
        // open tabs keep their file open, so Windows would not let the test delete it.
        manager.duplicateToNewWindow(files.filePath(QStringLiteral("gone.pdf")), {}, {}, {});
        QTRY_COMPARE(windows(), 2);
        QVERIFY(duplicate && duplicate->isLoaded());
        QVERIFY(clicked_.isEmpty());
        QVERIFY(unexpectedDialogs_.isEmpty());
    }

    void savingSuspendedEditsRequiresConfirmingExternalChanges()
    {
        QTemporaryDir files;
        const QString path = files.filePath(QStringLiteral("changed.pdf"));
        QVERIFY(QFile::copy(QStringLiteral(MERVIN_FIXTURE_PDF), path));
        RenderEngine engine;
        MainWindow window(&engine, nullptr);
        QVERIFY(mervin::testing::openAndWait(window, path));
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
        QVERIFY(mervin::testing::openAndWait(window, path));
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
        QVERIFY(mervin::testing::openAndWait(window, path));
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
    QStringList clicked_; // the text of each button the dialog handler clicked
    QStringList unexpectedDialogs_;
};

QTEST_MAIN(TstDocumentWorkflow)
#include "tst_document_workflow.moc"
