#include "config/ConfigPaths.h"
#include "config/Settings.h"
#include "dialogs/SettingsDialog.h"
#include "i18n/UiLanguage.h"
#include "ui/LanguageCombo.h"
#include "render/AnnotTypes.h"
#include "ui/DocumentThemePicker.h"
#include "ui/Theme.h"
#include "ui/ThemeTokens.h"

#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFontDatabase>
#include <QHash>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QLocale>
#include <QMessageBox>
#include <QMetaEnum>
#include <QPushButton>
#include <QRadioButton>
#include <QScopeGuard>
#include <QScrollBar>
#include <QSignalSpy>
#include <QSpinBox>
#include <QStackedWidget>
#include <QTemporaryDir>
#include <QTest>
#include <QTimeZone>
#include <QTimer>
#include <QToolButton>
#include <QTranslator>

#include <functional>

using Page = SettingsDialog::Page;

class TstSettingsDialog : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void init();
    void cleanupTestCase();

    void defaultPdfControlIsWindowsOnly();
    void opensOnRequestedPage_data();
    void opensOnRequestedPage();
    void arrowKeysSkipTheDivider();
    void defaultsComeBackUnchanged();
    void everyFieldRoundTrips();
    void memoryAndTrayControls();
    void customUnloadDuration_data();
    void customUnloadDuration();
    void unloadDurationRejectsText();
    void unloadDurationUsesSpinBoxLocale();
    void invalidUnloadDurationCannotBeAccepted_data();
    void invalidUnloadDurationCannotBeAccepted();
    void documentThemesRespondToMouseAndKeyboard();
    void legacyDocumentThemeIsPreserved();
    void uiThemesRespondToMouseAndKeyboard();
    void applyHandsOverPendingChanges();
    void okAppliesPendingChangesAndCancelDropsThem();
    void everyKindOfControlEnablesApply();
    void handEditedMeasureValuesLandOnOfferedChoices();
    void autoUpdateFollowsAnOutsideChange();
    void updateControlsFollowTheUpdater();
    void enterOnTheMenuDoesNotPressOk();
    void ocrPageShowsAnInstalledDefaultButKeepsTheSavedOne();
    void removingAnOcrLanguageAsksFirst();
    void theLastOcrLanguageStays();
    void newUiLanguageRestartsOnApply();
    void untouchedUiLanguageKeepsTheStoredValue();
    void pendingUiLanguageShowsUntilChanged();
    void failedSaveKeepsTheDialogOpen();
    void menuFitsLongerPageTitles();
    void shortcutKeysFollowTheUiLanguage();

private:
    QString tessdataPath(const char *code) const;

    // A profile of our own: the OCR page reads the tessdata folder.
    QTemporaryDir profile_;
};

namespace {

QLabel *labelStartingWith(QWidget *root, const QString &prefix)
{
    for (QLabel *label : root->findChildren<QLabel *>())
        if (label->text().startsWith(prefix))
            return label;
    return nullptr;
}

QCheckBox *checkBox(QWidget *root, const QString &text)
{
    for (QCheckBox *box : root->findChildren<QCheckBox *>())
        if (box->text() == text)
            return box;
    return nullptr;
}

QPushButton *dialogButton(QWidget *dialog, QDialogButtonBox::StandardButton which)
{
    auto *buttons = dialog->findChild<QDialogButtonBox *>();
    return buttons ? buttons->button(which) : nullptr;
}

// A catalog holding only the given texts, standing in for a language whose
// text differs from English. Installed for the lifetime of the object.
class StubCatalog : public QTranslator
{
public:
    explicit StubCatalog(QHash<QByteArray, QString> texts) : texts_(std::move(texts))
    {
        QCoreApplication::installTranslator(this);
    }
    ~StubCatalog() override { QCoreApplication::removeTranslator(this); }

    QString translate(const char *context, const char *source, const char *, int) const override
    {
        return texts_.value(QByteArray(context) + '|' + source);
    }
    bool isEmpty() const override { return false; }

private:
    QHash<QByteArray, QString> texts_; // "context|source" -> text
};

} // namespace

void TstSettingsDialog::initTestCase()
{
    // The assertions below read the English UI text.
    mervin::i18n::apply(QStringLiteral("en"));
    QVERIFY(profile_.isValid());
    mervin::ConfigPaths::setOverrideDir(profile_.path());
    QVERIFY(QDir().mkpath(QDir(profile_.path()).filePath(QStringLiteral("tessdata"))));
}

// Every case starts with English and Swedish installed; some remove one.
void TstSettingsDialog::init()
{
    for (const char *code : {"eng", "swe"}) {
        QFile model(tessdataPath(code));
        QVERIFY(model.open(QIODevice::WriteOnly));
        model.write("x");
    }
}

QString TstSettingsDialog::tessdataPath(const char *code) const
{
    return QDir(profile_.path())
        .filePath(QStringLiteral("tessdata/%1.traineddata").arg(QLatin1String(code)));
}

void TstSettingsDialog::cleanupTestCase()
{
    mervin::ConfigPaths::setOverrideDir({});
}

void TstSettingsDialog::defaultPdfControlIsWindowsOnly()
{
    SettingsDialog dialog(mervin::Settings{});
    auto *button =
        dialog.findChild<QPushButton *>(QStringLiteral("setDefaultPdfAppButton"));

#ifdef Q_OS_WIN
    QVERIFY(button);
    QCOMPARE(button->text(), QStringLiteral("Set as Default PDF App"));
#else
    QVERIFY(!button);
#endif
}

void TstSettingsDialog::opensOnRequestedPage_data()
{
    QTest::addColumn<Page>("page");
    for (Page page : {Page::General, Page::Appearance, Page::Viewing, Page::Annotations,
                      Page::Ocr, Page::Measuring, Page::Forms, Page::Shortcuts, Page::About})
        QTest::newRow(QMetaEnum::fromType<Page>().valueToKey(int(page))) << page;
}

// The requested page must agree with the selected navigation row.
void TstSettingsDialog::opensOnRequestedPage()
{
    QFETCH(Page, page);
    SettingsDialog dialog(mervin::Settings{}, {}, page);
    QCOMPARE(dialog.currentPage(), page);

    auto *nav = dialog.findChild<QListWidget *>(QStringLiteral("settingsNav"));
    auto *stack = dialog.findChild<QStackedWidget *>();
    QVERIFY(nav && stack && nav->currentItem());
    QCOMPARE(nav->currentItem()->text().isEmpty(), false);
    QCOMPARE(stack->currentIndex(), nav->currentItem()->data(Qt::UserRole + 1).toInt());

    // Switching away moves both, so a stale page can't stay on screen.
    const Page other = page == Page::General ? Page::About : Page::General;
    dialog.showPage(other);
    QCOMPARE(dialog.currentPage(), other);
    QCOMPARE(stack->currentIndex(), nav->currentItem()->data(Qt::UserRole + 1).toInt());
}

