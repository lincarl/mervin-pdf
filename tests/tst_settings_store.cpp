#include "config/ConfigPaths.h"
#include "config/Settings.h"
#include "io/AppFiles.h"

#include <QFile>
#include <QTemporaryDir>
#include <QtTest>

#include <memory>

class TstSettingsStore : public QObject
{
    Q_OBJECT
private slots:
    // Each test gets a profile of its own, which outlives the saves it queued.
    void init()
    {
        profile_ = std::make_unique<QTemporaryDir>();
        QVERIFY(profile_->isValid());
        mervin::ConfigPaths::setOverrideDir(profile_->path());
    }
    void cleanup()
    {
        QVERIFY(mervin::appfiles::flush(10000)); // before the test's folder goes
        mervin::ConfigPaths::setOverrideDir({});
        profile_.reset();
    }

    void staleWindowPreservesNewPreferences()
    {
        auto first = mervin::Settings::load();
        auto second = mervin::Settings::load();
        first.annotationAuthor = "Updated";
        first.accentColor = "#112233";
        first.unloadInactiveMinutes = 0;
        first.closeToTray = false;
        QVERIFY(first.save());
        second.windowGeometry = "geometry";
        QVERIFY(second.save());
        auto loaded = mervin::Settings::load();
        QCOMPARE(loaded.annotationAuthor, first.annotationAuthor);
        QCOMPARE(loaded.accentColor, first.accentColor);
        QCOMPARE(loaded.windowGeometry, second.windowGeometry);
        QCOMPARE(loaded.unloadInactiveMinutes, 0);
        QVERIFY(!loaded.closeToTray);
        second.measurementSnap = false;
        QVERIFY(second.save());
        QCOMPARE(mervin::Settings::load().annotationAuthor, first.annotationAuthor);
        QVERIFY(!mervin::Settings::load().measurementSnap);
    }

    void memoryAndTrayDefaultsAndRoundTrip()
    {
        auto s = mervin::Settings::load();
        QCOMPARE(s.unloadInactiveMinutes, 30);
        QVERIFY(s.closeToTray);
        for (const int minutes : {0, 1, 17, mervin::Settings::kMaxUnloadInactiveMinutes}) {
            s.unloadInactiveMinutes = minutes;
            s.closeToTray = false;
            QVERIFY(s.save());
            const auto loaded = mervin::Settings::load();
            QCOMPARE(loaded.unloadInactiveMinutes, minutes);
            QVERIFY(!loaded.closeToTray);
        }
        QVERIFY(mervin::appfiles::flush(10000)); // saves are written in the background
        QFile file(mervin::ConfigPaths::configFile());
        QVERIFY(file.open(QIODevice::ReadOnly));
        const QByteArray saved = file.readAll();
        QVERIFY(saved.contains("unload_inactive_minutes = 10080"));
        QVERIFY(saved.contains("close_to_tray = false"));
    }

    void invalidMemorySettingsUseDefaults_data()
    {
        QTest::addColumn<QByteArray>("value");
        for (const char *value : {"-1", "10081", "4294967296", "\"Never\"", "1.5", "true"})
            QTest::newRow(value) << QByteArray(value);
    }

    void invalidMemorySettingsUseDefaults()
    {
        QFETCH(QByteArray, value);
        QFile file(mervin::ConfigPaths::configFile());
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("unload_inactive_minutes = " + value + "\nclose_to_tray = \"invalid\"\n");
        file.close();
        const auto loaded = mervin::Settings::load();
        QCOMPARE(loaded.unloadInactiveMinutes, 30);
        QVERIFY(loaded.closeToTray);
    }

    void invalidMemoryTimeoutCannotBeSaved()
    {
        auto s = mervin::Settings::load();
        s.unloadInactiveMinutes = 17;
        QVERIFY(s.save());
        for (const int invalid : {-1, mervin::Settings::kMaxUnloadInactiveMinutes + 1}) {
            s.unloadInactiveMinutes = invalid;
            QString error;
            QVERIFY(!s.save(&error));
            QVERIFY(!error.isEmpty());
            QCOMPARE(mervin::Settings::load().unloadInactiveMinutes, 17);
        }
    }

    // Fresh profiles follow the system's light or dark chrome, use the system accent,
    // and remove missing recent files. Saved values survive the new defaults.
    void appearanceAndRecentDefaultsRespectSavedValues()
    {
        auto s = mervin::Settings::load();
        QCOMPARE(s.colorScheme, QStringLiteral("system"));
        QCOMPARE(s.accentColor, QStringLiteral("system"));
        QVERIFY(!s.recentKeepMissing);

        s.colorScheme = QStringLiteral("dark"); // the old default, saved by earlier versions
        s.accentColor = QStringLiteral("#112233");
        for (const bool keepMissing : {true, false}) {
            s.recentKeepMissing = keepMissing;
            QVERIFY(s.save());
            const auto loaded = mervin::Settings::load();
            QCOMPARE(loaded.colorScheme, s.colorScheme);
            QCOMPARE(loaded.accentColor, s.accentColor);
            QCOMPARE(loaded.recentKeepMissing, keepMissing);
        }
        QVERIFY(mervin::appfiles::flush(10000)); // saves are written in the background
        QFile file(mervin::ConfigPaths::configFile());
        QVERIFY(file.open(QIODevice::ReadOnly));
        QVERIFY(file.readAll().contains("recent_keep_removed = false"));
    }

