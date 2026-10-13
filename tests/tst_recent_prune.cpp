#include "app/WindowManager.h"
#include "config/ConfigPaths.h"
#include "config/Settings.h"
#include "io/AppFiles.h"
#include "recent/RecentStore.h"
#include "recent/ViewStateStore.h"

#include <QDateTime>
#include <QDir>
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
    std::unique_ptr<QTemporaryDir> profile_;
    QString kept_, gone_, goneFolder_, away_;
};

QTEST_MAIN(TstRecentPrune)
#include "tst_recent_prune.moc"
