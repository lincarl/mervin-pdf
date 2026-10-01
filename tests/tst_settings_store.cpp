#include "config/ConfigPaths.h"
#include "config/Settings.h"

#include <QFile>
#include <QTemporaryDir>
#include <QtTest>

class TstSettingsStore : public QObject
{
    Q_OBJECT
private slots:
    void staleWindowPreservesNewPreferences()
    {
        QTemporaryDir dir;
        mervin::ConfigPaths::setOverrideDir(dir.path());
        auto first = mervin::Settings::load();
        auto second = mervin::Settings::load();
        first.annotationAuthor = "Updated";
        first.accentColor = "#112233";
        QVERIFY(first.save());
        second.windowGeometry = "geometry";
        QVERIFY(second.save());
        auto loaded = mervin::Settings::load();
        QCOMPARE(loaded.annotationAuthor, first.annotationAuthor);
        QCOMPARE(loaded.accentColor, first.accentColor);
        QCOMPARE(loaded.windowGeometry, second.windowGeometry);
        second.measurementSnap = false;
        QVERIFY(second.save());
        QCOMPARE(mervin::Settings::load().annotationAuthor, first.annotationAuthor);
        QVERIFY(!mervin::Settings::load().measurementSnap);
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