// The divider between Forms and Keyboard shortcuts is a row in the list; it must
// never become the current row.
void TstSettingsDialog::arrowKeysSkipTheDivider()
{
    SettingsDialog dialog(mervin::Settings{}, {}, Page::Forms);
    auto *nav = dialog.findChild<QListWidget *>(QStringLiteral("settingsNav"));
    QVERIFY(nav);
    QTest::keyClick(nav, Qt::Key_Down);
    QCOMPARE(dialog.currentPage(), Page::Shortcuts);
    QTest::keyClick(nav, Qt::Key_Up);
    QCOMPARE(dialog.currentPage(), Page::Forms);
}

// Opening Settings and pressing OK must not rewrite anything.
void TstSettingsDialog::defaultsComeBackUnchanged()
{
    mervin::Settings in;
    in.ocrDefaultLanguage = QStringLiteral("eng");
    const mervin::Settings out = SettingsDialog(in).settings();
    QCOMPARE(out.defaultZoom, in.defaultZoom);
    QCOMPARE(out.pageMode, in.pageMode);
    QCOMPARE(out.twoPageSpread, in.twoPageSpread);
    QCOMPARE(out.colorScheme, in.colorScheme);
    QCOMPARE(out.documentTheme, in.documentTheme);
    QCOMPARE(out.accentColor, in.accentColor);
    QCOMPARE(out.openBehavior, in.openBehavior);
    QCOMPARE(out.restoreSession, in.restoreSession);
    QCOMPARE(out.unloadInactiveMinutes, in.unloadInactiveMinutes);
    QCOMPARE(out.closeToTray, in.closeToTray);
    QCOMPARE(out.recentVisibleCount, in.recentVisibleCount);
    QCOMPARE(out.recentRetention, in.recentRetention);
    QCOMPARE(out.recentKeepMissing, in.recentKeepMissing);
    QCOMPARE(out.recentSearchScope, in.recentSearchScope);
    QCOMPARE(out.autoUpdate, in.autoUpdate);
    QCOMPARE(out.measurementSnap, in.measurementSnap);
    QCOMPARE(out.measurementType, in.measurementType);
    QCOMPARE(out.measurementUnit, in.measurementUnit);
    QCOMPARE(out.measurementPrecision, in.measurementPrecision);
    QCOMPARE(out.measurementLineWidth, in.measurementLineWidth);
    QCOMPARE(out.highlightFormFields, in.highlightFormFields);
    QCOMPARE(out.autoFormFill, in.autoFormFill);
    QCOMPARE(out.annotationColor, in.annotationColor);
    QCOMPARE(out.annotationAuthor, in.annotationAuthor);
    QCOMPARE(out.ocrDefaultLanguage, in.ocrDefaultLanguage);
}

// Every field the pages show comes back as it went in, so a value is never
// silently replaced by a combo's first entry.
void TstSettingsDialog::everyFieldRoundTrips()
{
    mervin::Settings in;
    in.defaultZoom = QStringLiteral("150");
    in.pageMode = QStringLiteral("single");
    in.twoPageSpread = true;
    in.colorScheme = QStringLiteral("light");
    in.documentTheme = QStringLiteral("comfort");
    in.accentColor = QStringLiteral("#112233");
    in.openBehavior = QStringLiteral("new-window");
    in.restoreSession = false;
    in.unloadInactiveMinutes = 17;
    in.closeToTray = false;
    in.recentVisibleCount = 42;
    in.recentRetention = 900;
    in.recentKeepMissing = false;
    in.recentSearchScope = QStringLiteral("contents"); // neither the default nor the first item
    in.autoUpdate = false;
    in.measurementSnap = false;
    in.measurementType = QStringLiteral("angle");
    in.measurementUnit = QStringLiteral("ft");
    in.measurementPrecision = 3;
    in.measurementLineWidth = 4.0;
    in.highlightFormFields = false;
    in.autoFormFill = false;
    in.annotationColor = QStringLiteral("#5AB4FF");
    in.annotationAuthor = QStringLiteral("  Ann Reviewer  ");
    in.ocrDefaultLanguage = QStringLiteral("swe");

    const mervin::Settings out = SettingsDialog(in).settings();
    QCOMPARE(out.defaultZoom, in.defaultZoom);
    QCOMPARE(out.pageMode, in.pageMode);
    QCOMPARE(out.twoPageSpread, in.twoPageSpread);
    QCOMPARE(out.colorScheme, in.colorScheme);
    QCOMPARE(out.documentTheme, in.documentTheme);
    QCOMPARE(out.accentColor, in.accentColor);
    QCOMPARE(out.openBehavior, in.openBehavior);
    QCOMPARE(out.restoreSession, in.restoreSession);
    QCOMPARE(out.unloadInactiveMinutes, in.unloadInactiveMinutes);
    QCOMPARE(out.closeToTray, in.closeToTray);
    QCOMPARE(out.recentVisibleCount, in.recentVisibleCount);
    QCOMPARE(out.recentRetention, in.recentRetention);
    QCOMPARE(out.recentKeepMissing, in.recentKeepMissing);
    QCOMPARE(out.recentSearchScope, in.recentSearchScope);
    QCOMPARE(out.autoUpdate, in.autoUpdate);
    QCOMPARE(out.measurementSnap, in.measurementSnap);
    QCOMPARE(out.measurementType, in.measurementType);
    QCOMPARE(out.measurementUnit, in.measurementUnit);
    QCOMPARE(out.measurementPrecision, in.measurementPrecision);
    QCOMPARE(out.measurementLineWidth, in.measurementLineWidth);
    QCOMPARE(out.highlightFormFields, in.highlightFormFields);
    QCOMPARE(out.autoFormFill, in.autoFormFill);
    QCOMPARE(out.annotationColor, in.annotationColor);
    QCOMPARE(out.annotationAuthor, QStringLiteral("Ann Reviewer")); // trimmed, as TabPage reads it
    QCOMPARE(out.ocrDefaultLanguage, in.ocrDefaultLanguage);
}

