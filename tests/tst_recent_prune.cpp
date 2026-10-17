#include "app/WindowManager.h"
#include "config/ConfigPaths.h"
#include "config/Settings.h"
#include "io/AppFiles.h"
#include "recent/RecentStore.h"
#include "recent/ViewStateStore.h"

#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

#include <memory>

using namespace mervin;

// With "Keep removed files in list" off, WindowManager forgets recent
// entries whose file is gone, on a worker thread, together with their view state.
// Entries on a detached volume stay.
class TstRecentPrune : public QObject
{
    Q_OBJECT
private slots:
    void init()
    {
        // A profile of its own, so neither a write still queued by the previous test
        // nor the settings it left in memory reach this one.
        QVERIFY(mervin::appfiles::flush(10000));
        profile_ = std::make_unique<QTemporaryDir>();
        QVERIFY(profile_->isValid());
        ConfigPaths::setOverrideDir(profile_->path());
        QDir root(profile_->path());
        QVERIFY(root.mkpath(QStringLiteral("docs")));
        kept_ = root.filePath(QStringLiteral("docs/kept.pdf"));
        gone_ = root.filePath(QStringLiteral("docs/gone.pdf"));                 // deleted
        goneFolder_ = root.filePath(QStringLiteral("deleted-folder/plan.pdf")); // folder deleted
#ifdef Q_OS_WIN
        away_ = QStringLiteral("//mervin-test-no-such-server/share/away.pdf");
#else
        away_ = QStringLiteral("/media/mervin-test-no-such-user/USB/away.pdf");
#endif
        QFile file(kept_);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.close();

        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        RecentStore recent;
        recent.add(away_, now - 4);
        recent.add(goneFolder_, now - 3);
        recent.add(gone_, now - 2);
        recent.add(kept_, now - 1);
        QVERIFY(recent.save(RecentStore::defaultFile()));
        ViewStateStore states;
        states.put(gone_, ViewState{}, now);
        states.put(kept_, ViewState{}, now);
        QVERIFY(states.save(ViewStateStore::defaultFile()));
    }
    void cleanup()
    {
        QVERIFY(mervin::appfiles::flush(10000));
        ConfigPaths::setOverrideDir({});
    }

    void dropsRemovedFilesByDefaultButNotDetachedVolumes()
    {
        QVERIFY(!Settings::load().recentKeepMissing);
        WindowManager wm;
        wm.refreshRecent();
        QTRY_COMPARE(wm.recentEntries().size(), 2);
        QCOMPARE(wm.recentEntries().at(0).path, kept_);
        QCOMPARE(wm.recentEntries().at(1).path, away_);

        RecentStore onDisk;
        QVERIFY(mervin::appfiles::flush(10000));
        QVERIFY(onDisk.load(RecentStore::defaultFile()));
        QCOMPARE(onDisk.count(), 2);
        ViewStateStore states;
        QVERIFY(mervin::appfiles::flush(10000));
        QVERIFY(states.load(ViewStateStore::defaultFile()));
        QVERIFY(!states.get(gone_));
        QVERIFY(states.get(kept_));
    }

    // A share mounted by hand on a folder, outside fstab, leaves the folder empty
    // once it is unmounted. Recent recorded the share's mount when the file was
    // opened, so the entry stays.
    void keepsFilesWhoseRecordedMountIsGone()
    {
#ifndef Q_OS_WIN
        const QDir root(profile_->path());
        QVERIFY(root.mkpath(QStringLiteral("share")));
        const QString onShare = root.filePath(QStringLiteral("share/plan.pdf"));
        RecentStore recent;
        QVERIFY(mervin::appfiles::flush(10000));
        QVERIFY(recent.load(RecentStore::defaultFile()));
        recent.add(onShare, QDateTime::currentMSecsSinceEpoch(), 0,
                   root.filePath(QStringLiteral("share")));
        QVERIFY(recent.save(RecentStore::defaultFile()));

        WindowManager wm;
        wm.refreshRecent();
        QTRY_COMPARE(wm.recentEntries().size(), 3);
        QCOMPARE(wm.recentEntries().at(0).path, onShare);
        QCOMPARE(wm.recentEntries().at(1).path, kept_);
        QCOMPARE(wm.recentEntries().at(2).path, away_);
#endif
    }

