#include "config/ConfigPaths.h"
#include "config/Settings.h"
#include "dialogs/ManageLanguagesDialog.h"
#include "dialogs/OcrPopup.h"

#include <QComboBox>
#include <QDir>
#include <QFile>
#include <QLabel>
#include <QLayout>
#include <QListWidget>
#include <QPushButton>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

using mervin::OcrPopup;
using mervin::ManageLanguagesDialog;

namespace {

class ConfigOverrideGuard
{
public:
    ConfigOverrideGuard()
        : original_(mervin::ConfigPaths::overrideDir())
    {
    }

    ~ConfigOverrideGuard()
    {
        mervin::ConfigPaths::setOverrideDir(original_);
    }

private:
    QString original_;
};

QComboBox *languageCombo(OcrPopup &popup)
{
    return popup.findChild<QComboBox *>(QStringLiteral("ocrLanguageCombo"));
}

} // namespace

class TstOcrPopup : public QObject
{
    Q_OBJECT

private slots:
    void startsWithConfiguredDefault();
    void changingLanguageRequestsRecognition();
    void refreshAddsLanguagesWithoutLosingSelection();
    void refreshUsesChangedDefaultAndRequestsRecognition();
    void recognizeButtonIsAbsent();
    void noInstalledLanguagesDisablesPicker();
    void defaultLanguagePersistsInSettings();
    void managerShowsDefaultAndEqualWidthLists();
};

void TstOcrPopup::startsWithConfiguredDefault()
{
    OcrPopup popup({QStringLiteral("deu"), QStringLiteral("eng"), QStringLiteral("swe")},
                   QStringLiteral("swe"));
    QCOMPARE(popup.selectedLanguages(), QStringList{QStringLiteral("swe")});
    QCOMPARE(languageCombo(popup)->currentData().toString(), QStringLiteral("swe"));
}

void TstOcrPopup::changingLanguageRequestsRecognition()
{
    OcrPopup popup({QStringLiteral("eng"), QStringLiteral("swe")}, QStringLiteral("eng"));
    QSignalSpy spy(&popup, &OcrPopup::recognizeRequested);

    languageCombo(popup)->setCurrentIndex(languageCombo(popup)->findData(QStringLiteral("swe")));

    QCOMPARE(spy.count(), 1);
    QCOMPARE(spy.takeFirst().at(0).toStringList(), QStringList{QStringLiteral("swe")});
}

void TstOcrPopup::refreshAddsLanguagesWithoutLosingSelection()
{
    OcrPopup popup({QStringLiteral("eng")}, QStringLiteral("eng"));
    QSignalSpy spy(&popup, &OcrPopup::recognizeRequested);

    popup.refreshLanguages({QStringLiteral("eng"), QStringLiteral("swe")},
                           QStringLiteral("eng"));

    QCOMPARE(languageCombo(popup)->count(), 2);
    QCOMPARE(popup.selectedLanguages(), QStringList{QStringLiteral("eng")});
    QCOMPARE(spy.count(), 0);
}

void TstOcrPopup::refreshUsesChangedDefaultAndRequestsRecognition()
{
    OcrPopup popup({QStringLiteral("eng")}, QStringLiteral("eng"));
    QSignalSpy spy(&popup, &OcrPopup::recognizeRequested);

    popup.refreshLanguages({QStringLiteral("eng"), QStringLiteral("swe")},
                           QStringLiteral("swe"));

    QCOMPARE(popup.selectedLanguages(), QStringList{QStringLiteral("swe")});
    QCOMPARE(spy.count(), 1);
    QCOMPARE(spy.takeFirst().at(0).toStringList(), QStringList{QStringLiteral("swe")});
}

void TstOcrPopup::recognizeButtonIsAbsent()
{
    OcrPopup popup({QStringLiteral("eng")}, QStringLiteral("eng"));

    QVERIFY(!popup.findChild<QPushButton *>(QStringLiteral("ocrRecognizeButton")));
    const QList<QPushButton *> buttons = popup.findChildren<QPushButton *>();
    for (const QPushButton *button : buttons)
        QVERIFY2(button->text() != QStringLiteral("Recognise"),
                 "OCR Selection must not show a Recognise button");
}

void TstOcrPopup::noInstalledLanguagesDisablesPicker()
{
    OcrPopup popup({}, QStringLiteral("eng"));

    QVERIFY(!languageCombo(popup)->isEnabled());
    QVERIFY(popup.selectedLanguages().isEmpty());
}

void TstOcrPopup::defaultLanguagePersistsInSettings()
{
    ConfigOverrideGuard guard;
    QTemporaryDir profile;
    QVERIFY(profile.isValid());
    mervin::ConfigPaths::setOverrideDir(profile.path());

    QCOMPARE(mervin::Settings::load().ocrDefaultLanguage, QStringLiteral("eng"));
    mervin::Settings settings;
    settings.ocrDefaultLanguage = QStringLiteral("swe");
    settings.save();
    QCOMPARE(mervin::Settings::load().ocrDefaultLanguage, QStringLiteral("swe"));
}

void TstOcrPopup::managerShowsDefaultAndEqualWidthLists()
{
    ConfigOverrideGuard guard;
    QTemporaryDir profile;
    QVERIFY(profile.isValid());
    mervin::ConfigPaths::setOverrideDir(profile.path());

    const QString tessdata = QDir(profile.path()).filePath(QStringLiteral("tessdata"));
    QVERIFY(QDir().mkpath(tessdata));
    for (const QString &code : {QStringLiteral("eng"), QStringLiteral("swe")}) {
        QFile model(QDir(tessdata).filePath(code + QStringLiteral(".traineddata")));
        QVERIFY(model.open(QIODevice::WriteOnly));
    }

    ManageLanguagesDialog manager(QStringLiteral("swe"));
    auto *defaultCombo =
        manager.findChild<QComboBox *>(QStringLiteral("ocrDefaultLanguage"));
    QVERIFY(defaultCombo);
    QCOMPARE(defaultCombo->count(), 2);
    QCOMPARE(manager.defaultLanguage(), QStringLiteral("swe"));

    const QList<QLabel *> headings =
        manager.findChildren<QLabel *>(QStringLiteral("ocrLanguageHeading"));
    QStringList headingTexts;
    for (const QLabel *heading : headings)
        headingTexts.append(heading->text());
    QVERIFY(headingTexts.contains(QStringLiteral("Installed (2)")));
    QVERIFY(headingTexts.contains(QStringLiteral("Available - best models")));

    manager.resize(720, 470);
    QVERIFY(manager.layout()->activate());
    auto *installed =
        manager.findChild<QListWidget *>(QStringLiteral("ocrInstalledLanguages"));
    auto *available =
        manager.findChild<QListWidget *>(QStringLiteral("ocrAvailableLanguages"));
    QVERIFY(installed);
    QVERIFY(available);
    QCOMPARE(installed->width(), available->width());
}

QTEST_MAIN(TstOcrPopup)
#include "tst_ocr_popup.moc"