void TstSettingsDialog::memoryAndTrayControls()
{
    SettingsDialog dialog(mervin::Settings{});
    auto *duration = dialog.findChild<QSpinBox *>(QStringLiteral("unloadInactiveMinutes"));
    auto *never = dialog.findChild<QCheckBox *>(QStringLiteral("neverUnloadDocuments"));
    auto *tray = dialog.findChild<QCheckBox *>(QStringLiteral("closeToTray"));
    QVERIFY(duration && never && tray);
    QCOMPARE(duration->value(), 30);
    QVERIFY(duration->isEnabled());
    QVERIFY(!never->isChecked());
    QVERIFY(tray->isChecked());
    auto *hint = dialog.findChild<QLabel *>(QStringLiteral("unloadHint"));
    QVERIFY(hint);
    QVERIFY(hint->isHidden()); // a valid timeout needs no explanation

    duration->setValue(17);
    never->click();
    QCOMPARE(dialog.settings().unloadInactiveMinutes, 0);
    QVERIFY(!duration->isEnabled());
    QVERIFY(!hint->isHidden());
    QCOMPARE(hint->text(), QStringLiteral("Keep documents loaded, including in the tray."));
    tray->setChecked(false);
    QCOMPARE(dialog.settings().unloadInactiveMinutes, 0);
    QVERIFY(!dialog.settings().closeToTray);
    QVERIFY(labelStartingWith(&dialog, QStringLiteral("Keep documents loaded, including in the tray.")));

    // Toggling Never preserves the number the user just entered.
    never->click();
    QVERIFY(duration->isEnabled());
    QCOMPARE(duration->value(), 17);
    QCOMPARE(dialog.settings().unloadInactiveMinutes, 17);
    QVERIFY(hint->isHidden());

    mervin::Settings savedNever;
    savedNever.unloadInactiveMinutes = 0;
    SettingsDialog reopened(savedNever);
    auto *reopenedDuration = reopened.findChild<QSpinBox *>(QStringLiteral("unloadInactiveMinutes"));
    auto *reopenedNever = reopened.findChild<QCheckBox *>(QStringLiteral("neverUnloadDocuments"));
    QVERIFY(reopenedDuration && reopenedNever);
    QVERIFY(reopenedNever->isChecked());
    QVERIFY(!reopenedDuration->isEnabled());
    QCOMPARE(reopened.settings().unloadInactiveMinutes, 0);
    reopenedNever->click();
    QVERIFY(reopenedDuration->isEnabled());
    QCOMPARE(reopenedDuration->value(), 30);
    QCOMPARE(reopened.settings().unloadInactiveMinutes, 30);
}

void TstSettingsDialog::customUnloadDuration_data()
{
    QTest::addColumn<QString>("text");
    QTest::addColumn<int>("minutes");
    QTest::newRow("whole minutes") << QStringLiteral("17") << 17;
    QTest::newRow("minimum") << QStringLiteral("1") << 1;
    QTest::newRow("maximum") << QStringLiteral("10080") << mervin::Settings::kMaxUnloadInactiveMinutes;
}

void TstSettingsDialog::customUnloadDuration()
{
    QFETCH(QString, text);
    QFETCH(int, minutes);
    SettingsDialog dialog(mervin::Settings{});
    auto *duration = dialog.findChild<QSpinBox *>(QStringLiteral("unloadInactiveMinutes"));
    auto *buttons = dialog.findChild<QDialogButtonBox *>();
    QVERIFY(duration && buttons);
    auto *editor = duration->findChild<QLineEdit *>();
    QVERIFY(editor);
    editor->selectAll();
    QTest::keyClicks(editor, text);
    QCOMPARE(editor->text(), text);
    QCOMPARE(dialog.settings().unloadInactiveMinutes, minutes);
    QVERIFY(buttons->button(QDialogButtonBox::Ok)->isEnabled());
    QSignalSpy accepted(&dialog, &QDialog::accepted);
    buttons->button(QDialogButtonBox::Ok)->click();
    QCOMPARE(accepted.size(), 1);

    SettingsDialog reopened(dialog.settings());
    QCOMPARE(reopened.settings().unloadInactiveMinutes, minutes);
}

void TstSettingsDialog::unloadDurationRejectsText()
{
    SettingsDialog dialog(mervin::Settings{});
    auto *duration = dialog.findChild<QSpinBox *>(QStringLiteral("unloadInactiveMinutes"));
    QVERIFY(duration);
    auto *editor = duration->findChild<QLineEdit *>();
    QVERIFY(editor);
    editor->setCursorPosition(editor->text().size());
    QTest::keyClicks(editor, "minutes.Never");
    QCOMPARE(editor->text(), QStringLiteral("30"));

    // Pasting a suffix, a decimal, or a word must not replace the valid number.
    const QString previousClipboard = QApplication::clipboard()->text();
    for (const QString &text : {QStringLiteral("30 minutes"), QStringLiteral("1.5"),
                                QStringLiteral("Never"), QStringLiteral("soon")}) {
        QApplication::clipboard()->setText(text);
        editor->selectAll();
        QTest::keySequence(editor, QKeySequence::Paste);
        QCOMPARE(editor->text(), QStringLiteral("30"));
        QCOMPARE(dialog.settings().unloadInactiveMinutes, 30);
    }
    QApplication::clipboard()->setText(previousClipboard);
}

void TstSettingsDialog::unloadDurationUsesSpinBoxLocale()
{
    mervin::Settings in;
    in.unloadInactiveMinutes = 17;
    SettingsDialog dialog(in);
    auto *duration = dialog.findChild<QSpinBox *>(QStringLiteral("unloadInactiveMinutes"));
    auto *buttons = dialog.findChild<QDialogButtonBox *>();
    QVERIFY(duration && buttons);
    const QLocale locale(QStringLiteral("ar_EG"));
    duration->setLocale(locale);
    QCOMPARE(duration->text(), locale.toString(17));
    QVERIFY(buttons->button(QDialogButtonBox::Ok)->isEnabled());
    QCOMPARE(dialog.settings().unloadInactiveMinutes, 17);

    // Localized digits must save the new duration instead of preserving the old one.
    auto *editor = duration->findChild<QLineEdit *>();
    QVERIFY(editor);
    editor->setText(locale.toString(42));
    QVERIFY(duration->hasAcceptableInput());
    QVERIFY(buttons->button(QDialogButtonBox::Ok)->isEnabled());
    QCOMPARE(dialog.settings().unloadInactiveMinutes, 42);
    QSignalSpy accepted(&dialog, &QDialog::accepted);
    buttons->button(QDialogButtonBox::Ok)->click();
    QCOMPARE(accepted.size(), 1);
    SettingsDialog reopened(dialog.settings());
    QCOMPARE(reopened.settings().unloadInactiveMinutes, 42);
}