    // Entries an older version wrote have no recorded mount. A check that finds
    // the file there, here the prune pass of the Recent page, records the mount it
    // is on and saves it, so a share mounted outside fstab is recognised once it
    // goes away. A file that is not there gets none.
    void recordsTheMountOfAFileFoundThere()
    {
        WindowManager wm;
        QCOMPARE(entry(wm, kept_).mount, QString());
        wm.refreshRecent();
        QTRY_COMPARE(wm.recentEntries().size(), 2);
        const QString mount = RecentStore::mountOf(kept_);
#ifndef Q_OS_WIN
        QVERIFY(!mount.isEmpty());
#endif
        QTRY_COMPARE(entry(wm, kept_).mount, mount);
        QCOMPARE(entry(wm, away_).mount, QString());

        QVERIFY(mervin::appfiles::flush(10000));
        RecentStore onDisk;
        QVERIFY(onDisk.load(RecentStore::defaultFile()));
        QCOMPARE(onDisk.recordedMount(kept_), mount);
        QCOMPARE(onDisk.recordedMount(away_), QString());
    }

    // The Recent page checks its rows itself, also with Keep removed files in list
    // on, when nothing is pruned. Such a check records the mount too, and saves it
    // without a prune pass to end.
    void recordsTheMountFromAnyCheck()
    {
        Settings s = Settings::load();
        s.recentKeepMissing = true;
        QVERIFY(s.save());
        WindowManager wm;
        QCOMPARE(entry(wm, kept_).mount, QString());
        wm.fileProbe()->check({{kept_, QString()}}); // as RecentFilesPanel asks
        const QString mount = RecentStore::mountOf(kept_);
#ifndef Q_OS_WIN
        QVERIFY(!mount.isEmpty());
#endif
        QTRY_COMPARE(entry(wm, kept_).mount, mount);

        QVERIFY(mervin::appfiles::flush(10000));
        RecentStore onDisk;
        QVERIFY(onDisk.load(RecentStore::defaultFile()));
        QCOMPARE(onDisk.recordedMount(kept_), mount);
    }

    // A mount found just before quitting is saved on the way out, not lost with
    // the timer that would have saved it, or the next start could drop the entry
    // while its share is away.
    void savesFoundMountsOnQuit()
    {
        Settings s = Settings::load();
        s.recentKeepMissing = true; // no prune pass that would save at its end
        QVERIFY(s.save());
        {
            WindowManager wm;
            QSignalSpy checked(wm.fileProbe(), &FileProbe::checked);
            wm.fileProbe()->check({{kept_, QString()}});
            QVERIFY(checked.wait(10000));
            // Found, and waiting for the save that kMountSaveMs delays.
            QCOMPARE(entry(wm, kept_).mount, QString());
        }
        QVERIFY(mervin::appfiles::flush(10000));
        RecentStore onDisk;
        QVERIFY(onDisk.load(RecentStore::defaultFile()));
        QCOMPARE(onDisk.recordedMount(kept_), RecentStore::mountOf(kept_));
    }

    // A pass over many entries an older version wrote records every mount, but
    // saves recent.json and announces the list together, not once per answer,
    // since each announcement rebuilds every Recent page. The probe answers one
    // file at a time, so a pass that takes long saves at most once per
    // kMountSaveMs before it ends.
    void savesTheMountsOfAPassTogether()
    {
#ifdef Q_OS_WIN
        QSKIP("Windows records no mounts.");
#else
        constexpr int kEntries = 400;
        const QDir root(profile_->path());
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        RecentStore recent;
        for (int i = 0; i < kEntries; ++i) {
            const QString path = root.filePath(QStringLiteral("docs/old-%1.pdf").arg(i));
            QFile file(path);
            QVERIFY(file.open(QIODevice::WriteOnly));
            file.close();
            recent.add(path, now - 1000 - i);
        }
        QVERIFY(recent.save(RecentStore::defaultFile()));

        WindowManager wm;
        QSignalSpy announced(&wm, &WindowManager::recentListChanged);
        QElapsedTimer pass;
        pass.start();
        wm.refreshRecent(); // announces the list once before it prunes
        const auto recorded = [&wm] {
            int n = 0;
            for (const RecentEntry &e : wm.recentEntries())
                n += e.mount.isEmpty() ? 0 : 1;
            return n;
        };
        QTRY_COMPARE_WITH_TIMEOUT(recorded(), kEntries, 20000);
        const qint64 saves = announced.count() - 1;
        const qint64 allowed = 1 + pass.elapsed() / WindowManager::kMountSaveMs;
        QVERIFY2(saves >= 1 && saves <= allowed,
                 qPrintable(QStringLiteral("%1 saves in %2 ms").arg(saves).arg(pass.elapsed())));
#endif
    }

