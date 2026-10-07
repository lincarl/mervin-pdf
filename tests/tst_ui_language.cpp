// The UI language module: which catalogs ship, matching the OS language list to
// one of them, and installing application and standard-widget catalogs.
#include "i18n/UiLanguage.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QFont>
#include <QFontDatabase>
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
                 QStringLiteral("en ar az be bg bs ca cnr cs da de el es et fi fr ga hi hr hu hy "
                                "id is it ja ka kk ko lb lt lv mk mt nb nl nn pl pt_BR pt_PT rm "
                                "ro ru sk sl sq sr sv th tr uk vi zh_CN").split(QLatin1Char(' ')));
    }

    void normalizedAcceptsCaseAndSeparatorVariants_data()
    {
        QTest::addColumn<QString>("code");
        QTest::addColumn<QString>("expected");
        QTest::newRow("exact") << "sv" << "sv";
        QTest::newRow("upper case") << "SV" << "sv";
        QTest::newRow("hyphen") << "zh-cn" << "zh_CN";
        QTest::newRow("spaces") << " zh_CN " << "zh_CN";
        QTest::newRow("not shipped") << "he" << "";
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

    void newLocalesMatchWithoutCrossingWrittenStandards()
    {
        const QStringList shipped = i18n::availableLanguages();
        QCOMPARE(i18n::suggestedLanguage({"nb-NO"}, shipped), QStringLiteral("nb"));
        QCOMPARE(i18n::suggestedLanguage({"nn-NO"}, shipped), QStringLiteral("nn"));
        QCOMPARE(i18n::suggestedLanguage({"pt-PT"}, shipped), QStringLiteral("pt_PT"));
        QCOMPARE(i18n::suggestedLanguage({"pt-BR"}, shipped), QStringLiteral("pt_BR"));
        QCOMPARE(i18n::suggestedLanguage({"ja-JP"}, shipped), QStringLiteral("ja"));
        QCOMPARE(i18n::suggestedLanguage({"ar-EG"}, shipped), QStringLiteral("ar"));
        QCOMPARE(i18n::suggestedLanguage({"cnr-Latn-ME"}, shipped), QStringLiteral("cnr"));
        QCOMPARE(i18n::suggestedLanguage({"cnr-ME"}, shipped), QStringLiteral("cnr"));
        QCOMPARE(i18n::suggestedLanguage({"cnr-Cyrl-ME", "en"}, shipped), QStringLiteral("en"));
        QCOMPARE(i18n::suggestedLanguage({"cnr-Cyrl-ME", "cnr", "ja"}, shipped), QStringLiteral("ja"));
        QCOMPARE(i18n::suggestedLanguage({"sr-Cyrl-ME"}, shipped), QStringLiteral("sr"));
        QCOMPARE(i18n::suggestedLanguage({"zh-Hant-TW", "ja-JP"}, shipped), QStringLiteral("ja"));
        QVERIFY(!shipped.contains(QStringLiteral("zh_TW")));
    }

    void everyLanguageLoadsApplicationAndWidgetText_data()
    {
        QTest::addColumn<QString>("language");
        for (const QString &language : i18n::availableLanguages()) {
            if (language != QLatin1String("en"))
                QTest::newRow(qPrintable(language)) << language;
        }
    }

    void everyLanguageLoadsApplicationAndWidgetText()
    {
        QFETCH(QString, language);
        i18n::apply(language);
        QCOMPARE(i18n::current(), language);
        const QString welcome = QCoreApplication::translate("mervin::FirstRunDialog",
                                                            "Welcome to Mervin PDF");
        QVERIFY2(!welcome.isEmpty() && welcome != QLatin1String("Welcome to Mervin PDF"),
                 qPrintable(language));
        const QString cancel = QCoreApplication::translate("QPlatformTheme", "Cancel");
        QVERIFY2(!cancel.isEmpty() && cancel != QLatin1String("Cancel"), qPrintable(language));
        if (language == QLatin1String("ar")) {
            const QString folder = QCoreApplication::translate("QAbstractFileIconProvider", "Folder");
            QVERIFY(!folder.isEmpty() && folder != QLatin1String("Folder"));
        }
        // Exercise every plural category used by the shipped Qt locales.
        for (int count : {0, 1, 2, 3, 5, 11, 21, 100}) {
            const QString pages = QCoreApplication::translate("mervin::RecentFilesPanel",
                                                               "%n page(s)", nullptr, count);
            QVERIFY2(!pages.isEmpty() && !pages.contains(QLatin1String("page(s)")),
                     qPrintable(language + QStringLiteral(" plural %1").arg(count)));
        }
        QCOMPARE(QGuiApplication::layoutDirection(),
                 language == QLatin1String("ar") ? Qt::RightToLeft : Qt::LeftToRight);
    }

    void cjkFontsSwitchWithTheLanguage()
    {
        const QString fontPath = qEnvironmentVariable("MERVIN_TEST_FONT");
        if (!fontPath.isEmpty()) {
            const QDir directory = QFileInfo(fontPath).absoluteDir();
            for (const QString &name : {QStringLiteral("NotoSansCJKsc-Regular.otf"),
                                        QStringLiteral("NotoSansCJKjp-Regular.otf"),
                                        QStringLiteral("NotoSansCJKkr-Regular.otf")})
                QVERIFY(QFontDatabase::addApplicationFont(directory.filePath(name)) >= 0);
        }
        const QStringList original = QFontDatabase::applicationFallbackFontFamilies(QChar::Script_Han);
        const QFont originalFont = QGuiApplication::font();
        i18n::apply(QStringLiteral("zh_CN"));
        const QStringList chinese = QFontDatabase::applicationFallbackFontFamilies(QChar::Script_Han);
        QVERIFY(!chinese.isEmpty());
        i18n::apply(QStringLiteral("ja"));
        const QStringList japanese = QFontDatabase::applicationFallbackFontFamilies(QChar::Script_Han);
        QVERIFY(!japanese.isEmpty());
        QVERIFY(japanese.first() != chinese.first());
        QCOMPARE(QGuiApplication::font().family(), japanese.first());
        QCOMPARE(QGuiApplication::font().pointSizeF(), originalFont.pointSizeF());
        QVERIFY(QFontDatabase::applicationFallbackFontFamilies(QChar::Script_Hiragana)
                    .contains(japanese.first()));
        i18n::apply(QStringLiteral("ko"));
        const QStringList korean = QFontDatabase::applicationFallbackFontFamilies(QChar::Script_Han);
        QVERIFY(!korean.isEmpty());
        QVERIFY(korean.first() != japanese.first());
        QVERIFY(QFontDatabase::applicationFallbackFontFamilies(QChar::Script_Hangul)
                    .contains(korean.first()));
        QVERIFY(!QFontDatabase::applicationFallbackFontFamilies(QChar::Script_Hiragana)
                     .contains(japanese.first()));
        i18n::apply(QStringLiteral("en"));
        QCOMPARE(QFontDatabase::applicationFallbackFontFamilies(QChar::Script_Han), original);
        QCOMPARE(QGuiApplication::font(), originalFont);
    }

    // Standard-widget catalogs make buttons follow the UI
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