void TstSettingsDialog::invalidUnloadDurationCannotBeAccepted_data()
{
    QTest::addColumn<QString>("text");
    for (const char *text : {"", "0", "-1", "1.5", "10081", "2147483648", "soon", "30 minutes", "Never"})
        QTest::newRow(*text ? text : "empty") << QString::fromLatin1(text);
}

void TstSettingsDialog::invalidUnloadDurationCannotBeAccepted()
{
    QFETCH(QString, text);
    SettingsDialog dialog(mervin::Settings{});
    auto *duration = dialog.findChild<QSpinBox *>(QStringLiteral("unloadInactiveMinutes"));
    auto *never = dialog.findChild<QCheckBox *>(QStringLiteral("neverUnloadDocuments"));
    auto *buttons = dialog.findChild<QDialogButtonBox *>();
    QVERIFY(duration && never && buttons);
    auto *editor = duration->findChild<QLineEdit *>();
    QVERIFY(editor);
    // Bypass interactive validation to cover blank and invalid pending edits.
    editor->setText(text);
    dialog.findChild<QCheckBox *>(QStringLiteral("closeToTray"))->click(); // another pending change
    QVERIFY(!buttons->button(QDialogButtonBox::Ok)->isEnabled());
    QVERIFY(!buttons->button(QDialogButtonBox::Apply)->isEnabled());
    QVERIFY(labelStartingWith(&dialog, QStringLiteral("Enter 1 to 10080 whole minutes, or check Never.")));
    QSignalSpy accepted(&dialog, &QDialog::accepted);
    dialog.showPage(Page::Appearance);
    dialog.accept();
    QCOMPARE(accepted.size(), 0);
    QCOMPARE(dialog.currentPage(), Page::General);
    QCOMPARE(dialog.settings().unloadInactiveMinutes, 30);

    never->click();
    QVERIFY(buttons->button(QDialogButtonBox::Ok)->isEnabled());
    QVERIFY(buttons->button(QDialogButtonBox::Apply)->isEnabled()); // Never is a change
    dialog.accept();
    QCOMPARE(accepted.size(), 1);
    QCOMPARE(dialog.settings().unloadInactiveMinutes, 0);
}

void TstSettingsDialog::documentThemesRespondToMouseAndKeyboard()
{
    SettingsDialog dialog(mervin::Settings{}, {}, Page::Appearance);
    auto *picker = dialog.findChild<mervin::DocumentThemePicker *>();
    QVERIFY(picker);
    auto *traditional = picker->findChild<QRadioButton *>(QStringLiteral("documentThemeLight"));
    auto *comfort = picker->findChild<QRadioButton *>(QStringLiteral("documentThemeComfort"));
    auto *inverted = picker->findChild<QRadioButton *>(QStringLiteral("documentThemeDark"));
    QVERIFY(traditional && comfort && inverted);
    dialog.show();
    QVERIFY(QTest::qWaitForWindowExposed(&dialog));
    QVERIFY(traditional->isChecked());
    QCOMPARE(picker->theme(), QStringLiteral("light"));

    // Clicking the preview itself selects the theme and persists its key.
    QTest::mouseClick(comfort, Qt::LeftButton, Qt::NoModifier, QPoint(comfort->width() / 2, 20));
    QVERIFY(comfort->isChecked());
    QCOMPARE(dialog.settings().documentTheme, QStringLiteral("comfort"));
    QTest::mouseClick(inverted, Qt::LeftButton, Qt::NoModifier, QPoint(inverted->width() / 2, 20));
    QVERIFY(inverted->isChecked());
    QCOMPARE(dialog.settings().documentTheme, QStringLiteral("dark"));

    // Left and right follow the displayed Traditional, Comfort, Inverted order.
    inverted->setFocus();
    QTest::keyClick(inverted, Qt::Key_Left);
    QVERIFY(comfort->isChecked());
    QCOMPARE(dialog.settings().documentTheme, QStringLiteral("comfort"));
    QTest::keyClick(comfort, Qt::Key_Left);
    QVERIFY(traditional->isChecked());
    QCOMPARE(dialog.settings().documentTheme, QStringLiteral("light"));
    QTest::keyClick(traditional, Qt::Key_Right);
    QVERIFY(comfort->isChecked());
    QCOMPARE(dialog.settings().documentTheme, QStringLiteral("comfort"));

    SettingsDialog reopened(dialog.settings(), {}, Page::Appearance);
    QCOMPARE(reopened.settings().documentTheme, QStringLiteral("comfort"));
    QVERIFY(reopened.findChild<QRadioButton *>(QStringLiteral("documentThemeComfort"))->isChecked());
}

void TstSettingsDialog::legacyDocumentThemeIsPreserved()
{
    mervin::Settings in;
    in.documentTheme = QStringLiteral("follow-ui");
    SettingsDialog dialog(in, {}, Page::Appearance);
    auto *picker = dialog.findChild<mervin::DocumentThemePicker *>();
    auto *followUi = dialog.findChild<QRadioButton *>(QStringLiteral("documentThemeFollowUi"));
    auto *traditional = dialog.findChild<QRadioButton *>(QStringLiteral("documentThemeLight"));
    QVERIFY(picker && followUi && traditional);
    QVERIFY(followUi->isChecked());
    QCOMPARE(picker->theme(), in.documentTheme);
    QCOMPARE(dialog.settings().documentTheme, in.documentTheme);

    traditional->click();
    QCOMPARE(dialog.settings().documentTheme, QStringLiteral("light"));
    followUi->click();
    QCOMPARE(dialog.settings().documentTheme, in.documentTheme);
}

