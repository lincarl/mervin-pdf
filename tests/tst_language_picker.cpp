// The UI language picker (LanguageCombo), the first-run window that hosts it,
// and the program a restart launches.
#include "app/Relaunch.h"
#include "dialogs/FirstRunDialog.h"
#include "i18n/UiLanguage.h"
#include "ui/LanguageCombo.h"

#include <QApplication>
#include <QCheckBox>
#include <QLabel>
#include <QLineEdit>
#include <QListView>
#include <QPushButton>
#include <QSignalSpy>
#include <QTemporaryFile>
#include <QTest>

#include <algorithm>

using mervin::FirstRunDialog;
using mervin::LanguageCombo;
namespace i18n = mervin::i18n;

class TstLanguagePicker : public QObject
{
    Q_OBJECT

private slots:
    void cleanup() { i18n::apply(QStringLiteral("en")); }

    void listsTheShippedLanguages()
    {
        LanguageCombo combo;
        QCOMPARE(combo.count(), i18n::availableLanguages().size());
        QCOMPARE(combo.itemText(0), QStringLiteral("English"));
        for (const QString &code : i18n::availableLanguages()) {
            combo.setLanguage(code);
            QCOMPARE(combo.language(), code);
            QCOMPARE(combo.currentText(), i18n::displayName(code));
        }
        combo.setLanguage(QStringLiteral("zh-CN"));
        QCOMPARE(combo.language(), QStringLiteral("zh_CN"));
        combo.setLanguage(QStringLiteral("zh-TW"));
        QCOMPARE(combo.language(), QStringLiteral("zh_TW"));
        combo.setLanguage(QStringLiteral("xx"));
        QCOMPARE(combo.language(), QStringLiteral("en"));
    }

    void searchFiltersAndEnterPicks_data()
    {
        QTest::addColumn<QString>("typed");
        QTest::addColumn<QString>("expected"); // empty: nothing matches
        QTest::addColumn<int>("matches");
        QTest::newRow("English name") << "chinese" << "zh_CN" << 2;
        QTest::newRow("Simplified English name") << "simplified" << "zh_CN" << 1;
        QTest::newRow("Traditional English name") << "traditional" << "zh_TW" << 1;
        QTest::newRow("native name") << "svenska" << "sv" << 1;
        QTest::newRow("case") << "SVENSKA" << "sv" << 1;
        QTest::newRow("Simplified Chinese characters") << "简体" << "zh_CN" << 1;
        QTest::newRow("Traditional Chinese characters") << "繁體" << "zh_TW" << 1;
        QTest::newRow("Simplified code") << "zh_cn" << "zh_CN" << 1;
        QTest::newRow("Traditional code") << "zh_tw" << "zh_TW" << 1;
        QTest::newRow("Japanese native name") << "日本語" << "ja" << 1;
        QTest::newRow("accent folding") << "francais" << "fr" << 1;
        QTest::newRow("Portuguese region") << "portugal" << "pt_PT" << 1;
        QTest::newRow("Arabic native name") << "العربية" << "ar" << 1;
        QTest::newRow("no match") << "klingon" << "" << 0;
    }

    void searchFiltersAndEnterPicks()
    {
        QFETCH(QString, typed);
        QFETCH(QString, expected);
        QFETCH(int, matches);
        LanguageCombo combo;
        combo.show();
        QVERIFY(QTest::qWaitForWindowExposed(&combo));
        QSignalSpy picked(&combo, &LanguageCombo::languagePicked);

        combo.showPopup();
        QVERIFY(combo.isPopupVisible());
        // QTest types ASCII only; an input method delivers the rest as text.
        const bool ascii = std::all_of(typed.cbegin(), typed.cend(),
                                       [](QChar c) { return c.unicode() < 128; });
        if (ascii)
            QTest::keyClicks(combo.searchField(), typed);
        else
            combo.searchField()->insert(typed);
        QCOMPARE(combo.listView()->model()->rowCount(), matches);
        QTest::keyClick(combo.searchField(), Qt::Key_Return);
        if (expected.isEmpty()) {
            // Enter on an empty list picks nothing and leaves the list open.
            QVERIFY(picked.isEmpty());
            QVERIFY(combo.isPopupVisible());
            QCOMPARE(combo.language(), QStringLiteral("en"));
        } else {
            QVERIFY(!combo.isPopupVisible());
            QCOMPARE(combo.language(), expected);
            QCOMPARE(picked.size(), 1);
            QCOMPARE(picked.first().first().toString(), expected);
        }
    }

