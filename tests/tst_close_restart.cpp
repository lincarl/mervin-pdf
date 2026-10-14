#include "app/Relaunch.h"
#include "app/WindowManager.h"
#include "config/ConfigPaths.h"
#include "config/Settings.h"
#include "io/AppFiles.h"
#include "io/Background.h"
#include "io/JobContext.h"
#include "render/AnnotModel.h"
#include "render/Document.h"
#include "ui/MainWindow.h"
#include "ui/TabPage.h"
#include "ui/ViewerWidget.h"
#include "TestWaits.h"

#include <QApplication>
#include <QFile>
#include <QMessageBox>
#include <QPushButton>
#include <QScopeGuard>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QTimer>
#include <QtTest>

using namespace mervin;

// Relaunch's process-wide request has no reset. Keep this in its own executable.
class TstCloseRestart : public QObject
{
    Q_OBJECT

private slots:
    void restartRequestsRelaunchOnlyAfterApproval()
    {
        QTemporaryDir profile;
        QTemporaryDir files;
        const auto cleanup = qScopeGuard([] {
            job::testing::release();
            background::settle(10000);
            Document::waitForCloses(10000);
            appfiles::flush(10000);
            ConfigPaths::setOverrideDir({});
        });
        ConfigPaths::setOverrideDir(profile.path());
        Settings settings;
        settings.autoUpdate = false;
        settings.closeToTray = false;
        settings.unloadInactiveMinutes = 0;
        QVERIFY(settings.save());
        const QString path = files.filePath(QStringLiteral("edited.pdf"));
        QVERIFY(QFile::copy(QStringLiteral(MERVIN_FIXTURE_PDF), path));
        {
            WindowManager manager;
            const QPointer<MainWindow> window = manager.createWindow();
            QVERIFY(testing::openAndWait(*window, path));
            auto *tabs = window->findChild<QTabWidget *>();
            QTRY_VERIFY(qobject_cast<TabPage *>(tabs->currentWidget())->isLoaded());
            const QPointer<TabPage> tab = qobject_cast<TabPage *>(tabs->currentWidget());
            QVERIFY(tab->viewer()->annotModel()->addTextNote(
                0, {40, 40}, Qt::yellow, {}, QStringLiteral("Pending")) >= 0);
            QList<QMessageBox::StandardButton> answers{QMessageBox::Cancel, QMessageBox::Discard};
            QTimer dialogs;
            connect(&dialogs, &QTimer::timeout, this, [&] {
                auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
                if (box && !answers.isEmpty())
                    box->button(answers.takeFirst())->click();
            });
            dialogs.start(5);
            auto hold = [&] {
                job::testing::holdWhen([path](job::Step step, const QString &where) {
                    return step == job::Step::Reading && where == path;
                });
                tab->startOperation({files.path(), [path] { job::reading(path); }, {}});
            };
            hold();
            QTRY_COMPARE(job::testing::held(), 1);
            manager.restart();
            QTRY_VERIFY(!tab->findChild<QWidget *>(QStringLiteral("waitBar"))->isHidden());
            QVERIFY(!relaunch::requested());
            job::testing::release();
            QTRY_COMPARE(answers.size(), 1);
            QTRY_VERIFY(!tab->isClosing());
            QVERIFY(window && !relaunch::requested());

            bool requestedAtDestruction = false;
            connect(window, &QObject::destroyed, this, [&] {
                requestedAtDestruction = relaunch::requested();
            });
            hold();
            QTRY_COMPARE(job::testing::held(), 1);
            manager.restart();
            QTRY_VERIFY(tab->isClosing());
            QVERIFY(!relaunch::requested());
            job::testing::release();
            QTRY_VERIFY(!window);
            QVERIFY(answers.isEmpty());
            QVERIFY(requestedAtDestruction);
            QVERIFY(relaunch::requested());
        }
        job::testing::release();
        QVERIFY(background::settle(10000));
        QVERIFY(Document::waitForCloses(10000));
        QVERIFY(appfiles::flush(10000));
        ConfigPaths::setOverrideDir({});
    }
};

QTEST_MAIN(TstCloseRestart)
#include "tst_close_restart.moc"