void TstSettingsDialog::uiThemesRespondToMouseAndKeyboard()
{
    SettingsDialog dialog(mervin::Settings{}, {}, Page::Appearance);
    auto *dark = dialog.findChild<QRadioButton *>(QStringLiteral("uiThemeDark"));
    auto *light = dialog.findChild<QRadioButton *>(QStringLiteral("uiThemeLight"));
    auto *system = dialog.findChild<QRadioButton *>(QStringLiteral("uiThemeSystem"));
    QVERIFY(dark && light && system);
    dialog.show();
    QVERIFY(QTest::qWaitForWindowExposed(&dialog));
    QVERIFY(dark->isChecked());

    // Clicking the preview itself selects the scheme and persists its key.
    QTest::mouseClick(system, Qt::LeftButton, Qt::NoModifier, QPoint(system->width() / 2, 20));
    QVERIFY(system->isChecked());
    QCOMPARE(dialog.settings().colorScheme, QStringLiteral("system"));
    QTest::mouseClick(light, Qt::LeftButton, Qt::NoModifier, QPoint(light->width() / 2, 20));
    QVERIFY(light->isChecked());
    QCOMPARE(dialog.settings().colorScheme, QStringLiteral("light"));

    // Left and right follow the displayed Dark, Light, Follow system order.
    light->setFocus();
    QTest::keyClick(light, Qt::Key_Left);
    QVERIFY(dark->isChecked());
    QCOMPARE(dialog.settings().colorScheme, QStringLiteral("dark"));
    QTest::keyClick(dark, Qt::Key_Right);
    QTest::keyClick(light, Qt::Key_Right);
    QVERIFY(system->isChecked());
    QCOMPARE(dialog.settings().colorScheme, QStringLiteral("system"));

    // The app follows the system for a value the picker does not offer, so the
    // picker shows Follow system and choosing Dark is a change to apply.
    mervin::Settings unknown;
    unknown.colorScheme = QStringLiteral("Dark");
    SettingsDialog reopened(unknown, {}, Page::Appearance);
    QVERIFY(reopened.findChild<QRadioButton *>(QStringLiteral("uiThemeSystem"))->isChecked());
    QCOMPARE(reopened.settings().colorScheme, QStringLiteral("system"));
    QVERIFY(!dialogButton(&reopened, QDialogButtonBox::Apply)->isEnabled());
    reopened.findChild<QRadioButton *>(QStringLiteral("uiThemeDark"))->click();
    QCOMPARE(reopened.settings().colorScheme, QStringLiteral("dark"));
    QVERIFY(dialogButton(&reopened, QDialogButtonBox::Apply)->isEnabled());
}

// Apply hands the pending changes over and keeps the dialog open. It is
// available only while a control differs from what was last applied.
void TstSettingsDialog::applyHandsOverPendingChanges()
{
    SettingsDialog dialog(mervin::Settings{}, {}, Page::Appearance);
    QList<mervin::Settings> applied;
    connect(&dialog, &SettingsDialog::applyRequested, this,
            [&applied](const mervin::Settings &s) { applied.append(s); });
    QSignalSpy accepted(&dialog, &QDialog::accepted);
    QPushButton *apply = dialogButton(&dialog, QDialogButtonBox::Apply);
    auto *dark = dialog.findChild<QRadioButton *>(QStringLiteral("uiThemeDark"));
    auto *light = dialog.findChild<QRadioButton *>(QStringLiteral("uiThemeLight"));
    QVERIFY(apply && dark && light);
    QVERIFY(!apply->isEnabled());

    light->click();
    QVERIFY(apply->isEnabled());
    dark->click(); // back to the applied value: nothing to apply
    QVERIFY(!apply->isEnabled());

    light->click();
    apply->click();
    QCOMPARE(applied.size(), 1);
    QCOMPARE(applied.first().colorScheme, QStringLiteral("light"));
    QVERIFY(!apply->isEnabled());
    QCOMPARE(accepted.size(), 0);

    // Later edits are compared with the applied values, not the opening ones.
    dark->click();
    QVERIFY(apply->isEnabled());
    light->click();
    QVERIFY(!apply->isEnabled());

    // OK with nothing pending closes without applying again.
    dialogButton(&dialog, QDialogButtonBox::Ok)->click();
    QCOMPARE(accepted.size(), 1);
    QCOMPARE(applied.size(), 1);
}

void TstSettingsDialog::okAppliesPendingChangesAndCancelDropsThem()
{
    for (const auto button : {QDialogButtonBox::Ok, QDialogButtonBox::Cancel}) {
        SettingsDialog dialog(mervin::Settings{}, {}, Page::Appearance);
        QList<mervin::Settings> applied;
        connect(&dialog, &SettingsDialog::applyRequested, this,
                [&applied](const mervin::Settings &s) { applied.append(s); });
        dialog.findChild<QRadioButton *>(QStringLiteral("documentThemeComfort"))->click();
        dialogButton(&dialog, button)->click();
        if (button == QDialogButtonBox::Ok) {
            QCOMPARE(dialog.result(), int(QDialog::Accepted));
            QCOMPARE(applied.size(), 1);
            QCOMPARE(applied.first().documentTheme, QStringLiteral("comfort"));
        } else {
            QCOMPARE(dialog.result(), int(QDialog::Rejected));
            QVERIFY(applied.isEmpty());
        }
    }
}

// Every kind of control turns Apply on, including the values the dialog keeps
// outside a widget: the accent, the highlight swatches and the OCR language.
void TstSettingsDialog::everyKindOfControlEnablesApply()
{
    using Edit = std::function<void(SettingsDialog &)>;
    const QList<QPair<const char *, Edit>> edits = {
        {"combo box",
         [](SettingsDialog &d) {
             for (QComboBox *combo : d.findChildren<QComboBox *>())
                 if (const int i = combo->findData(QStringLiteral("fit-page")); i >= 0)
                     combo->setCurrentIndex(i);
         }},
        {"check box",
         [](SettingsDialog &d) {
             checkBox(&d, QStringLiteral("Reopen my tabs when Mervin starts"))->click();
         }},
        {"typed number",
         [](SettingsDialog &d) {
             auto *editor = d.findChild<QSpinBox *>(QStringLiteral("unloadInactiveMinutes"))
                                ->findChild<QLineEdit *>();
             editor->selectAll();
             QTest::keyClicks(editor, "45");
         }},
        {"text field",
         [](SettingsDialog &d) {
             QTest::keyClicks(d.findChild<QLineEdit *>(QStringLiteral("annotationAuthor")), "Ann");
         }},
        {"highlight swatch",
         [](SettingsDialog &d) {
             const QString other = mervin::annot::palette()[1].name();
             for (QToolButton *swatch : d.findChildren<QToolButton *>())
                 if (swatch->toolTip() == other)
                     swatch->click();
         }},
        {"accent reset",
         [](SettingsDialog &d) {
             for (QPushButton *button : d.findChildren<QPushButton *>())
                 if (button->text() == QStringLiteral("Reset"))
                     button->click();
         }},
        {"document theme",
         [](SettingsDialog &d) {
             d.findChild<QRadioButton *>(QStringLiteral("documentThemeComfort"))->click();
         }},
        {"UI theme",
         [](SettingsDialog &d) {
             d.findChild<QRadioButton *>(QStringLiteral("uiThemeSystem"))->click();
         }},
        {"OCR language",
         [](SettingsDialog &d) {
             auto *combo = d.findChild<QComboBox *>(QStringLiteral("ocrDefaultLanguage"));
             const int swe = combo->findData(QStringLiteral("swe"));
             combo->setCurrentIndex(swe);
             emit combo->activated(swe);
         }},
    };
    // Values the controls normalise (a lower-case accent, the legacy measure type)
    // are not changes, so Apply starts disabled. The custom accent lets Reset act.
    mervin::Settings in;
    in.accentColor = QStringLiteral("#aabbcc");
    in.measurementType = QStringLiteral("polyline");
    for (const auto &[name, edit] : edits) {
        SettingsDialog dialog(in);
        QPushButton *apply = dialogButton(&dialog, QDialogButtonBox::Apply);
        QVERIFY2(apply && !apply->isEnabled(), name);
        edit(dialog);
        QVERIFY2(apply->isEnabled(), name);
    }
}