    // Arrow keys typed in the search field move the list; Esc closes only the list.
    void keyboardMovesAndEscKeepsTheChoice()
    {
        LanguageCombo combo;
        combo.show();
        QVERIFY(QTest::qWaitForWindowExposed(&combo));
        QSignalSpy picked(&combo, &LanguageCombo::languagePicked);

        combo.showPopup();
        QTest::keyClick(combo.searchField(), Qt::Key_Down);
        QTest::keyClick(combo.searchField(), Qt::Key_Escape);
        QVERIFY(!combo.isPopupVisible());
        QCOMPARE(combo.language(), QStringLiteral("en"));
        QVERIFY(picked.isEmpty());

        combo.showPopup();
        QVERIFY(combo.searchField()->text().isEmpty()); // a fresh search each time
        QTest::keyClick(combo.searchField(), Qt::Key_Down);
        QTest::keyClick(combo.searchField(), Qt::Key_Return);
        QCOMPARE(combo.language(), i18n::availableLanguages().at(1));
        QCOMPARE(picked.size(), 1);
    }

    // The search also matches the name in the UI language: in Swedish,
    // "kinesiska" finds both Chinese written standards.
    void searchMatchesTheNameInTheUiLanguage()
    {
        LanguageCombo combo;
        i18n::apply(QStringLiteral("sv"));
        const QString localName =
            QCoreApplication::translate("UiLanguage", "Chinese, Simplified");
        QVERIFY(localName.startsWith(QStringLiteral("kinesiska")));
        QCoreApplication::processEvents(); // LanguageChange is posted
        combo.show();
        QVERIFY(QTest::qWaitForWindowExposed(&combo));
        combo.showPopup();
        QTest::keyClicks(combo.searchField(), localName.left(4));
        QCOMPARE(combo.listView()->model()->rowCount(), 2);
        QCOMPARE(combo.listView()->model()->index(0, 0).data().toString(),
                 QStringLiteral("简体中文 (Chinese, Simplified)"));
        QCOMPARE(combo.listView()->model()->index(1, 0).data().toString(),
                 QStringLiteral("繁體中文 (Chinese, Traditional)"));
    }

    void downloadOcrDefaultsOnAndKeepsChoiceAcrossLanguages()
    {
        for (const bool offerDefault : {false, true}) {
            FirstRunDialog dialog(offerDefault);
            auto *check = dialog.findChild<QCheckBox *>(QStringLiteral("downloadOcr"));
            QVERIFY(check);
            QVERIFY(dialog.downloadOcr());
            check->setChecked(false);
            i18n::apply(QStringLiteral("sv"));
            QCoreApplication::processEvents();
            QCOMPARE(check->text(), QStringLiteral("Ladda ned OCR"));
            QVERIFY(!dialog.downloadOcr());
            i18n::apply(QStringLiteral("en"));
        }
    }

    void defaultAppCheckboxOnlyWhenOffered()
    {
        FirstRunDialog plain(false);
        QVERIFY(!plain.findChild<QCheckBox *>(QStringLiteral("makeDefaultPdfApp")));
        QVERIFY(!plain.makeDefaultApp());

        FirstRunDialog offered(true);
        auto *check = offered.findChild<QCheckBox *>(QStringLiteral("makeDefaultPdfApp"));
        QVERIFY(check);
        QVERIFY(check->isChecked());
        QVERIFY(offered.makeDefaultApp());
        check->setChecked(false);
        QVERIFY(!offered.makeDefaultApp());
    }

    // Picking a language applies it to the app at once, and the open window
    // follows; closing it keeps the language shown.
    void firstRunWindowSwitchesLanguageLive_data()
    {
        QTest::addColumn<QString>("search");
        QTest::addColumn<QString>("language");
        QTest::addColumn<QString>("welcome");
        QTest::addColumn<QString>("continueText");
        QTest::newRow("Swedish")
            << "svenska" << "sv" << "Välkommen till Mervin PDF" << "Fortsätt";
        QTest::newRow("Traditional Chinese")
            << "traditional" << "zh_TW" << "歡迎使用 Mervin PDF" << "繼續";
    }