    // Versions up to 1.65.0 saved recent_keep_missing = true as their default in
    // every config. Mervin ignores that retired key, so those profiles get the
    // current default, and the next save drops it.
    void retiredKeepMissingKeyIsIgnored()
    {
        QFile file(mervin::ConfigPaths::configFile());
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("recent_keep_missing = true\nrecent_visible_count = 42\n");
        file.close();
        auto s = mervin::Settings::load();
        QVERIFY(!s.recentKeepMissing);
        QCOMPARE(s.recentVisibleCount, 42);

        s.annotationAuthor = QStringLiteral("Ann");
        QVERIFY(s.save());
        QVERIFY(!mervin::Settings::load().recentKeepMissing);
        // Read it last, because Windows won't replace a file that is open.
        QVERIFY(mervin::appfiles::flush(10000));
        QVERIFY(file.open(QIODevice::ReadOnly));
        const QByteArray saved = file.readAll();
        QVERIFY(saved.contains("recent_keep_removed = false"));
        QVERIFY(!saved.contains("recent_keep_missing"));
    }

    // The Recent search starts in Names unless Settings says otherwise; a value
    // the app does not know (a hand edit, a newer version) falls back to Names.
    void recentSearchScopeDefaultsToNamesAndRoundTrips()
    {
        QCOMPARE(mervin::Settings::load().recentSearchScope, QStringLiteral("names"));
        auto s = mervin::Settings::load();
        s.recentSearchScope = QStringLiteral("all");
        QVERIFY(s.save());
        QCOMPARE(mervin::Settings::load().recentSearchScope, QStringLiteral("all"));

        s.recentSearchScope = QStringLiteral("everything");
        QVERIFY(s.save());
        QCOMPARE(mervin::Settings::load().recentSearchScope, QStringLiteral("names"));
    }

    // The UI language stays empty until the first-run window stores a choice,
    // which is how startup tells a first run apart. A stored ID comes back as
    // written, including one this build doesn't ship (a newer version's).
    void uiLanguageStartsEmptyAndRoundTrips()
    {
        QVERIFY(mervin::Settings::load().uiLanguage.isEmpty());
        auto s = mervin::Settings::load();
        s.uiLanguage = QStringLiteral("zh_CN");
        QVERIFY(s.save());
        QCOMPARE(mervin::Settings::load().uiLanguage, QStringLiteral("zh_CN"));

        s.uiLanguage = QStringLiteral("pt_BR");
        QVERIFY(s.save());
        QCOMPARE(mervin::Settings::load().uiLanguage, QStringLiteral("pt_BR"));
        // Read it last, because Windows won't replace a file that is open.
        QVERIFY(mervin::appfiles::flush(10000)); // saves are written in the background
        QFile file(mervin::ConfigPaths::configFile());
        QVERIFY(file.open(QIODevice::ReadOnly));
        const QByteArray saved = file.readAll();
        QVERIFY(saved.contains("ui_language = "));
        QVERIFY(saved.contains("pt_BR"));
    }

    // Equality compares the saved values only, not what each copy was last
    // loaded or saved against.
    void equalityComparesValuesOnly()
    {
        const auto loaded = mervin::Settings::load();
        QVERIFY(loaded == mervin::Settings{});
        auto changed = loaded;
        changed.colorScheme = QStringLiteral("light");
        QVERIFY(changed != loaded);
        changed = loaded;
        changed.windowGeometry = "geometry";
        QVERIFY(changed != loaded);
    }

    // Settings' Apply and OK both save the window's copy. Taking new values must
    // keep that copy's baseline, or a value set back to what it was when Settings
    // opened would not be written after an earlier Apply.
    void assignedValuesSaveAgainstTheLastSave()
    {
        auto window = mervin::Settings::load();
        const auto opened = window;
        auto edited = opened;
        edited.defaultZoom = QStringLiteral("fit-page");
        window.assignValues(edited);
        QVERIFY(window.save());
        QCOMPARE(mervin::Settings::load().defaultZoom, QStringLiteral("fit-page"));
        window.assignValues(opened);
        QVERIFY(window.save());
        QCOMPARE(mervin::Settings::load().defaultZoom, QStringLiteral("fit-width"));
    }

    void writeFailureIsReported()
    {
        QVERIFY(QDir().mkdir(mervin::ConfigPaths::configFile()));
        QString error;
        mervin::Settings changed = mervin::Settings::load();
        const QString before = changed.annotationAuthor;
        changed.annotationAuthor = QStringLiteral("Not saved");
        QVERIFY(!changed.save(&error));
        QVERIFY(!error.isEmpty());
        // The settings in memory keep matching the file.
        QCOMPARE(mervin::Settings::load().annotationAuthor, before);
    }

private:
    std::unique_ptr<QTemporaryDir> profile_;
};
QTEST_GUILESS_MAIN(TstSettingsStore)
#include "tst_settings_store.moc"
