#pragma once

#include <QList>
#include <QObject>
#include <QPointer>

#include <optional>

class MainWindow;

namespace mervin {

class TabPage;
class WindowManager;

// Resolves a close request through the event loop. It never waits for a document
// worker and changes tabs only through their current window's tab-change queue.
class CloseFlow : public QObject
{
public:
    enum class Intent { Tab, AllTabs, Window, Quit, Restart, SaveOnly };

    static void request(MainWindow *window, Intent intent, TabPage *tab = nullptr,
                        bool remember = true);
    static void requestQuit(WindowManager *manager, bool restart = false);
    // The updater needs a synchronous answer. Chosen saves run later without
    // closing windows. A false answer never schedules a later shutdown.
    static bool tryCloseForUpdate(WindowManager *manager);

    ~CloseFlow() override;

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    struct Entry
    {
        QPointer<TabPage> tab;
        std::optional<quint64> accepted;
        bool save = false;
    };
    enum class WaitReason { None, Operation, ResumeForSave, DocumentReleaseForSave };

    CloseFlow(WindowManager *manager, MainWindow *window, Intent intent,
              TabPage *tab, bool remember);
    static CloseFlow *start(WindowManager *manager, MainWindow *window, Intent intent,
                            TabPage *tab = nullptr, bool remember = true);
    bool covers(const CloseFlow &other) const;
    bool includes(TabPage *tab) const;
    QList<QPointer<MainWindow>> windows() const;
    void collect();
    void add(TabPage *tab);
    Entry *entry(TabPage *tab);
    void post(TabPage *tab = nullptr);
    void advance(MainWindow *readyWindow);
    void wait(Entry &entry, WaitReason reason);
    void clearWait();
    void attention(TabPage *tab);
    void finish();
    void cancel();
    void releaseClaims();

    QPointer<WindowManager> manager_;
    QPointer<MainWindow> origin_;
    Intent intent_;
    QList<Entry> entries_;
    QPointer<TabPage> waitingTab_;
    QPointer<MainWindow> waitingWindow_;
    WaitReason waiting_ = WaitReason::None;
    QList<QMetaObject::Connection> waitConnections_;
    bool remember_ = true;
    bool posted_ = false;
    bool ended_ = false;
    bool advancing_ = false;
    bool advanceAgain_ = false;
};

} // namespace mervin