    void firstRunWindowSwitchesLanguageLive()
    {
        QFETCH(QString, search);
        QFETCH(QString, language);
        QFETCH(QString, welcome);
        QFETCH(QString, continueText);
        FirstRunDialog dialog(true);
        dialog.show();
        QVERIFY(QTest::qWaitForWindowActive(&dialog));
        auto *heading = dialog.findChild<QLabel *>(QStringLiteral("firstRunHeading"));
        QVERIFY(heading);
        QCOMPARE(heading->text(), QStringLiteral("Welcome to Mervin PDF"));
        QCOMPARE(dialog.language(), QStringLiteral("en"));

        LanguageCombo *combo = dialog.languageCombo();
        combo->showPopup();
        QTest::keyClicks(combo->searchField(), search);
        QTest::keyClick(combo->searchField(), Qt::Key_Return);
        QCOMPARE(i18n::current(), language);
        QTRY_COMPARE(heading->text(), welcome);

        // Enter after a pick continues (on Linux, Enter on the combo would reopen it).
        auto *continueButton = dialog.findChild<QPushButton *>();
        QVERIFY(continueButton);
        QCOMPARE(continueButton->text(), continueText);
        QTRY_COMPARE(QApplication::focusWidget(), continueButton);

        dialog.reject();
        QCOMPARE(dialog.language(), language);
    }

    // Arrow keys on the closed combo apply each language and keep the focus, so
    // the next arrow press reaches the next language.
    void arrowKeysStepThroughLanguages()
    {
        FirstRunDialog dialog(false);
        dialog.show();
        QVERIFY(QTest::qWaitForWindowActive(&dialog));
        LanguageCombo *combo = dialog.languageCombo();
        combo->setFocus();
        QTest::keyClick(combo, Qt::Key_Down);
        QCOMPARE(i18n::current(), i18n::availableLanguages().at(1));
        QCOMPARE(QApplication::focusWidget(), combo);
        QTest::keyClick(combo, Qt::Key_Down);
        QCOMPARE(i18n::current(), i18n::availableLanguages().at(2));
        QCOMPARE(QApplication::focusWidget(), combo);
    }

    // Closing the window with Esc keeps the language it shows.
    void escapeKeepsTheShownLanguage()
    {
        i18n::apply(QStringLiteral("zh_CN"));
        FirstRunDialog dialog(false);
        dialog.show();
        QVERIFY(QTest::qWaitForWindowActive(&dialog));
        QTest::keyClick(&dialog, Qt::Key_Escape);
        QCOMPARE(dialog.result(), int(QDialog::Rejected));
        QCOMPARE(dialog.language(), QStringLiteral("zh_CN"));
    }

    // A restart launches the AppImage file when this copy runs from its mount
    // (the binary inside goes away with this process), and this executable
    // otherwise, even when another AppImage's variables were inherited.
    void restartLaunchesTheAppImageFile()
    {
        using mervin::relaunch::program;
        QTemporaryFile image;
        QVERIFY(image.open());
        const QString mount = QStringLiteral("/tmp/.mount_MervinXYZ");
        const QString inside = mount + QStringLiteral("/usr/bin/MervinPDF");
        QCOMPARE(program(image.fileName(), mount, inside), image.fileName());
        QCOMPARE(program(image.fileName(), mount + QLatin1Char('/'), inside), image.fileName());

        const QString deb = QStringLiteral("/usr/bin/MervinPDF");
        QCOMPARE(program(QString(), QString(), deb), deb);
        // Started from another AppImage's terminal: its variables don't apply.
        QCOMPARE(program(image.fileName(), QStringLiteral("/tmp/.mount_Cursor123"), deb), deb);
        QCOMPARE(program(image.fileName(), QString(), deb), deb);
        // A mount whose name only starts the same.
        QCOMPARE(program(image.fileName(), mount, mount + QStringLiteral("2/usr/bin/MervinPDF")),
                 mount + QStringLiteral("2/usr/bin/MervinPDF"));
        QCOMPARE(program(QStringLiteral("/no/such/Mervin.AppImage"), mount, inside), inside);
        QVERIFY(!mervin::relaunch::requested());
    }
};

QTEST_MAIN(TstLanguagePicker)
#include "tst_language_picker.moc"