    // Favourites never age out of Recent, and their view state stays with them,
    // also when the history is read again with the lower retention.
    void lowerRetentionKeepsFavourites()
    {
        // Saved views, oldest first: away_, gone_, kept_.
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        ViewStateStore states;
        states.put(away_, ViewState{}, now - 30);
        states.put(gone_, ViewState{}, now - 20);
        states.put(kept_, ViewState{}, now - 10);
        QVERIFY(states.save(ViewStateStore::defaultFile()));

        {
            WindowManager wm;
            wm.setFavorite(away_, true); // the oldest entry
            wm.applyRecentSettings(7, 1, true);
            QCOMPARE(wm.recentEntries().size(), 2);
            QCOMPARE(wm.recentEntries().at(0).path, kept_);
            QCOMPARE(wm.recentEntries().at(1).path, away_);

            QVERIFY(mervin::appfiles::flush(10000));
            RecentStore onDisk;
            QVERIFY(onDisk.load(RecentStore::defaultFile()));
            QCOMPARE(onDisk.count(), 2);
            QVERIFY(onDisk.entries().at(1).favorite);
            ViewStateStore statesOnDisk;
            QVERIFY(statesOnDisk.load(ViewStateStore::defaultFile()));
            QCOMPARE(statesOnDisk.count(), 2);
            QVERIFY(statesOnDisk.get(kept_));
            QVERIFY(statesOnDisk.get(away_));
        }

        Settings s = Settings::load();
        s.recentRetention = 1;
        QVERIFY(s.save());
        WindowManager wm;
        QCOMPARE(wm.recentEntries().size(), 2);
        // A newer view state for another file takes the one place from kept_'s,
        // not from the favourite's.
        wm.saveViewState(gone_, ViewState{});
        QVERIFY(mervin::appfiles::flush(10000));
        ViewStateStore statesOnDisk;
        QVERIFY(statesOnDisk.load(ViewStateStore::defaultFile()));
        QCOMPARE(statesOnDisk.count(), 2);
        QVERIFY(statesOnDisk.get(gone_));
        QVERIFY(!statesOnDisk.get(kept_));
        QVERIFY(statesOnDisk.get(away_));
    }

    void keepsEverythingWhileTheSettingIsOn()
    {
        Settings s = Settings::load();
        s.recentKeepMissing = true;
        QVERIFY(s.save());
        WindowManager wm;
        wm.refreshRecent();
        QTest::qWait(200);
        QCOMPARE(wm.recentEntries().size(), 4);
    }

    // Settings OK: a lower retention is saved at once, the shown count goes to
    // every window, and switching the setting off prunes.
    void applyRecentSettingsTrimsAndPrunes()
    {
        WindowManager wm;
        QSignalSpy shown(&wm, &WindowManager::recentVisibleCountChanged);
        wm.applyRecentSettings(7, 3, true);
        QCOMPARE(shown.count(), 1);
        QCOMPARE(shown.at(0).at(0).toInt(), 7);
        RecentStore onDisk;
        QVERIFY(mervin::appfiles::flush(10000));
        QVERIFY(onDisk.load(RecentStore::defaultFile()));
        QCOMPARE(onDisk.count(), 3); // the oldest, away, trimmed

        wm.applyRecentSettings(7, 3, false);
        QTRY_COMPARE(wm.recentEntries().size(), 1);
        QCOMPARE(wm.recentEntries().first().path, kept_);
    }

private:
    // The entry for `path` in `wm`'s Recent list, or an empty one.
    static RecentEntry entry(const WindowManager &wm, const QString &path)
    {
        for (const RecentEntry &e : wm.recentEntries())
            if (e.path == path)
                return e;
        return {};
    }

    std::unique_ptr<QTemporaryDir> profile_;
    QString kept_, gone_, goneFolder_, away_;
};

QTEST_MAIN(TstRecentPrune)
#include "tst_recent_prune.moc"