// config.toml is hand-editable. Spellings and values the tabs accept must land on
// the matching choice instead of resetting to the first one.
void TstSettingsDialog::handEditedMeasureValuesLandOnOfferedChoices()
{
    mervin::Settings in;
    in.measurementType = QStringLiteral("polyline");
    in.measurementUnit = QStringLiteral("Inch");
    in.measurementLineWidth = 2.5; // between 2 and 3: the panel rounds down
    in.measurementPrecision = 9;   // the panel offers 0 to 4

    const mervin::Settings out = SettingsDialog(in).settings();
    QCOMPARE(out.measurementType, QStringLiteral("path"));
    QCOMPARE(out.measurementUnit, QStringLiteral("in"));
    QCOMPARE(out.measurementLineWidth, 2.0);
    QCOMPARE(out.measurementPrecision, 4);

    in.measurementUnit = QStringLiteral("furlong"); // unknown: TabPage falls back to mm
    QCOMPARE(SettingsDialog(in).settings().measurementUnit, QStringLiteral("mm"));
}

// Another window switches updates off while Settings is open. OK must not
// switch them back on.
void TstSettingsDialog::autoUpdateFollowsAnOutsideChange()
{
    mervin::Settings in;
    in.autoUpdate = true;
    SettingsDialog dialog(in);
    QCheckBox *box = checkBox(&dialog, QStringLiteral("Update automatically at start (every 30 days)"));
    QVERIFY(box);
    QVERIFY(box->isChecked());

    dialog.setAutoUpdate(false);
    QVERIFY(!box->isChecked());
    QVERIFY(!dialog.settings().autoUpdate);
    QVERIFY(!dialogButton(&dialog, QDialogButtonBox::Apply)->isEnabled()); // already in effect

    // The user can still turn it back on afterwards.
    box->setChecked(true);
    QVERIFY(dialog.settings().autoUpdate);
    QVERIFY(dialogButton(&dialog, QDialogButtonBox::Apply)->isEnabled());
}

void TstSettingsDialog::updateControlsFollowTheUpdater()
{
    // No Updater (tests, embedded use): no check button and no date.
    {
        SettingsDialog dialog(mervin::Settings{});
        QVERIFY(!dialog.findChild<QPushButton *>(QStringLiteral("checkForUpdatesButton")));
        QVERIFY(!labelStartingWith(&dialog, QStringLiteral("Never checked")));
    }

    // An installed copy: the button asks MainWindow to check, the date follows.
    {
        SettingsDialog::UpdateInfo info;
        info.checkAvailable = true;
        info.canSelfUpdate = true;
        SettingsDialog dialog(mervin::Settings{}, info, Page::General);
        auto *button = dialog.findChild<QPushButton *>(QStringLiteral("checkForUpdatesButton"));
        QVERIFY(button);
        QSignalSpy requested(&dialog, &SettingsDialog::checkForUpdatesRequested);
        button->click();
        QCOMPARE(requested.count(), 1);
        QVERIFY(labelStartingWith(&dialog, QStringLiteral("Never checked")));

        dialog.setLastUpdateCheck(QDateTime(QDate(2026, 10, 2), QTime(12, 0), QTimeZone::UTC));
        QVERIFY(!labelStartingWith(&dialog, QStringLiteral("Never checked")));
        QLabel *date = labelStartingWith(&dialog, QStringLiteral("Last checked "));
        QVERIFY(date);
        QVERIFY(date->text().contains(QStringLiteral("2026")));
        // The OS long date, without the weekday it normally carries.
        const QDate shown = QDateTime(QDate(2026, 10, 2), QTime(12, 0), QTimeZone::UTC).toLocalTime().date();
        QVERIFY(!date->text().contains(QLocale().dayName(shown.dayOfWeek())));
        QVERIFY(checkBox(&dialog, QStringLiteral("Update automatically at start (every 30 days)"))->isEnabled());
    }

    // A portable or dev copy never checks on its own, so there is no switch. The
    // saved value is left for an installed copy.
    {
        SettingsDialog::UpdateInfo info;
        info.checkAvailable = true;
        info.canSelfUpdate = false;
        mervin::Settings in;
        in.autoUpdate = true;
        SettingsDialog dialog(in, info, Page::General);
        QVERIFY(checkBox(&dialog, QStringLiteral("Update automatically at start (every 30 days)"))->isHidden());
        QVERIFY(labelStartingWith(&dialog, QStringLiteral("This copy can't update itself")));
        QVERIFY(dialog.findChild<QPushButton *>(QStringLiteral("checkForUpdatesButton")));
        QVERIFY(dialog.settings().autoUpdate);
    }
}

