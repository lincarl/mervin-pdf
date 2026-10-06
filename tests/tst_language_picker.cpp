// The UI language picker (LanguageCombo), the first-run window that hosts it,
// and the program a restart launches.
#include "app/Relaunch.h"
#include "dialogs/FirstRunDialog.h"
#include "i18n/UiLanguage.h"
#include "ui/LanguageCombo.h"

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
        QCOMPARE(combo.count(), 3);
        QCOMPARE(combo.itemText(0), QStringLiteral("English"));
        QCOMPARE(combo.itemText(1), QStringLiteral("Svenska (Swedish)"));
        QCOMPARE(combo.itemText(2), QStringLiteral("简体中文 (Chinese, Simplified)"));
        combo.setLanguage(QStringLiteral("zh-CN"));
        QCOMPARE(combo.language(), QStringLiteral("zh_CN"));
        combo.setLanguage(QStringLiteral("xx"));
        QCOMPARE(combo.language(), QStringLiteral("en"));
    }

    void searchFiltersAndEnterPicks_data()
    {
        QTest::addColumn<QString>("typed");
        QTest::addColumn<QString>("expected"); // empty: nothing matches
        QTest::newRow("English name") << "chi" << "zh_CN";
        QTest::newRow("native name") << "svenska" << "sv";
        QTest::newRow("case") << "SVENSKA" << "sv";
        QTest::newRow("Chinese characters") << "简体" << "zh_CN";
        QTest::newRow("code") << "zh_cn" << "zh_CN";
        QTest::newRow("no match") << "klingon" << "";
    }

    void searchFiltersAndEnterPicks()
    {
        QFETCH(QString, typed);
        QFETCH(QString, expected);
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
        QCOMPARE(combo.listView()->model()->rowCount(), expected.isEmpty() ? 0 : 1);
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
        QCOMPARE(combo.language(), QStringLiteral("sv"));
        QCOMPARE(picked.size(), 1);
    }

    // The search also matches the name in the UI language: in Swedish,
    // "kinesiska" finds Chinese.
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
        QCOMPARE(combo.listView()->model()->rowCount(), 1);
        QCOMPARE(combo.listView()->model()->index(0, 0).data().toString(),
                 QStringLiteral("简体中文 (Chinese, Simplified)"));
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
    void firstRunWindowSwitchesLanguageLive()
    {
        FirstRunDialog dialog(true);
        dialog.show();
        QVERIFY(QTest::qWaitForWindowExposed(&dialog));
        auto *heading = dialog.findChild<QLabel *>(QStringLiteral("firstRunHeading"));
        QVERIFY(heading);
        QCOMPARE(heading->text(), QStringLiteral("Welcome to Mervin PDF"));
        QCOMPARE(dialog.language(), QStringLiteral("en"));

        LanguageCombo *combo = dialog.languageCombo();
        combo->showPopup();
        QTest::keyClicks(combo->searchField(), QStringLiteral("svenska"));
        QTest::keyClick(combo->searchField(), Qt::Key_Return);
        QCOMPARE(i18n::current(), QStringLiteral("sv"));
        QTRY_COMPARE(heading->text(), QStringLiteral("Välkommen till Mervin PDF"));

        dialog.reject();
        QCOMPARE(dialog.language(), QStringLiteral("sv"));
    }

    // A restart launches the AppImage file when there is one: the binary inside
    // its mount goes away with this process.
    void restartLaunchesTheAppImageFile()
    {
        const QString exe = QStringLiteral("/opt/mervin/MervinPDF");
        QCOMPARE(mervin::relaunch::program(QString(), exe), exe);
        QCOMPARE(mervin::relaunch::program(QStringLiteral("/no/such/Mervin.AppImage"), exe), exe);
        QTemporaryFile image;
        QVERIFY(image.open());
        QCOMPARE(mervin::relaunch::program(image.fileName(), exe), image.fileName());
        QVERIFY(!mervin::relaunch::requested());
    }
};

QTEST_MAIN(TstLanguagePicker)
#include "tst_language_picker.moc"
