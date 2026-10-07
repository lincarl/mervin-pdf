// The UI language module: which catalogs ship, matching the OS language list to
// one of them, and installing a catalog (with Qt's own strings merged in).
#include "i18n/UiLanguage.h"

#include <QCoreApplication>
#include <QGuiApplication>
#include <QLocale>
#include <QTest>

namespace i18n = mervin::i18n;

class TstUiLanguage : public QObject
{
    Q_OBJECT

private slots:
    void cleanup() { i18n::apply(QStringLiteral("en")); }

    // English first, then every catalog the build compiled in.
    void availableLanguagesListTheCatalogs()
    {
        QCOMPARE(i18n::availableLanguages(),
                 (QStringList{QStringLiteral("en"), QStringLiteral("sv"), QStringLiteral("zh_CN")}));
    }

    void normalizedAcceptsCaseAndSeparatorVariants_data()
    {
        QTest::addColumn<QString>("code");
        QTest::addColumn<QString>("expected");
        QTest::newRow("exact") << "sv" << "sv";
        QTest::newRow("upper case") << "SV" << "sv";
        QTest::newRow("hyphen") << "zh-cn" << "zh_CN";
        QTest::newRow("spaces") << " zh_CN " << "zh_CN";
        QTest::newRow("not shipped") << "de" << "";
        QTest::newRow("base of a regional catalog") << "zh" << "";
        QTest::newRow("other script") << "zh_TW" << "";
        QTest::newRow("empty") << "" << "";
    }

    void normalizedAcceptsCaseAndSeparatorVariants()
    {
        QFETCH(QString, code);
        QFETCH(QString, expected);
        QCOMPARE(i18n::normalized(code, i18n::availableLanguages()), expected);
    }

    void suggestionFollowsTheOsLanguageOrder_data()
    {
        QTest::addColumn<QStringList>("os");
        QTest::addColumn<QStringList>("available");
        QTest::addColumn<QString>("expected");
        const QStringList shipped{"en", "sv", "zh_CN"};
        QTest::newRow("Swedish") << QStringList{"sv-SE"} << shipped << "sv";
        QTest::newRow("Swedish in Finland") << QStringList{"sv-FI"} << shipped << "sv";
        // An English UI with Swedish regional formats lists English first.
        QTest::newRow("English before Swedish") << QStringList{"en-US", "sv-SE"} << shipped << "en";
        QTest::newRow("first shipped wins") << QStringList{"de-DE", "sv-SE"} << shipped << "sv";
        QTest::newRow("Chinese, China") << QStringList{"zh-CN"} << shipped << "zh_CN";
        QTest::newRow("Simplified in Singapore") << QStringList{"zh-Hans-SG"} << shipped << "zh_CN";
        QTest::newRow("bare zh is Simplified") << QStringList{"zh"} << shipped << "zh_CN";
        // Traditional Chinese readers don't get the Simplified catalog.
        QTest::newRow("Hong Kong") << QStringList{"zh-HK"} << shipped << "en";
        QTest::newRow("Taiwan, then Swedish") << QStringList{"zh-TW", "sv"} << shipped << "sv";
        // The lists as Qt makes them end with the bare "zh", which alone means
        // Simplified. After a Traditional tag it must not pick Simplified.
        const auto qtList = [](QLocale::Script script, QLocale::Territory territory) {
            return QLocale(QLocale::Chinese, script, territory).uiLanguages();
        };
        QTest::newRow("Qt's Taiwan list")
            << qtList(QLocale::TraditionalHanScript, QLocale::Taiwan) << shipped << "en";
        QTest::newRow("Qt's Hong Kong list")
            << qtList(QLocale::TraditionalHanScript, QLocale::HongKong) << shipped << "en";
        QTest::newRow("Qt's Singapore list")
            << qtList(QLocale::SimplifiedHanScript, QLocale::Singapore) << shipped << "zh_CN";
        // Debian's default LANGUAGE=zh_TW:zh, where Qt expands "zh" to zh-Hans-CN.
        QTest::newRow("Debian's Taiwan list")
            << QStringList{"zh-Hant-TW", "zh-TW", "zh-Hant", "zh-Hans-CN", "zh-CN", "zh-Hans", "zh"}
            << shipped << "en";
        // The first Chinese tag decides the script, whichever comes first.
        QTest::newRow("Traditional, then Simplified")
            << QStringList{"zh-Hant-TW", "zh-Hans-CN"} << shipped << "en";
        QTest::newRow("Simplified, then Traditional")
            << QStringList{"zh-Hans-CN", "zh-Hant-TW"} << shipped << "zh_CN";
        QTest::newRow("not shipped") << QStringList{"nb-NO", "fr-FR"} << shipped << "en";
        QTest::newRow("C locale") << QStringList{"C"} << shipped << "en";
        QTest::newRow("empty list") << QStringList{} << shipped << "en";
        // Regional variants reach a base catalog, and the territory breaks ties.
        const QStringList more{"en", "de", "pt_BR", "pt_PT"};
        QTest::newRow("Austrian German") << QStringList{"de-AT"} << more << "de";
        QTest::newRow("Portugal") << QStringList{"pt-PT"} << more << "pt_PT";
        QTest::newRow("Brazil") << QStringList{"pt-BR"} << more << "pt_BR";
        QTest::newRow("Portugal, only Brazil shipped")
            << QStringList{"pt-PT"} << QStringList{"en", "pt_BR"} << "pt_BR";
    }

    void suggestionFollowsTheOsLanguageOrder()
    {
        QFETCH(QStringList, os);
        QFETCH(QStringList, available);
        QFETCH(QString, expected);
        QCOMPARE(i18n::suggestedLanguage(os, available), expected);
    }

    // The catalogs carry Qt's own strings, so standard buttons follow the UI
    // language; an unknown ID shows English.
    void applyInstallsTheCatalogWithQtStrings()
    {
        const auto cancel = [] { return QCoreApplication::translate("QPlatformTheme", "Cancel"); };
        QCOMPARE(i18n::current(), QStringLiteral("en"));
        QCOMPARE(cancel(), QStringLiteral("Cancel"));

        i18n::apply(QStringLiteral("sv"));
        QCOMPARE(i18n::current(), QStringLiteral("sv"));
        QCOMPARE(cancel(), QStringLiteral("Avbryt"));

        i18n::apply(QStringLiteral("zh-CN"));
        QCOMPARE(i18n::current(), QStringLiteral("zh_CN"));
        QCOMPARE(cancel(), QStringLiteral("取消"));
        QCOMPARE(QGuiApplication::layoutDirection(), Qt::LeftToRight);

        i18n::apply(QStringLiteral("xx"));
        QCOMPARE(i18n::current(), QStringLiteral("en"));
        QCOMPARE(cancel(), QStringLiteral("Cancel"));
    }

    void namesShowTheLanguageInItselfAndInEnglish()
    {
        QCOMPARE(i18n::displayName(QStringLiteral("en")), QStringLiteral("English"));
        QCOMPARE(i18n::displayName(QStringLiteral("sv")), QStringLiteral("Svenska (Swedish)"));
        QCOMPARE(i18n::displayName(QStringLiteral("zh_CN")),
                 QStringLiteral("简体中文 (Chinese, Simplified)"));
        const QString search = i18n::searchText(QStringLiteral("zh_CN"));
        QVERIFY(search.contains(QStringLiteral("简体中文")));
        QVERIFY(search.contains(QStringLiteral("Chinese")));
        QVERIFY(search.contains(QStringLiteral("zh_CN")));
    }
};

QTEST_MAIN(TstUiLanguage)
#include "tst_ui_language.moc"