// Enter on a menu row moves into the page; it must not press OK and apply
// everything (a lowered retention trims the history at once).
void TstSettingsDialog::enterOnTheMenuDoesNotPressOk()
{
    SettingsDialog dialog(mervin::Settings{}, {}, Page::Viewing);
    dialog.show();
    QVERIFY(QTest::qWaitForWindowExposed(&dialog));
    auto *nav = dialog.findChild<QListWidget *>(QStringLiteral("settingsNav"));
    QVERIFY(nav);
    nav->setFocus();
    QTest::keyClick(nav, Qt::Key_Return);
    QVERIFY(dialog.isVisible());
    QCOMPARE(dialog.result(), int(QDialog::Rejected));
    QVERIFY(dialog.focusWidget() && dialog.focusWidget() != nav);
}

// The page shows an installed language, like the OCR picker's fallback, but OK
// keeps a saved default whose model is away until the user picks another one.
void TstSettingsDialog::ocrPageShowsAnInstalledDefaultButKeepsTheSavedOne()
{
    mervin::Settings in;
    in.ocrDefaultLanguage = QStringLiteral("deu"); // not installed in this profile
    SettingsDialog dialog(in);
    auto *combo = dialog.findChild<QComboBox *>(QStringLiteral("ocrDefaultLanguage"));
    QVERIFY(combo);
    QCOMPARE(combo->count(), 2);
    QCOMPARE(combo->currentData().toString(), QStringLiteral("eng"));
    QCOMPARE(dialog.settings().ocrDefaultLanguage, QStringLiteral("deu"));

    // Picking an installed language is a real choice and is saved.
    const int swe = combo->findData(QStringLiteral("swe"));
    QVERIFY(swe >= 0);
    combo->setCurrentIndex(swe);
    emit combo->activated(swe);
    QCOMPARE(dialog.settings().ocrDefaultLanguage, QStringLiteral("swe"));
}

namespace {

QToolButton *removeButtonFor(QWidget *dialog, const QString &language)
{
    for (auto *button : dialog->findChildren<QToolButton *>(QStringLiteral("settingsListRemove")))
        if (button->toolTip() == QStringLiteral("Remove %1").arg(language))
            return button;
    return nullptr;
}

// Answers the next message box with `button`.
void answerNextBox(QObject *context, QMessageBox::StandardButton button)
{
    QTimer::singleShot(0, context, [button] {
        auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
        QVERIFY(box);
        box->button(button)->click();
    });
}

} // namespace

// Removal deletes the model at once, so it asks first; Cancel keeps the file.
// Removing the saved default moves it to a language that is still installed.
void TstSettingsDialog::removingAnOcrLanguageAsksFirst()
{
    mervin::Settings in;
    in.ocrDefaultLanguage = QStringLiteral("swe");
    SettingsDialog dialog(in, {}, Page::Ocr);

    QToolButton *removeSwe = removeButtonFor(&dialog, QStringLiteral("Swedish"));
    QVERIFY(removeSwe);
    answerNextBox(this, QMessageBox::Cancel);
    removeSwe->click();
    QVERIFY(QFile::exists(tessdataPath("swe")));
    QCOMPARE(dialog.settings().ocrDefaultLanguage, QStringLiteral("swe"));

    answerNextBox(this, QMessageBox::Yes);
    removeSwe->click();
    QVERIFY(!QFile::exists(tessdataPath("swe")));
    QVERIFY(!removeButtonFor(&dialog, QStringLiteral("Swedish")));
    QCOMPARE(dialog.findChild<QComboBox *>(QStringLiteral("ocrDefaultLanguage"))->count(), 1);
    QCOMPARE(dialog.settings().ocrDefaultLanguage, QStringLiteral("eng"));
    QVERIFY(dialogButton(&dialog, QDialogButtonBox::Apply)->isEnabled());
}

// Keep at least one installed OCR language available through the settings UI.
void TstSettingsDialog::theLastOcrLanguageStays()
{
    QVERIFY(QFile::remove(tessdataPath("swe")));
    SettingsDialog dialog(mervin::Settings{}, {}, Page::Ocr);
    const auto buttons =
        dialog.findChildren<QToolButton *>(QStringLiteral("settingsListRemove"));
    QCOMPARE(buttons.size(), 1);
    QVERIFY(!buttons.first()->isEnabled());
}

// Picking a language other than the one Mervin shows says "Mervin will restart.";
// Apply then hands it over and closes the dialog like OK, so the caller restarts.
void TstSettingsDialog::newUiLanguageRestartsOnApply()
{
    SettingsDialog dialog(mervin::Settings{});
    auto *combo = dialog.findChild<mervin::LanguageCombo *>(QStringLiteral("uiLanguage"));
    QLabel *hint = labelStartingWith(&dialog, QStringLiteral("Mervin will restart"));
    QPushButton *apply = dialogButton(&dialog, QDialogButtonBox::Apply);
    QVERIFY(combo && hint && apply);
    QCOMPARE(combo->language(), mervin::i18n::current());
    QVERIFY(!dialog.restartNeeded());
    QVERIFY(hint->isHidden());

    QList<mervin::Settings> applied;
    connect(&dialog, &SettingsDialog::applyRequested, this,
            [&applied](const mervin::Settings &s) { applied.append(s); });
    QSignalSpy accepted(&dialog, &QDialog::accepted);
    combo->setLanguage(QStringLiteral("sv"));
    QVERIFY(dialog.restartNeeded());
    QVERIFY(!hint->isHidden());
    QVERIFY(apply->isEnabled());
    QCOMPARE(dialog.settings().uiLanguage, QStringLiteral("sv"));

    // Back to the language shown: nothing to restart for.
    combo->setLanguage(mervin::i18n::current());
    QVERIFY(!dialog.restartNeeded());
    QVERIFY(hint->isHidden());

    combo->setLanguage(QStringLiteral("zh_CN"));
    apply->click();
    QCOMPARE(applied.size(), 1);
    QCOMPARE(applied.first().uiLanguage, QStringLiteral("zh_CN"));
    QCOMPARE(accepted.size(), 1);
}

// Opening and confirming Settings never rewrites the stored language: not an
// empty one (no choice yet) and not one this build doesn't ship.
void TstSettingsDialog::untouchedUiLanguageKeepsTheStoredValue()
{
    for (const QString &stored : {QString(), QStringLiteral("pt_BR"), QStringLiteral("sv")}) {
        mervin::Settings in;
        in.uiLanguage = stored;
        SettingsDialog dialog(in);
        QCOMPARE(dialog.settings().uiLanguage, stored);
    }
}

