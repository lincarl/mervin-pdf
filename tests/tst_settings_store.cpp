#include "config/ConfigPaths.h"
#include "config/Settings.h"

#include <QFile>
#include <QTemporaryDir>
#include <QtTest>

class TstSettingsStore : public QObject
{
    Q_OBJECT
private slots:
    void cleanup() { mervin::ConfigPaths::setOverrideDir({}); }

    void staleWindowPreservesNewPreferences()
    {
        QTemporaryDir dir;
        mervin::ConfigPaths::setOverrideDir(dir.path());
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
        mervin::ConfigPaths::setOverrideDir({});
    }

    void memoryAndTrayDefaultsAndRoundTrip()
    {
        QTemporaryDir dir;
        mervin::ConfigPaths::setOverrideDir(dir.path());
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
        QTemporaryDir dir;
        mervin::ConfigPaths::setOverrideDir(dir.path());
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
        QTemporaryDir dir;
        mervin::ConfigPaths::setOverrideDir(dir.path());
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

    // Existing configs have no recent_keep_missing key: they keep today's
    // behaviour, and an explicit false survives a save and load.
    void keepMissingRecentDefaultsOnAndRoundTrips()
    {
        QTemporaryDir dir;
        mervin::ConfigPaths::setOverrideDir(dir.path());
        QVERIFY(mervin::Settings::load().recentKeepMissing);
        auto s = mervin::Settings::load();
        s.recentKeepMissing = false;
        QVERIFY(s.save());
        QVERIFY(!mervin::Settings::load().recentKeepMissing);
        QFile file(mervin::ConfigPaths::configFile());
        QVERIFY(file.open(QIODevice::ReadOnly));
        QVERIFY(file.readAll().contains("recent_keep_missing = false"));
        mervin::ConfigPaths::setOverrideDir({});
    }

    // The Recent search starts in Names unless Settings says otherwise; a value
    // the app does not know (a hand edit, a newer version) falls back to Names.
    void recentSearchScopeDefaultsToNamesAndRoundTrips()
    {
        QTemporaryDir dir;
        mervin::ConfigPaths::setOverrideDir(dir.path());
        QCOMPARE(mervin::Settings::load().recentSearchScope, QStringLiteral("names"));
        auto s = mervin::Settings::load();
        s.recentSearchScope = QStringLiteral("all");
        QVERIFY(s.save());
        QCOMPARE(mervin::Settings::load().recentSearchScope, QStringLiteral("all"));

        s.recentSearchScope = QStringLiteral("everything");
        QVERIFY(s.save());
        QCOMPARE(mervin::Settings::load().recentSearchScope, QStringLiteral("names"));
        mervin::ConfigPaths::setOverrideDir({});
    }

    // The UI language stays empty until the first-run window stores a choice,
    // which is how startup tells a first run apart. A stored ID comes back as
    // written, including one this build doesn't ship (a newer version's).
    void uiLanguageStartsEmptyAndRoundTrips()
    {
        QTemporaryDir dir;
        mervin::ConfigPaths::setOverrideDir(dir.path());
        QVERIFY(mervin::Settings::load().uiLanguage.isEmpty());
        auto s = mervin::Settings::load();
        s.uiLanguage = QStringLiteral("zh_CN");
        QVERIFY(s.save());
        QCOMPARE(mervin::Settings::load().uiLanguage, QStringLiteral("zh_CN"));

        s.uiLanguage = QStringLiteral("pt_BR");
        QVERIFY(s.save());
        QCOMPARE(mervin::Settings::load().uiLanguage, QStringLiteral("pt_BR"));
        // Read last: Windows won't replace a file that is open.
        QFile file(mervin::ConfigPaths::configFile());
        QVERIFY(file.open(QIODevice::ReadOnly));
        const QByteArray saved = file.readAll();
        QVERIFY(saved.contains("ui_language = "));
        QVERIFY(saved.contains("pt_BR"));
        mervin::ConfigPaths::setOverrideDir({});
    }

    // Equality compares the saved values only, not what each copy was last
    // loaded or saved against.
    void equalityComparesValuesOnly()
    {
        QTemporaryDir dir;
        mervin::ConfigPaths::setOverrideDir(dir.path());
        const auto loaded = mervin::Settings::load();
        QVERIFY(loaded == mervin::Settings{});
        auto changed = loaded;
        changed.colorScheme = QStringLiteral("light");
        QVERIFY(changed != loaded);
        changed = loaded;
        changed.windowGeometry = "geometry";
        QVERIFY(changed != loaded);
        mervin::ConfigPaths::setOverrideDir({});
    }

    // Settings' Apply and OK both save the window's copy. Taking new values must
    // keep that copy's baseline, or a value set back to what it was when Settings
    // opened would not be written after an earlier Apply.
    void assignedValuesSaveAgainstTheLastSave()
    {
        QTemporaryDir dir;
        mervin::ConfigPaths::setOverrideDir(dir.path());
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
        mervin::ConfigPaths::setOverrideDir({});
    }

    void writeFailureIsReported()
    {
        QTemporaryDir dir;
        mervin::ConfigPaths::setOverrideDir(dir.path());
        QVERIFY(QDir().mkdir(mervin::ConfigPaths::configFile()));
        QString error;
        QVERIFY(!mervin::Settings{}.save(&error));
        QVERIFY(!error.isEmpty());
        mervin::ConfigPaths::setOverrideDir({});
    }
};
QTEST_GUILESS_MAIN(TstSettingsStore)
#include "tst_settings_store.moc"
