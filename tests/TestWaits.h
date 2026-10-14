#pragma once

#include "recent/PathKey.h"
#include "ui/MainWindow.h"
#include "ui/TabPage.h"

#include <QApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QDir>
#include <QFileInfo>
#include <QLabel>
#include <QPointer>
#include <QSet>
#include <QSignalSpy>
#include <QThread>

namespace mervin::testing {

// Opens or focuses one tab and waits for that tab's load, leaving other held loads
// alone. Lazy opens keep their existing scheduling and return without waiting.
inline bool openAndWait(MainWindow &window, const QString &path, bool allowDuplicate = false,
                        int atIndex = -1, bool makeCurrent = true,
                        const QString &password = {}, bool lazy = false)
{
    QSet<TabPage *> before;
    for (TabPage *tab : window.findChildren<TabPage *>())
        before.insert(tab);
    if (!window.openFile(path, allowDuplicate, atIndex, makeCurrent, password, lazy)) {
        qWarning("Open was not accepted for %s", qPrintable(path));
        return false;
    }
    if (lazy)
        return true;

    QPointer<TabPage> page;
    for (TabPage *tab : window.findChildren<TabPage *>()) {
        if (!before.contains(tab)) {
            page = tab;
            break;
        }
    }
    if (!page) {
        const QString key = normalizePathKey(QDir::cleanPath(QFileInfo(path).absoluteFilePath()));
        // A focus can select a tab in another window. Match its path rather than
        // waiting for every tab in either window.
        QList<MainWindow *> windows{&window};
        for (QWidget *widget : QApplication::topLevelWidgets())
            if (auto *other = qobject_cast<MainWindow *>(widget); other && other != &window)
                windows.append(other);
        for (MainWindow *owner : windows) {
            for (TabPage *tab : owner->findChildren<TabPage *>()) {
                if (normalizePathKey(tab->canonicalPath()) == key
                    || normalizePathKey(tab->path()) == key) {
                    page = tab;
                    break;
                }
            }
            if (page)
                break;
        }
    }
    if (!page) {
        qWarning("Open accepted %s without a matching tab", qPrintable(path));
        return false;
    }

    QSignalSpy finished(page, &TabPage::resumeFinished);
    bool loading = page->isLoading();
    const auto ready = [&] {
        if (!page || page->isLoaded())
            return true;
        loading = loading || page->isLoading();
        return !page->isLoading() && (loading || !finished.isEmpty());
    };
    QElapsedTimer elapsed;
    elapsed.start();
    while (!ready() && elapsed.elapsed() < 10000) {
        // Process the current event batch. Once this tab loads, leave later
        // completions, such as its FileProbe answer, for the test to observe.
        QCoreApplication::processEvents(QEventLoop::AllEvents);
        if (!ready())
            QThread::msleep(1);
    }
    const bool answered = ready();
    if (page && page->isLoaded())
        return true;
    const auto *label = page ? page->findChild<QLabel *>(QStringLiteral("documentLoadStatus"))
                             : nullptr;
    const QString reason = !page ? QStringLiteral("the accepted tab was removed")
                           : !answered ? QStringLiteral("the load did not finish within 10 seconds")
                           : label ? label->text() : QStringLiteral("the load failed");
    qWarning("Could not load %s. %s", qPrintable(path), qPrintable(reason));
    return false;
}

} // namespace mervin::testing