// A failed save shows why and keeps the dialog open with the change pending, so a
// new language can't restart Mervin without being stored. OK then saves again.
void TstSettingsDialog::failedSaveKeepsTheDialogOpen()
{
    SettingsDialog dialog(mervin::Settings{});
    auto *combo = dialog.findChild<mervin::LanguageCombo *>(QStringLiteral("uiLanguage"));
    QVERIFY(combo);
    int failures = 2;
    QList<mervin::Settings> applied;
    connect(&dialog, &SettingsDialog::applyRequested, this,
            [&](const mervin::Settings &s) {
                applied.append(s);
                if (failures-- > 0)
                    dialog.reportSaveFailure(QStringLiteral("Permission denied"));
            });
    QSignalSpy accepted(&dialog, &QDialog::accepted);
    QString shown;
    const auto readNextBox = [this, &shown] {
        shown.clear();
        QTimer::singleShot(0, this, [&shown] {
            auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
            QVERIFY(box);
            shown = box->text();
            box->button(QMessageBox::Ok)->click();
        });
    };

    combo->setLanguage(QStringLiteral("sv"));
    readNextBox();
    dialogButton(&dialog, QDialogButtonBox::Apply)->click();
    QVERIFY(shown.contains(QStringLiteral("Permission denied")));
    QCOMPARE(accepted.size(), 0);
    QVERIFY(dialog.restartNeeded());
    QVERIFY(dialogButton(&dialog, QDialogButtonBox::Apply)->isEnabled());

    readNextBox();
    dialogButton(&dialog, QDialogButtonBox::Ok)->click();
    QVERIFY(shown.contains(QStringLiteral("Permission denied")));
    QCOMPARE(accepted.size(), 0);

    dialogButton(&dialog, QDialogButtonBox::Ok)->click();
    QCOMPARE(applied.size(), 3);
    QCOMPARE(applied.last().uiLanguage, QStringLiteral("sv"));
    QCOMPARE(accepted.size(), 1);
}

// The menu on the left keeps its 196 px for the English page titles and widens
// for a longer one, which would otherwise be cut off: it never scrolls sideways.
void TstSettingsDialog::menuFitsLongerPageTitles()
{
    const QString previousStyle = qApp->styleSheet();
    const auto restoreStyle = qScopeGuard([&] { qApp->setStyleSheet(previousStyle); });
    const QPalette palette = mervin::theme::darkPalette(QColor(QStringLiteral("#4f8cff")));
    qApp->setStyleSheet(mervin::Theme::buildStyleSheet(palette, QStringLiteral("#4f8cff")));

    int englishWidth = 0;
    {
        SettingsDialog dialog(mervin::Settings{});
        auto *nav = dialog.findChild<QListWidget *>(QStringLiteral("settingsNav"));
        QVERIFY(nav);
        englishWidth = nav->width();
    }
    // Without fonts (Windows' offscreen platform in CI) Qt draws every letter
    // as a wide box, so the English titles need more than 196 px there too.
    if (QFontDatabase::families().isEmpty())
        QVERIFY(englishWidth >= 196);
    else
        QCOMPARE(englishWidth, 196);

    const QString longTitle = QStringLiteral("Tangentbordsgenvägar och kortkommandon för alla verktyg");
    StubCatalog catalog({{"SettingsDialog|Keyboard shortcuts", longTitle}});
    SettingsDialog dialog(mervin::Settings{}, {}, Page::Shortcuts);
    auto *nav = dialog.findChild<QListWidget *>(QStringLiteral("settingsNav"));
    QVERIFY(nav);
    QCOMPARE(nav->currentItem()->text(), longTitle);
    dialog.show();
    QVERIFY(QTest::qWaitForWindowExposed(&dialog));
    QVERIFY(nav->width() > englishWidth);
    QVERIFY(!nav->verticalScrollBar()->isVisible());
    const int needed = nav->iconSize().width() + nav->fontMetrics().horizontalAdvance(longTitle);
    QVERIFY2(nav->viewport()->width() >= needed, "The longest page title must fit the menu.");
}

// The shortcuts page writes keys the way the OS does, with key names from the UI
// language's catalog ("Strg" in German), not fixed English text.
void TstSettingsDialog::shortcutKeysFollowTheUiLanguage()
{
    const auto firstRowKeys = [] {
        SettingsDialog dialog(mervin::Settings{}, {}, Page::Shortcuts);
        const auto keys = dialog.findChildren<QLabel *>(QStringLiteral("shortcutKeys"));
        return keys.isEmpty() ? QString() : keys.first()->text();
    };
    QCOMPARE(firstRowKeys(), QStringLiteral("Ctrl+O"));

    StubCatalog german({{"QShortcut|Ctrl", QStringLiteral("Strg")}});
    QCOMPARE(firstRowKeys(), QStringLiteral("Strg+O"));
}

// A stored language other than the one on screen (its restart was cancelled at a
// save prompt) shows with the restart note, and picking the language on screen
// takes it back without a restart. A --language run shows the language on screen.
void TstSettingsDialog::pendingUiLanguageShowsUntilChanged()
{
    mervin::Settings in;
    in.uiLanguage = QStringLiteral("sv"); // the tests run in English
    {
        SettingsDialog dialog(in);
        auto *combo = dialog.findChild<mervin::LanguageCombo *>(QStringLiteral("uiLanguage"));
        QLabel *hint = labelStartingWith(&dialog, QStringLiteral("Mervin will restart"));
        QPushButton *apply = dialogButton(&dialog, QDialogButtonBox::Apply);
        QVERIFY(combo && hint && apply);
        QCOMPARE(combo->language(), QStringLiteral("sv"));
        QVERIFY(dialog.restartNeeded());
        QVERIFY(!hint->isHidden());
        QVERIFY(apply->isEnabled()); // Apply restarts, as OK does

        combo->setLanguage(QStringLiteral("en"));
        QVERIFY(!dialog.restartNeeded());
        QVERIFY(hint->isHidden());
        QCOMPARE(dialog.settings().uiLanguage, QStringLiteral("en"));
    }

    mervin::i18n::setOneRunOverride(true);
    SettingsDialog dialog(in);
    mervin::i18n::setOneRunOverride(false);
    auto *combo = dialog.findChild<mervin::LanguageCombo *>(QStringLiteral("uiLanguage"));
    QVERIFY(combo);
    QCOMPARE(combo->language(), mervin::i18n::current());
    QVERIFY(!dialog.restartNeeded());
    QCOMPARE(dialog.settings().uiLanguage, QStringLiteral("sv"));
}

QTEST_MAIN(TstSettingsDialog)
#include "tst_settings_dialog.moc"
