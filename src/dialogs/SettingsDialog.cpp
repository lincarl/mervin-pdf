#include "dialogs/SettingsDialog.h"

#include "config/ConfigPaths.h"
#include "dialogs/ManageLanguagesDialog.h"
#include "i18n/UiLanguage.h"
#include "mervin_version.h"
#include "ocr/TessdataManager.h"
#include "render/AnnotTypes.h"
#include "ui/DocumentThemePicker.h"
#include "ui/Icons.h"
#include "ui/LanguageCombo.h"
#include "ui/TextLayout.h"
#include "ui/Theme.h"
#include "ui/ThemeTokens.h"
#include "ui/UiThemePicker.h"

#ifdef Q_OS_WIN
#  include "platform/PlatformIntegration.h"
#endif

#include <QAbstractButton>
#include <QCheckBox>
#include <QColorDialog>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QEvent>
#include <QFile>
#include <QFileInfo>
#include <QFormLayout>
#include <QFrame>
#include <QKeyEvent>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QLocale>
#include <QMessageBox>
#include <QPalette>
#include <QPushButton>
#include <QRegularExpression>
#include <QScreen>
#include <QScrollArea>
#include <QSizePolicy>
#include <QSpinBox>
#include <QStackedWidget>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

namespace {

using Page = SettingsDialog::Page;

constexpr int kPageRole = Qt::UserRole;      // nav item -> Page
constexpr int kStackRole = Qt::UserRole + 1; // nav item -> index in the page stack

void selectByData(QComboBox *combo, const QString &value)
{
    const int idx = combo->findData(value);
    combo->setCurrentIndex(idx >= 0 ? idx : 0);
}

// FieldsStayAtSizeHint prevents platform styles stretching compact inputs while preserving
// label alignment and mnemonic buddies. WrapLongRows puts a field under its label when
// the two don't fit side by side (a long translated label), so the page doesn't scroll
// sideways. Pass nullptr for a form inside another layout.
QFormLayout *snugForm(QWidget *parent)
{
    auto *form = new QFormLayout(parent);
    form->setFieldGrowthPolicy(QFormLayout::FieldsStayAtSizeHint);
    form->setRowWrapPolicy(QFormLayout::WrapLongRows);
    return form;
}

// A box on a page. Without a title it is a frame drawn like a group box
// (QFrame#settingsBox), for groups the menu already names: a QGroupBox keeps the
// room for its title even when the title is empty.
QWidget *groupBox(const QString &title, QWidget *parent)
{
    if (!title.isEmpty())
        return new QGroupBox(title, parent);
    auto *frame = new QFrame(parent);
    frame->setObjectName(QStringLiteral("settingsBox"));
    return frame;
}

// Secondary text under a control: explains what it does in a sentence.
QLabel *hintLabel(const QString &text, QWidget *parent)
{
    auto *label = new QLabel(text, parent);
    label->setObjectName(QStringLiteral("settingsHint"));
    label->setWordWrap(true);
    return label;
}

QFrame *rule(QWidget *parent)
{
    auto *line = new QFrame(parent);
    line->setObjectName(QStringLiteral("settingsRule"));
    line->setFixedHeight(1);
    return line;
}

// One label width for every form on a page, so the fields of separate group
// boxes start at the same x instead of jumping from box to box.
void alignFormLabels(QWidget *page)
{
    QList<QLabel *> labels;
    int width = 0;
    for (QFormLayout *form : page->findChildren<QFormLayout *>()) {
        for (int row = 0; row < form->rowCount(); ++row) {
            QLayoutItem *item = form->itemAt(row, QFormLayout::LabelRole);
            auto *label = item ? qobject_cast<QLabel *>(item->widget()) : nullptr;
            if (!label || label->text().isEmpty())
                continue;
            labels.append(label);
            width = std::max(width, label->sizeHint().width());
        }
    }
    for (QLabel *label : labels)
        label->setMinimumWidth(width);
}

// The name new comments carry when the author field is empty (see TabPage).
QString osUserName()
{
    QString name = qEnvironmentVariable("USERNAME");
    if (name.isEmpty())
        name = qEnvironmentVariable("USER");
    return name;
}

QString formattedSize(qint64 bytes)
{
    return QLocale().formattedDataSize(bytes, 1, QLocale::DataSizeTraditionalFormat);
}

// The day in the OS regional long format without the weekday, such as
// "2 October 2026" or "October 2, 2026". Qt has no long format without it, so
// this drops the weekday field and the literal text up to the next field.
QString longDateWithoutWeekday(QDate day)
{
    const QLocale locale;
    static const QRegularExpression weekday(QStringLiteral("dddd?[^dMy']*"));
    // A weekday at the end leaves a space or comma (Latin, Arabic or ideographic).
    static const QRegularExpression trailing(QStringLiteral("[\\s,\u060C\u3001]+$"));
    QString format = locale.dateFormat(QLocale::LongFormat);
    format.remove(weekday).remove(trailing);
    return locale.toString(day, format);
}

mervin::icons::Glyph pageGlyph(Page page)
{
    using mervin::icons::Glyph;
    switch (page) {
    case Page::General: return Glyph::Settings;
    case Page::Appearance: return Glyph::Appearance;
    case Page::Viewing: return Glyph::Viewing;
    case Page::Annotations: return Glyph::Comments;
    case Page::Ocr: return Glyph::Ocr;
    case Page::Measuring: return Glyph::Measure;
    case Page::Forms: return Glyph::FillForm;
    case Page::Shortcuts: return Glyph::Keyboard;
    case Page::About: return Glyph::About;
    }
    return Glyph::Settings;
}

// The bindings MainWindow::createActions sets up. Menu rows carry no shortcut
// hints (see MainWindow::hideShortcutHints), so this page is the only place they
// are written down - keep it in step with the actions. The keys are written in
// Qt's portable form; " / " separates two bindings on one row.
struct ShortcutRow
{
    const char *action;
    const char *keys;
};
// The action names are short commands in a two-column list of keyboard shortcuts.
constexpr ShortcutRow kShortcuts[] = {
    //: Keyboard shortcut list (verb): open a PDF file.
    {QT_TRANSLATE_NOOP("SettingsDialog", "Open"), "Ctrl+O"},
    {QT_TRANSLATE_NOOP("SettingsDialog", "Save edits"), "Ctrl+S"},
    //: Keyboard shortcut list (command): open a new window.
    {QT_TRANSLATE_NOOP("SettingsDialog", "New window"), "Ctrl+N"},
    {QT_TRANSLATE_NOOP("SettingsDialog", "New tab"), "Ctrl+T"},
    {QT_TRANSLATE_NOOP("SettingsDialog", "Close tab"), "Ctrl+W"},
    {QT_TRANSLATE_NOOP("SettingsDialog", "Reopen closed tab"), "Ctrl+Shift+T"},
    //: Keyboard shortcut list: switch to the next or previous tab.
    {QT_TRANSLATE_NOOP("SettingsDialog", "Cycle tabs"), "Ctrl+Tab / Ctrl+Shift+Tab"},
    //: Keyboard shortcut list (verb): search the document's text.
    {QT_TRANSLATE_NOOP("SettingsDialog", "Find"), "Ctrl+F"},
    {QT_TRANSLATE_NOOP("SettingsDialog", "Find next / previous"), "F3 / Shift+F3"},
    {QT_TRANSLATE_NOOP("SettingsDialog", "Previous / next page"), "Ctrl+Up / Ctrl+Down"},
    {QT_TRANSLATE_NOOP("SettingsDialog", "Zoom in / out"), "Ctrl+= / Ctrl+-"},
    //: Keyboard shortcut list: fit the whole page, or the page width, to the window.
    {QT_TRANSLATE_NOOP("SettingsDialog", "Fit page / width"), "Ctrl+1 / Ctrl+2"},
    //: Keyboard shortcut list: switch between fitting the page and the page width.
    {QT_TRANSLATE_NOOP("SettingsDialog", "Toggle fit page / width"), "Home"},
    {QT_TRANSLATE_NOOP("SettingsDialog", "Rotate left / right"), "Ctrl+Shift+L / Ctrl+Shift+R"},
    //: Keyboard shortcut list (verbs): copy the selected text / select all text.
    {QT_TRANSLATE_NOOP("SettingsDialog", "Copy / Select all"), "Ctrl+C / Ctrl+A"},
    //: Keyboard shortcut list: turn the comment tool on or off.
    {QT_TRANSLATE_NOOP("SettingsDialog", "Comment"), "Ctrl+Shift+N"},
    //: Keyboard shortcut list: recognise the text in a selected region.
    //: OCR is optical character recognition.
    {QT_TRANSLATE_NOOP("SettingsDialog", "OCR selection"), "Ctrl+Shift+O"},
    //: Keyboard shortcut list: turn the measuring tool on or off.
    {QT_TRANSLATE_NOOP("SettingsDialog", "Measure"), "Ctrl+Shift+M"},
    //: Keyboard shortcut list: turn the form filling tool on or off.
    {QT_TRANSLATE_NOOP("SettingsDialog", "Fill forms"), "Ctrl+Shift+F"},
    //: Keyboard shortcut list (verb): print the document.
    {QT_TRANSLATE_NOOP("SettingsDialog", "Print"), "Ctrl+P"},
    {QT_TRANSLATE_NOOP("SettingsDialog", "Full screen"), "F11"},
};

// Key names in kShortcuts that Qt's catalog writes unlike the key caps and the OS
// ("Hem" for Home in Swedish). QKeySequence looks key names up in the "QShortcut"
// context, and the build merges Mervin's catalog ahead of Qt's, so these entries
// replace Qt's names. An unfinished entry also hides Qt's name and shows the English
// one, so a language that is happy with Qt's name copies it.
[[maybe_unused]] constexpr const char *kKeyNames[] = {
    //: Key name in the keyboard shortcut list. Write it as printed on the key or as
    //: the OS documents it. Left unfinished, the English name shows.
    QT_TRANSLATE_NOOP("QShortcut", "Home"),
    //: Key name in the keyboard shortcut list: the up arrow key. Write it as the OS
    //: documents it. Left unfinished, the English name shows.
    QT_TRANSLATE_NOOP("QShortcut", "Up"),
    //: Key name in the keyboard shortcut list: the down arrow key. Write it as the OS
    //: documents it. Left unfinished, the English name shows.
    QT_TRANSLATE_NOOP("QShortcut", "Down"),
};

// A row's keys as the OS writes them. Key names follow the UI language through
// Qt's catalog, which the build merges into ours ("Strg" in German), apart from
// the ones kKeyNames overrides.
QString nativeKeys(const char *keys)
{
    QStringList bindings;
    for (const QString &binding : QString::fromLatin1(keys).split(QStringLiteral(" / ")))
        bindings.append(QKeySequence(binding, QKeySequence::PortableText)
                            .toString(QKeySequence::NativeText));
    return bindings.join(QStringLiteral(" / "));
}

// The settings menu is at least this wide; longer page titles widen it.
constexpr int kMinNavWidth = 196;

} // namespace

SettingsDialog::SettingsDialog(const mervin::Settings &current, QWidget *parent)
    : SettingsDialog(current, UpdateInfo{}, Page::General, parent)
{
}

SettingsDialog::SettingsDialog(const mervin::Settings &current, const UpdateInfo &updates,
                               Page page, QWidget *parent)
    : QDialog(parent)
    , base_(current)
    , updates_(updates)
    , accent_(current.accentColor)
    , ocrLanguage_(current.ocrDefaultLanguage)
{
    // "system" (or empty) means follow the OS accent; show it as the swatch.
    const bool useSystemAccent =
        base_.accentColor.isEmpty()
        || base_.accentColor.compare(QLatin1String("system"), Qt::CaseInsensitive) == 0;
    if (useSystemAccent) {
        accent_ = mervin::Theme::systemAccent(mervin::theme::isDark(palette()));
    } else if (!accent_.isValid()) {
        accent_ = mervin::theme::defaultAccent(mervin::theme::isDark(palette()));
    }
    setWindowTitle(tr("Settings"));
    setMinimumSize(680, 420);
    // Keep the dialog within the screen; each page scrolls when its controls need more room.
    const QRect screenArea = screen() ? screen()->availableGeometry() : QRect(0, 0, 1280, 720);
    resize(std::min(820, screenArea.width() - 40), std::min(640, screenArea.height() - 60));

    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);

    auto *body = new QHBoxLayout;
    body->setContentsMargins(0, 0, 0, 0);
    body->setSpacing(0);
    outer->addLayout(body, 1);

    nav_ = new QListWidget(this);
    nav_->setObjectName(QStringLiteral("settingsNav"));
    nav_->setFixedWidth(kMinNavWidth);
    nav_->setIconSize(QSize(16, 16));
    nav_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    nav_->setFrameShape(QFrame::NoFrame);
    body->addWidget(nav_);

    stack_ = new QStackedWidget(this);
    body->addWidget(stack_, 1);

    // The page titles are the rows of the menu on the left of Settings.
    //: Settings page title: general settings.
    addPage(Page::General, tr("General"), buildGeneralPage());
    addPage(Page::Appearance, tr("Appearance"), buildAppearancePage());
    //: Settings page title: how documents are shown (zoom, scrolling).
    addPage(Page::Viewing, tr("Viewing"), buildViewingPage());
    //: Settings page title: highlights and comments added to documents.
    addPage(Page::Annotations, tr("Annotations"), buildAnnotationsPage());
    //: Settings page title. OCR is optical character recognition (text from images).
    addPage(Page::Ocr, tr("OCR"), buildOcrPage());
    //: Settings page title: the measuring tool.
    addPage(Page::Measuring, tr("Measuring"), buildMeasuringPage());
    //: Settings page title: fillable PDF forms.
    addPage(Page::Forms, tr("Forms"), buildFormsPage());

    // A divider between the settings and the two reference pages. It is a
    // disabled row, so arrow keys and clicks skip it.
    auto *divider = new QListWidgetItem(nav_);
    divider->setFlags(Qt::NoItemFlags);
    divider->setSizeHint(QSize(0, 9));
    auto *dividerLine = new QFrame(nav_);
    dividerLine->setObjectName(QStringLiteral("settingsNavDivider"));
    auto *dividerHolder = new QWidget(nav_);
    auto *dividerLayout = new QVBoxLayout(dividerHolder);
    dividerLayout->setContentsMargins(6, 4, 6, 4);
    dividerLine->setFixedHeight(1);
    dividerLayout->addWidget(dividerLine);
    nav_->setItemWidget(divider, dividerHolder);

    addPage(Page::Shortcuts, tr("Keyboard shortcuts"), buildShortcutsPage());
    addPage(Page::About, tr("About"), buildAboutPage());
    refreshNavIcons();
    // Wide enough for the longest page title in the UI language, with its icon
    // and the stylesheet's padding. The menu does not scroll sideways.
    nav_->ensurePolished();
    const QMargins navFrame = nav_->contentsMargins();
    nav_->setFixedWidth(std::max(kMinNavWidth, nav_->sizeHintForColumn(0) + navFrame.left()
                                                   + navFrame.right()));

    connect(nav_, &QListWidget::currentItemChanged, this, [this](QListWidgetItem *item) {
        if (item)
            stack_->setCurrentIndex(item->data(kStackRole).toInt());
    });
    // Enter on a menu row moves into its page instead of pressing OK.
    nav_->installEventFilter(this);

    auto *footer = new QWidget(this);
    footer->setObjectName(QStringLiteral("settingsFooter"));
    auto *footerLayout = new QHBoxLayout(footer);
    footerLayout->setContentsMargins(14, 10, 14, 10);
    auto *buttons = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel | QDialogButtonBox::Apply, footer);
    okButton_ = buttons->button(QDialogButtonBox::Ok);
    applyButton_ = buttons->button(QDialogButtonBox::Apply);
    connect(buttons, &QDialogButtonBox::accepted, this, &SettingsDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    // Apply with a new language closes the dialog like OK: the caller restarts Mervin.
    connect(applyButton_, &QPushButton::clicked, this, [this] {
        if (applyChanges() && restartNeeded())
            QDialog::accept();
    });
    footerLayout->addWidget(buttons);
    outer->addWidget(footer);
    refreshUnloadHint();
    // Compare with the values as the controls show them, so a saved value they
    // normalise (a lower-case accent, a legacy measure type) is not a change.
    applied_ = settings();
    watchForEdits();
    refreshButtons();

    showPage(page);
    nav_->setFocus();
}

void SettingsDialog::addPage(Page page, const QString &title, QWidget *content)
{
    // Every page scrolls on its own, so a long one (About, Keyboard shortcuts)
    // neither stretches the dialog nor squeezes the others.
    alignFormLabels(content);
    auto *scroll = new QScrollArea(stack_);
    scroll->setObjectName(QStringLiteral("settingsPage"));
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setWidgetResizable(true);
    scroll->setFocusPolicy(Qt::NoFocus); // Tab goes straight to the page's controls
    scroll->setWidget(content);
    // setWidget turns on autoFillBackground, which paints the palette's Window
    // colour over the dialog background (visible in light mode).
    content->setAutoFillBackground(false);
    scroll->viewport()->setAutoFillBackground(false);
    const int index = stack_->addWidget(scroll);

    auto *item = new QListWidgetItem(title, nav_);
    item->setData(kPageRole, QVariant::fromValue(page));
    item->setData(kStackRole, index);
}

// Each page is a column of group boxes with a stretch below, so short pages sit
// at the top.
static QVBoxLayout *pageLayout(QWidget *page)
{
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(20, 14, 20, 14);
    layout->setSpacing(12);
    return layout;
}

QWidget *SettingsDialog::buildGeneralPage()
{
    auto *page = new QWidget(this);
    auto *layout = pageLayout(page);

    // The UI language. Windows set their text when they are built, so a new
    // language takes effect when Mervin restarts, which OK and Apply then do.
    //: Group title: the language of Mervin's own text.
    auto *languageBox = groupBox(tr("Language"), page);
    auto *languageForm = snugForm(languageBox);
    languageCombo_ = new mervin::LanguageCombo(languageBox);
    languageCombo_->setObjectName(QStringLiteral("uiLanguage"));
    // The picker starts at the language the next start uses: the stored one. It
    // differs from the one on screen when a restart for it was cancelled at a save
    // prompt, and then the restart note shows too. A --language run starts at the
    // language on screen instead. settings() keeps the stored value until the
    // picker changes.
    const QString storedLanguage =
        mervin::i18n::normalized(base_.uiLanguage, mervin::i18n::availableLanguages());
    languageCombo_->setLanguage(storedLanguage.isEmpty() || mervin::i18n::oneRunOverride()
                                    ? mervin::i18n::current()
                                    : storedLanguage);
    languageAtOpen_ = languageCombo_->language();
    languageForm->addRow(tr("Display language:"), languageCombo_);
    restartHint_ = hintLabel(tr("Mervin will restart."), languageBox);
    restartHint_->setWordWrap(false);
    languageForm->addRow(QString(), restartHint_);
    const auto showRestartHint = [this, languageForm] {
        languageForm->setRowVisible(restartHint_, restartNeeded());
    };
    connect(languageCombo_, &QComboBox::currentIndexChanged, this, showRestartHint);
    showRestartHint();
    layout->addWidget(languageBox);

    auto *openBox = groupBox(tr("Opening files"), page);
    auto *openForm = snugForm(openBox);
    openBehaviorCombo_ = new QComboBox(openBox);
    openBehaviorCombo_->addItem(tr("New tab in current window"), QStringLiteral("new-tab"));
    //: Where a PDF opens: in a new window, not a new tab.
    openBehaviorCombo_->addItem(tr("New window", "where a PDF opens"), QStringLiteral("new-window"));
    selectByData(openBehaviorCombo_, base_.openBehavior);
    openForm->addRow(tr("When opening a PDF:"), openBehaviorCombo_);
    restoreSessionCheck_ = new QCheckBox(tr("Reopen my tabs when Mervin starts"), openBox);
    restoreSessionCheck_->setChecked(base_.restoreSession);
    openForm->addRow(QString(), restoreSessionCheck_);
    layout->addWidget(openBox);

    auto *memoryBox = groupBox(tr("Memory and tray"), page);
    auto *memoryLayout = new QVBoxLayout(memoryBox);
    auto *memoryForm = snugForm(nullptr);
    auto *durationRow = new QWidget(memoryBox);
    auto *durationLayout = new QHBoxLayout(durationRow);
    durationLayout->setContentsMargins(0, 0, 0, 0);
    durationLayout->setSpacing(12);
    unloadInactiveSpin_ = new QSpinBox(durationRow);
    unloadInactiveSpin_->setObjectName(QStringLiteral("unloadInactiveMinutes"));
    unloadInactiveSpin_->setRange(1, mervin::Settings::kMaxUnloadInactiveMinutes);
    unloadInactiveSpin_->setValue(base_.unloadInactiveMinutes > 0 ? base_.unloadInactiveMinutes : 30);
    unloadInactiveSpin_->setAccessibleName(tr("Unload inactive documents after (minutes)"));
    mervin::Theme::useTypedSpinBox(unloadInactiveSpin_);
    //: Checkbox after the minutes field: never unload inactive documents.
    neverUnloadCheck_ = new QCheckBox(tr("Never"), durationRow);
    neverUnloadCheck_->setObjectName(QStringLiteral("neverUnloadDocuments"));
    neverUnloadCheck_->setChecked(base_.unloadInactiveMinutes == 0);
    unloadInactiveSpin_->setEnabled(!neverUnloadCheck_->isChecked());
    durationLayout->addWidget(unloadInactiveSpin_);
    durationLayout->addWidget(neverUnloadCheck_);
    //: Label before a field for a number of minutes, followed by a "Never" checkbox.
    auto *durationLabel = new QLabel(tr("Unload inactive documents after (minutes)"), memoryBox);
    durationLabel->setBuddy(unloadInactiveSpin_);
    memoryForm->addRow(durationLabel, durationRow);
    memoryLayout->addLayout(memoryForm);
    unloadHint_ = hintLabel(QString(), memoryBox);
    unloadHint_->setObjectName(QStringLiteral("unloadHint"));
    memoryLayout->addWidget(unloadHint_);
    memoryLayout->addSpacing(4);
    //: Checkbox: closing the window keeps Mervin running in the system tray instead of quitting.
    closeToTrayCheck_ = new QCheckBox(tr("Close to tray"), memoryBox);
    closeToTrayCheck_->setObjectName(QStringLiteral("closeToTray"));
    closeToTrayCheck_->setChecked(base_.closeToTray);
    memoryLayout->addWidget(closeToTrayCheck_);
    //: Quit Mervin is the tray menu item of that name.
    auto *trayHint = hintLabel(tr("Keep Mervin running when you close the window. "
                                 "Use Quit Mervin from the tray menu to exit."), memoryBox);
    trayHint->setContentsMargins(20, 0, 0, 0);
    memoryLayout->addWidget(trayHint);
    connect(unloadInactiveSpin_->findChild<QLineEdit *>(), &QLineEdit::textChanged, this,
            &SettingsDialog::refreshUnloadHint);
    connect(neverUnloadCheck_, &QCheckBox::toggled, this, [this](bool never) {
        unloadInactiveSpin_->setEnabled(!never);
        refreshUnloadHint();
    });
    connect(closeToTrayCheck_, &QCheckBox::toggled, this, &SettingsDialog::refreshUnloadHint);
    layout->addWidget(memoryBox);

    auto *recentBox = groupBox(tr("Recent files"), page);
    auto *recentForm = snugForm(recentBox);
    // Both counts are typed, not stepped: the useful values are round numbers
    // hundreds apart, so the stepper arrows were only ever visual noise (nudging
    // 500 by one at a time is not a real interaction). Typing, Up/Down and the
    // wheel all still work - see Theme::useTypedSpinBox.
    visibleSpin_ = new QSpinBox(recentBox);
    visibleSpin_->setRange(1, 100000);
    visibleSpin_->setValue(base_.recentVisibleCount);
    mervin::Theme::useTypedSpinBox(visibleSpin_);
    //: Label before a number: how many recent files the Recent page lists.
    recentForm->addRow(tr("Recent files shown:"), visibleSpin_);
    retentionSpin_ = new QSpinBox(recentBox);
    retentionSpin_->setRange(1, 1000000);
    retentionSpin_->setValue(base_.recentRetention);
    mervin::Theme::useTypedSpinBox(retentionSpin_);
    //: Label before a number: how many recent files Mervin remembers.
    recentForm->addRow(tr("Recent history kept:"), retentionSpin_);
    // The scope the Recent page's search starts in; the field's own toggles
    // change it for the moment.
    recentSearchCombo_ = new QComboBox(recentBox);
    recentSearchCombo_->setObjectName(QStringLiteral("recentSearchScope"));
    //: Where the Recent page's search looks first: in file names.
    recentSearchCombo_->addItem(tr("Names"), QStringLiteral("names"));
    //: Where the Recent page's search looks first: in the text inside the files.
    recentSearchCombo_->addItem(tr("Contents"), QStringLiteral("contents"));
    recentSearchCombo_->addItem(tr("All (names, then contents)"), QStringLiteral("all"));
    mervin::fitComboText(*recentSearchCombo_);
    selectByData(recentSearchCombo_, base_.recentSearchScope);
    recentForm->addRow(tr("Default search:"), recentSearchCombo_);
    keepMissingCheck_ = new QCheckBox(tr("Keep removed files in list"), recentBox);
    keepMissingCheck_->setChecked(base_.recentKeepMissing);
    keepMissingCheck_->ensurePolished();
    keepMissingCheck_->setMinimumWidth(keepMissingCheck_->sizeHint().width());
    recentForm->addRow(QString(), keepMissingCheck_);
    layout->addWidget(recentBox);

    // With this setting off Mervin never checks or installs on its own.
    // Check for Updates still downloads, installs and restarts when an update exists.
    auto *updateBox = groupBox(tr("Updates"), page);
    auto *updateLayout = new QVBoxLayout(updateBox);
    //: Settings checkbox. At startup, if 30 days have passed since the last completed
    //: check, download and install any new release automatically, then restart the app.
    updatesCheck_ = new QCheckBox(tr("Update automatically at start (every 30 days)"), updateBox);
    updatesCheck_->setChecked(base_.autoUpdate);
    updateLayout->addWidget(updatesCheck_);
    if (!selfUpdating()) {
        // Portable and dev copies never check on their own (Updater::onStartup),
        // so there is no switch to show. The saved value is kept for an
        // installed copy that shares this profile.
        updatesCheck_->hide();
        //: Check for Updates is the button below.
        updateLayout->addWidget(hintLabel(tr("This copy can't update itself. Check for Updates "
                                             "shows where to download a new version."),
                                          updateBox));
    }
    if (updates_.checkAvailable) {
        auto *checkRow = new QHBoxLayout;
        checkRow->setSpacing(10);
        auto *checkButton = new QPushButton(tr("Check for Updates"), updateBox);
        checkButton->setObjectName(QStringLiteral("checkForUpdatesButton"));
        checkButton->setAutoDefault(false);
        connect(checkButton, &QPushButton::clicked, this, &SettingsDialog::checkForUpdatesRequested);
        lastCheckLabel_ = hintLabel(QString(), updateBox);
        lastCheckLabel_->setWordWrap(false);
        checkRow->addWidget(checkButton);
        checkRow->addWidget(lastCheckLabel_);
        checkRow->addStretch(1);
        updateLayout->addSpacing(4);
        updateLayout->addLayout(checkRow);
        refreshLastCheck();
    }
    layout->addWidget(updateBox);

    // Windows requires the user to confirm default-app changes in system Settings.
#ifdef Q_OS_WIN
    auto *winBox = groupBox(tr("System integration"), page);
    auto *winLayout = new QHBoxLayout(winBox);
    auto *defaultBtn = new QPushButton(tr("Set as Default PDF App"), winBox);
    defaultBtn->setObjectName(QStringLiteral("setDefaultPdfAppButton"));
    defaultBtn->setAutoDefault(false);
    // A --profile (dev/test) instance must not touch the machine's file-type
    // registration - it would point the .pdf handler at the dev executable.
    if (!mervin::ConfigPaths::overrideDir().isEmpty()) {
        defaultBtn->setEnabled(false);
        //: --profile is a command-line option; keep it as it is.
        defaultBtn->setToolTip(tr("Disabled while running with --profile"));
    }
    connect(defaultBtn, &QPushButton::clicked, this, [this] {
        if (mervin::PlatformIntegration::registerPdfHandlerAndPromptDefault())
            return;
        // A confined Snap can't change the association from inside the app; tell
        // the user where to finish it rather than leave the button looking dead.
        QMessageBox::information(
            this, tr("Mervin PDF"),
            tr("Mervin couldn't set itself as your default PDF viewer automatically.\n\n"
               "Open your system's Settings → Default Applications (or right-click a "
               "PDF → Open With) and choose Mervin PDF for PDF files."));
    });
    winLayout->addWidget(defaultBtn);
    winLayout->addStretch(1);
    layout->addWidget(winBox);
#endif

    layout->addStretch(1);
    return page;
}

QWidget *SettingsDialog::buildAppearancePage()
{
    auto *page = new QWidget(this);
    auto *layout = pageLayout(page);

    //: Group title: the look of Mervin's windows, as opposed to the document pages.
    auto *appBox = groupBox(tr("Application"), page);
    auto *appLayout = new QVBoxLayout(appBox);
    // UI theme: the application chrome's light/dark scheme. Dark is the default
    // and sits first. "Follow system" tracks the OS setting, including live
    // auto-switches, and is what a config with an unknown value shows.
    uiThemePicker_ = new mervin::UiThemePicker(appBox);
    uiThemePicker_->setScheme(base_.colorScheme);
    appLayout->addWidget(uiThemePicker_);
    appLayout->addSpacing(4);
    auto *appForm = new QFormLayout;
    appForm->setFieldGrowthPolicy(QFormLayout::FieldsStayAtSizeHint);
    appLayout->addLayout(appForm);

    const bool useSystemAccent =
        base_.accentColor.isEmpty()
        || base_.accentColor.compare(QLatin1String("system"), Qt::CaseInsensitive) == 0;
    auto *accentRow = new QHBoxLayout;
    accentRow->setContentsMargins(0, 0, 0, 0);
    accentRow->setSpacing(8);
    accentBtn_ = new QPushButton(appBox);
    accentBtn_->setFixedSize(48, 24);
    accentBtn_->setAutoDefault(false);
    accentBtn_->setToolTip(tr("Choose accent colour"));
    connect(accentBtn_, &QPushButton::clicked, this, &SettingsDialog::pickAccent);
    //: Button (verb): go back to the default accent colour.
    auto *accentReset = new QPushButton(tr("Reset"), appBox);
    accentReset->setAutoDefault(false);
    connect(accentReset, &QPushButton::clicked, this, [this] {
        accent_ = mervin::theme::defaultAccent(mervin::theme::isDark(palette()));
        updateAccentSwatch();
    });
    accentRow->addWidget(accentBtn_);
    accentRow->addWidget(accentReset);
    accentRow->addStretch();
    appForm->addRow(tr("Accent colour:"), accentRow);

    systemAccentCheck_ = new QCheckBox(tr("Use the system accent colour"), appBox);
    systemAccentCheck_->setChecked(useSystemAccent);
    appForm->addRow(QString(), systemAccentCheck_);
    accentBtn_->setEnabled(!useSystemAccent);
    accentReset->setEnabled(!useSystemAccent);
    connect(systemAccentCheck_, &QCheckBox::toggled, this, [this, accentReset](bool sys) {
        accentBtn_->setEnabled(!sys);
        accentReset->setEnabled(!sys);
        // Preview the OS accent. The palette can't show it while a custom
        // accent is applied, since applyApp() installs that accent in its place.
        if (sys)
            accent_ = mervin::Theme::systemAccent(mervin::theme::isDark(palette()));
        updateAccentSwatch();
    });
    updateAccentSwatch();
    layout->addWidget(appBox);

    // How pages are tinted, independent of the UI light/dark scheme. The
    // toolbar's moon/sun button still flips Traditional and Comfort while reading.
    //: Group title: how document pages are coloured.
    auto *documentBox = groupBox(tr("Document"), page);
    auto *documentLayout = new QVBoxLayout(documentBox);
    docThemePicker_ = new mervin::DocumentThemePicker(documentBox);
    docThemePicker_->setTheme(base_.documentTheme);
    documentLayout->addWidget(docThemePicker_);
    //: Comfort is the name of a document theme above.
    documentLayout->addWidget(hintLabel(tr("Comfort darkens the page and keeps photos readable."),
                                       documentBox));
    layout->addWidget(documentBox);

    layout->addStretch(1);
    return page;
}

QWidget *SettingsDialog::buildViewingPage()
{
    auto *page = new QWidget(this);
    auto *layout = pageLayout(page);
    auto *box = groupBox(QString(), page);
    auto *form = snugForm(box);

    zoomCombo_ = new QComboBox(box);
    //: Default zoom choice: scale the page so its width fits the window.
    zoomCombo_->addItem(tr("Fit Width"), QStringLiteral("fit-width"));
    //: Default zoom choice: scale the page so the whole page fits the window.
    zoomCombo_->addItem(tr("Fit Page"), QStringLiteral("fit-page"));
    for (const char *p : {"50", "75", "100", "125", "150", "200"})
        zoomCombo_->addItem(QStringLiteral("%1%").arg(p), QString::fromLatin1(p));
    selectByData(zoomCombo_, base_.defaultZoom);
    form->addRow(tr("Default zoom:"), zoomCombo_);

    // Scrolling and the spread are separate rows because they are separate
    // choices: a two-page spread can be scrolled continuously or turned one
    // spread at a time, and asking for one must not silently pick the other.
    pageModeCombo_ = new QComboBox(box);
    pageModeCombo_->addItem(tr("Continuous scroll"), QStringLiteral("continuous"));
    pageModeCombo_->addItem(tr("Single page"), QStringLiteral("single"));
    selectByData(pageModeCombo_, base_.pageMode);
    form->addRow(tr("Default scrolling:"), pageModeCombo_);

    twoPageSpreadCheck_ = new QCheckBox(tr("Show pages as two-page spreads"), box);
    twoPageSpreadCheck_->setChecked(base_.twoPageSpread);
    form->addRow(QString(), twoPageSpreadCheck_);

    layout->addWidget(box);
    layout->addStretch(1);
    return page;
}

QWidget *SettingsDialog::buildAnnotationsPage()
{
    auto *page = new QWidget(this);
    auto *layout = pageLayout(page);
    auto *box = groupBox(QString(), page);
    auto *form = snugForm(box);

    // The default colour for new highlights and sticky-note comments. Individual
    // marks are still recoloured from the swatches in their own comment card; this
    // sets the colour each new mark starts from (yellow out of the box).
    annotColor_ = QColor(base_.annotationColor);
    if (!annotColor_.isValid())
        annotColor_ = mervin::annot::defaultColor();
    auto *colorRow = new QHBoxLayout;
    colorRow->setContentsMargins(0, 0, 0, 0);
    colorRow->setSpacing(6);
    for (const QColor &c : mervin::annot::palette()) {
        auto *b = new QToolButton(box);
        b->setFixedSize(22, 22);
        b->setCheckable(true);
        b->setToolTip(c.name());
        connect(b, &QToolButton::clicked, this, [this, c] {
            annotColor_ = c;
            refreshAnnotSwatches();
        });
        annotSwatches_.append(b);
        annotSwatchColors_.append(c);
        colorRow->addWidget(b);
    }
    colorRow->addStretch(1);
    refreshAnnotSwatches();
    auto *colorLabel = new QLabel(tr("Default highlight colour:"), box);
    colorLabel->setToolTip(tr("Colour applied to new highlights and comments"));
    form->addRow(colorLabel, colorRow);

    // Empty means "use the OS user name", resolved when a tab opens; the
    // placeholder shows the name that will be used.
    authorEdit_ = new QLineEdit(box);
    authorEdit_->setObjectName(QStringLiteral("annotationAuthor"));
    authorEdit_->setText(base_.annotationAuthor);
    authorEdit_->setPlaceholderText(osUserName());
    authorEdit_->setMinimumWidth(220);
    form->addRow(tr("Author name:"), authorEdit_);

    layout->addWidget(box);
    layout->addStretch(1);
    return page;
}

QWidget *SettingsDialog::buildOcrPage()
{
    auto *page = new QWidget(this);
    auto *layout = pageLayout(page);

    auto *defaultBox = groupBox(QString(), page);
    auto *defaultForm = snugForm(defaultBox);
    ocrLanguageCombo_ = new QComboBox(defaultBox);
    ocrLanguageCombo_->setObjectName(QStringLiteral("ocrDefaultLanguage"));
    ocrLanguageCombo_->setSizeAdjustPolicy(QComboBox::AdjustToContents);
    ocrLanguageCombo_->setMinimumWidth(200);
    connect(ocrLanguageCombo_, &QComboBox::activated, this, [this](int index) {
        ocrLanguage_ = ocrLanguageCombo_->itemData(index).toString();
        ocrLanguagePicked_ = true;
        refreshButtons();
    });
    //: The language OCR reads text in unless the user picks another.
    defaultForm->addRow(tr("Default language:"), ocrLanguageCombo_);
    layout->addWidget(defaultBox);

    auto *installedBox = groupBox(tr("Installed languages"), page);
    auto *installedLayout = new QVBoxLayout(installedBox);
    installedLayout->setSpacing(8);
    auto *list = new QFrame(installedBox);
    list->setObjectName(QStringLiteral("settingsList"));
    ocrInstalledLayout_ = new QVBoxLayout(list);
    ocrInstalledLayout_->setContentsMargins(2, 2, 2, 2);
    ocrInstalledLayout_->setSpacing(0);
    installedLayout->addWidget(list);

    auto *buttonRow = new QHBoxLayout;
    auto *addButton = new QPushButton(tr("Add Languages..."), installedBox);
    ocrAddButton_ = addButton;
    addButton->setObjectName(QStringLiteral("addOcrLanguagesButton"));
    addButton->setAutoDefault(false);
    connect(addButton, &QPushButton::clicked, this, &SettingsDialog::manageOcrLanguages);
    //: Button: open the folder that holds the OCR language files.
    auto *folderButton = new QPushButton(tr("Open Folder"), installedBox);
    folderButton->setAutoDefault(false);
    connect(folderButton, &QPushButton::clicked, this, [] { mervin::TessdataManager::openFolder(); });
    buttonRow->addWidget(addButton);
    buttonRow->addWidget(folderButton);
    buttonRow->addStretch(1);
    installedLayout->addLayout(buttonRow);
    layout->addWidget(installedBox);

    refreshOcrLanguages();
    layout->addStretch(1);
    return page;
}

QWidget *SettingsDialog::buildMeasuringPage()
{
    auto *page = new QWidget(this);
    auto *layout = pageLayout(page);
    auto *box = groupBox(QString(), page);
    auto *form = snugForm(box);

    //: "&&" shows as a single "&".
    snapCheck_ = new QCheckBox(tr("Snap to vertices && edges"), box);
    snapCheck_->setToolTip(
        tr("Snap endpoints to the drawing's lines for precise picks on CAD geometry"));
    snapCheck_->setChecked(base_.measurementSnap);
    form->addRow(QString(), snapCheck_);

    // The values each new tab's measure panel starts from; the panel can still
    // change them per tab. Choices and spellings match MeasurePanel and the
    // parsers in TabPage (kindFromString) and MeasureMath (unitFromString).
    measureTypeCombo_ = new QComboBox(box);
    //: Measurement type (noun): a straight distance between two points.
    measureTypeCombo_->addItem(tr("Distance"), QStringLiteral("distance"));
    //: Measurement type (noun): the length of a line with several segments.
    measureTypeCombo_->addItem(tr("Path"), QStringLiteral("path"));
    //: Measurement type (noun): the area of a polygon.
    measureTypeCombo_->addItem(tr("Area"), QStringLiteral("area"));
    //: Measurement type (noun): the angle between two lines.
    measureTypeCombo_->addItem(tr("Angle"), QStringLiteral("angle"));
    QString type = base_.measurementType.trimmed().toLower();
    if (type == QLatin1String("polyline"))
        type = QStringLiteral("path");
    selectByData(measureTypeCombo_, type);
    //: The measurement type a new measuring tool starts with.
    form->addRow(tr("Default type:"), measureTypeCombo_);

    measureUnitCombo_ = new QComboBox(box);
    // Unit symbols are not translated (see measure::unitSuffix).
    for (const char *unit : {"mm", "cm", "m", "in", "ft"})
        measureUnitCombo_->addItem(QString::fromLatin1(unit), QString::fromLatin1(unit));
    // TabPage also accepts these long spellings (measure::unitFromString).
    QString unit = base_.measurementUnit.trimmed().toLower();
    if (unit == QLatin1String("inch"))
        unit = QStringLiteral("in");
    else if (unit == QLatin1String("foot") || unit == QLatin1String("feet"))
        unit = QStringLiteral("ft");
    selectByData(measureUnitCombo_, unit);
    form->addRow(tr("Default unit:"), measureUnitCombo_);

    measurePrecisionSpin_ = new QSpinBox(box);
    measurePrecisionSpin_->setRange(0, 4);
    measurePrecisionSpin_->setValue(base_.measurementPrecision);
    mervin::Theme::useTypedSpinBox(measurePrecisionSpin_);
    form->addRow(tr("Decimal places:"), measurePrecisionSpin_);

    measureLineWidthCombo_ = new QComboBox(box);
    int widthIndex = 0;
    double bestDelta = 1e9;
    // "pt" is a unit symbol, so it is not translated.
    for (double w : {0.5, 1.0, 1.5, 2.0, 3.0, 4.0}) {
        measureLineWidthCombo_->addItem(QStringLiteral("%1 pt").arg(w, 0, 'g', 2), w);
        // Land a hand-edited width on the nearest offered one, as MeasurePanel does.
        const double delta = std::abs(w - base_.measurementLineWidth);
        if (delta < bestDelta) {
            bestDelta = delta;
            widthIndex = measureLineWidthCombo_->count() - 1;
        }
    }
    measureLineWidthCombo_->setCurrentIndex(widthIndex);
    form->addRow(tr("Line width:"), measureLineWidthCombo_);

    layout->addWidget(box);
    layout->addStretch(1);
    return page;
}

QWidget *SettingsDialog::buildFormsPage()
{
    auto *page = new QWidget(this);
    auto *layout = pageLayout(page);
    auto *box = groupBox(QString(), page);
    auto *boxLayout = new QVBoxLayout(box);

    autoFormFillCheck_ = new QCheckBox(tr("Open documents with forms in Fill Forms mode"), box);
    autoFormFillCheck_->setToolTip(
        tr("Automatically enter Fill Forms mode when a PDF has fillable fields"));
    autoFormFillCheck_->setChecked(base_.autoFormFill);
    boxLayout->addWidget(autoFormFillCheck_);
    highlightFormFieldsCheck_ = new QCheckBox(tr("Highlight fillable form fields"), box);
    highlightFormFieldsCheck_->setToolTip(
        tr("Tint fillable form fields while the Fill Forms tool is active"));
    highlightFormFieldsCheck_->setChecked(base_.highlightFormFields);
    boxLayout->addWidget(highlightFormFieldsCheck_);

    layout->addWidget(box);
    layout->addStretch(1);
    return page;
}

QWidget *SettingsDialog::buildShortcutsPage()
{
    auto *page = new QWidget(this);
    auto *layout = pageLayout(page);
    layout->setSpacing(0);
    for (const ShortcutRow &r : kShortcuts) {
        auto *row = new QWidget(page);
        row->setObjectName(QStringLiteral("shortcutRow"));
        row->setAttribute(Qt::WA_StyledBackground); // lets the QSS draw its rule
        auto *rowLayout = new QHBoxLayout(row);
        rowLayout->setContentsMargins(0, 6, 0, 6);
        auto *action = new QLabel(tr(r.action), row);
        action->setObjectName(QStringLiteral("shortcutAction"));
        auto *keys = new QLabel(nativeKeys(r.keys), row);
        keys->setObjectName(QStringLiteral("shortcutKeys"));
        rowLayout->addWidget(action);
        rowLayout->addStretch(1);
        rowLayout->addWidget(keys);
        layout->addWidget(row);
    }
    layout->addStretch(1);
    return page;
}

QWidget *SettingsDialog::buildAboutPage()
{
    auto *page = new QWidget(this);
    auto *layout = pageLayout(page);
    layout->setSpacing(10);

    auto *head = new QHBoxLayout;
    head->setSpacing(14);
    auto *icon = new QLabel(page);
    icon->setPixmap(mervin::icons::applicationIcon().pixmap(48, 48));
    auto *names = new QVBoxLayout;
    names->setSpacing(0);
    auto *name = new QLabel(QStringLiteral(MERVIN_APP_NAME), page);
    name->setObjectName(QStringLiteral("aboutName"));
    //: %1 is the version number, such as 1.64.13.
    auto *version = new QLabel(tr("Version %1").arg(QStringLiteral(MERVIN_VERSION_STRING)), page);
    version->setObjectName(QStringLiteral("aboutVersion"));
    version->setTextInteractionFlags(Qt::TextSelectableByMouse);
    names->addWidget(name);
    names->addWidget(version);
    head->addWidget(icon);
    head->addLayout(names);
    head->addStretch(1);
    layout->addLayout(head);

    layout->addWidget(rule(page));

    // Required open-source notices (LGPL for Qt, AGPL/commercial for MuPDF), plus
    // the other direct and bundled components, after Mervin's own licence.
    auto *licenses = new QLabel(
        //: Licence notice. Keep the HTML tags, the component and licence names, and
        //: THIRD_PARTY_LICENSES.md as they are.
        tr("<p>Mervin PDF is free software under the GNU AGPL v3, with no warranty.</p>"
           "<p><b>Open-source components</b></p>"
           "<ul>"
           "<li><b>Qt 6</b>: GNU LGPL v3 (dynamically linked; libraries may be replaced).</li>"
           "<li><b>MuPDF</b> (Artifex): GNU AGPL v3 or commercial. Rendering, text, OCR.</li>"
           "<li><b>qpdf</b>: Apache 2.0. Security &amp; page operations.</li>"
           "<li><b>Tesseract</b>: Apache 2.0, and <b>Leptonica</b>: BSD. Selection OCR.</li>"
           "<li><b>toml++</b>: MIT. Configuration file.</li>"
           "<li>Bundled by MuPDF: FreeType, HarfBuzz, libjpeg-turbo, OpenJPEG, "
           "jbig2dec (AGPL), zlib.</li>"
           "</ul>"
           "<p>See <i>THIRD_PARTY_LICENSES.md</i> for full licence texts.</p>"),
        page);
    licenses->setWordWrap(true);
    licenses->setTextFormat(Qt::RichText);
    layout->addWidget(licenses);

    layout->addStretch(1);
    return page;
}

mervin::Settings SettingsDialog::settings() const
{
    mervin::Settings s = base_; // keep window geometry/state etc.
    // An untouched picker keeps the stored value, even an empty or unknown one.
    if (languageCombo_->language() != languageAtOpen_)
        s.uiLanguage = languageCombo_->language();
    s.defaultZoom = zoomCombo_->currentData().toString();
    s.pageMode = pageModeCombo_->currentData().toString();
    s.twoPageSpread = twoPageSpreadCheck_->isChecked();
    s.documentTheme = docThemePicker_->theme();
    s.colorScheme = uiThemePicker_->scheme();
    s.openBehavior = openBehaviorCombo_->currentData().toString();
    s.restoreSession = restoreSessionCheck_->isChecked();
    // Invalid edits keep the dialog open, so callers only apply a validated duration.
    s.unloadInactiveMinutes = unloadMinutes().value_or(base_.unloadInactiveMinutes);
    s.closeToTray = closeToTrayCheck_->isChecked();
    s.recentVisibleCount = visibleSpin_->value();
    s.recentRetention = retentionSpin_->value();
    s.recentKeepMissing = keepMissingCheck_->isChecked();
    s.recentSearchScope = recentSearchCombo_->currentData().toString();
    s.autoUpdate = updatesCheck_->isChecked();
    s.measurementSnap = snapCheck_->isChecked();
    s.measurementType = measureTypeCombo_->currentData().toString();
    s.measurementUnit = measureUnitCombo_->currentData().toString();
    s.measurementPrecision = measurePrecisionSpin_->value();
    s.measurementLineWidth = measureLineWidthCombo_->currentData().toDouble();
    s.highlightFormFields = highlightFormFieldsCheck_->isChecked();
    s.autoFormFill = autoFormFillCheck_->isChecked();
    s.accentColor = systemAccentCheck_->isChecked()
                        ? QStringLiteral("system")
                        : accent_.name(QColor::HexRgb).toUpper();
    if (annotColor_.isValid())
        s.annotationColor = annotColor_.name(QColor::HexRgb).toUpper();
    s.annotationAuthor = authorEdit_->text().trimmed();
    // The page shows a fallback when the saved language is not installed, but
    // only a choice the user made here is written: the saved one comes back into
    // force as soon as that model is installed again.
    if (ocrLanguagePicked_ && !ocrLanguage_.isEmpty())
        s.ocrDefaultLanguage = ocrLanguage_;
    return s;
}

std::optional<int> SettingsDialog::unloadMinutes() const
{
    if (neverUnloadCheck_->isChecked())
        return 0;
    if (!unloadInactiveSpin_->hasAcceptableInput())
        return std::nullopt;
    bool ok = false;
    const int minutes = unloadInactiveSpin_->locale().toInt(unloadInactiveSpin_->text(), &ok);
    if (ok && minutes > 0 && minutes <= mervin::Settings::kMaxUnloadInactiveMinutes)
        return minutes;
    return std::nullopt;
}

void SettingsDialog::refreshUnloadHint()
{
    const auto minutes = unloadMinutes();
    if (!minutes) {
        //: %1 is the largest number of minutes allowed. Never is the checkbox of that name.
        unloadHint_->setText(tr("Enter 1 to %1 whole minutes, or check Never.")
                                 .arg(mervin::Settings::kMaxUnloadInactiveMinutes));
    } else if (*minutes == 0) {
        unloadHint_->setText(tr("Keep documents loaded, including in the tray."));
    } else {
        unloadHint_->clear(); // a valid timeout needs no explanation
    }
    unloadHint_->setHidden(unloadHint_->text().isEmpty());
}

bool SettingsDialog::restartNeeded() const
{
    return languageCombo_->language() != mervin::i18n::current();
}

void SettingsDialog::accept()
{
    if (applyChanges())
        QDialog::accept();
}

bool SettingsDialog::applyChanges()
{
    if (!unloadMinutes()) {
        showPage(Page::General);
        unloadInactiveSpin_->setFocus();
        unloadInactiveSpin_->selectAll();
        return false;
    }
    const mervin::Settings pending = settings();
    if (pending != applied_) {
        saveError_.reset();
        emit applyRequested(pending);
        if (saveError_) {
            // Still pending, so OK or Apply saves again. No restart either,
            // because the new language would be lost.
            QMessageBox::warning(this, windowTitle(),
                                 tr("Couldn't save the settings.\n\n%1").arg(*saveError_));
            refreshButtons();
            return false;
        }
        applied_ = pending;
    }
    refreshButtons();
    return true;
}

void SettingsDialog::reportSaveFailure(const QString &error)
{
    saveError_ = error;
}

void SettingsDialog::refreshButtons()
{
    if (!applyButton_)
        return; // the pages are still being built
    const bool valid = unloadMinutes().has_value();
    okButton_->setEnabled(valid);
    // A pending language restart also counts: Apply then restarts, as OK does.
    applyButton_->setEnabled(valid && (settings() != applied_ || restartNeeded()));
}

void SettingsDialog::watchForEdits()
{
    // The swatches, accent and OCR language keep their state outside these
    // controls and call refreshButtons themselves.
    for (auto *combo : findChildren<QComboBox *>()) {
        mervin::fitComboText(*combo);
        connect(combo, &QComboBox::currentIndexChanged, this, &SettingsDialog::refreshButtons);
    }
    for (auto *check : findChildren<QCheckBox *>()) {
        check->ensurePolished();
        check->setMinimumWidth(check->sizeHint().width());
    }
    for (auto *button : findChildren<QAbstractButton *>())
        if (button->isCheckable())
            connect(button, &QAbstractButton::toggled, this, &SettingsDialog::refreshButtons);
    for (auto *edit : findChildren<QLineEdit *>()) // including each spin box's editor
        connect(edit, &QLineEdit::textChanged, this, &SettingsDialog::refreshButtons);
}

void SettingsDialog::showPage(Page page)
{
    for (int row = 0; row < nav_->count(); ++row) {
        QListWidgetItem *item = nav_->item(row);
        if ((item->flags() & Qt::ItemIsEnabled) && item->data(kPageRole).value<Page>() == page) {
            nav_->setCurrentRow(row);
            return;
        }
    }
}

SettingsDialog::Page SettingsDialog::currentPage() const
{
    const QListWidgetItem *item = nav_->currentItem();
    return item ? item->data(kPageRole).value<Page>() : Page::General;
}

void SettingsDialog::setAutoUpdate(bool on)
{
    base_.autoUpdate = on;
    applied_.autoUpdate = on; // already saved and in effect
    {
        QSignalBlocker block(updatesCheck_);
        updatesCheck_->setChecked(on);
    }
    refreshButtons();
}

void SettingsDialog::setLastUpdateCheck(const QDateTime &utc)
{
    updates_.lastCheck = utc;
    refreshLastCheck();
}

void SettingsDialog::refreshLastCheck()
{
    if (!lastCheckLabel_)
        return;
    if (!updates_.lastCheck.isValid()) {
        lastCheckLabel_->setText(tr("Never checked"));
        return;
    }
    const QDate day = updates_.lastCheck.toLocalTime().date();
    //: %1 is the date of the last update check, such as "2 October 2026".
    lastCheckLabel_->setText(tr("Last checked %1").arg(longDateWithoutWeekday(day)));
}

bool SettingsDialog::selfUpdating() const
{
    // Without an Updater (tests) the page shows the installed-copy wording.
    return !updates_.checkAvailable || updates_.canSelfUpdate;
}

void SettingsDialog::refreshOcrLanguages(int focusRow)
{
    const QStringList installed = mervin::TessdataManager::installedLanguages();

    // Show an installed model as the default, falling back to the first one as
    // the OCR picker does. settings() writes it only if the user picks it.
    if (!installed.contains(ocrLanguage_))
        ocrLanguage_ = installed.isEmpty() ? QString() : installed.first();
    {
        QSignalBlocker block(ocrLanguageCombo_);
        ocrLanguageCombo_->clear();
        for (const QString &code : installed)
            ocrLanguageCombo_->addItem(mervin::TessdataManager::languageName(code), code);
        if (installed.isEmpty())
            //: Shown in the OCR language picker when no language is installed.
            ocrLanguageCombo_->addItem(tr("None"));
        ocrLanguageCombo_->setEnabled(!installed.isEmpty());
        selectByData(ocrLanguageCombo_, ocrLanguage_);
        mervin::fitComboText(*ocrLanguageCombo_);
    }

    while (QLayoutItem *item = ocrInstalledLayout_->takeAt(0)) {
        delete item->widget();
        delete item;
    }
    const QDir dir(mervin::TessdataManager::directory());
    QList<QToolButton *> removeButtons;
    for (const QString &code : installed) {
        auto *row = new QWidget;
        row->setObjectName(QStringLiteral("settingsListRow"));
        auto *rowLayout = new QHBoxLayout(row);
        rowLayout->setContentsMargins(8, 3, 4, 3);
        rowLayout->setSpacing(10);
        rowLayout->addWidget(new QLabel(mervin::TessdataManager::languageName(code), row));
        rowLayout->addStretch(1);
        const qint64 bytes = QFileInfo(dir.filePath(code + QStringLiteral(".traineddata"))).size();
        auto *meta = new QLabel(QStringLiteral("%1 · %2").arg(code, formattedSize(bytes)), row);
        meta->setObjectName(QStringLiteral("settingsHint"));
        rowLayout->addWidget(meta);
        auto *removeButton = new QToolButton(row);
        removeButton->setObjectName(QStringLiteral("settingsListRemove"));
        removeButton->setAutoRaise(true);
        mervin::icons::setButtonGlyph(removeButton, mervin::icons::Glyph::Delete, 14);
        // Keep one installed model available for OCR.
        if (installed.size() == 1) {
            removeButton->setEnabled(false);
            removeButton->setToolTip(tr("OCR needs at least one language"));
        } else {
            removeButton->setToolTip(
                //: Tooltip of a delete button. %1 is the name of an OCR language, such as Swedish.
                tr("Remove %1").arg(mervin::TessdataManager::languageName(code)));
        }
        connect(removeButton, &QToolButton::clicked, this, [this, code] { removeOcrLanguage(code); });
        rowLayout->addWidget(removeButton);
        ocrInstalledLayout_->addWidget(row);
        removeButtons.append(removeButton);
    }
    if (installed.isEmpty()) {
        auto *empty = hintLabel(tr("No OCR languages installed"), nullptr);
        empty->setContentsMargins(8, 6, 8, 6);
        ocrInstalledLayout_->addWidget(empty);
    }

    // The rows are rebuilt after the buttons below them exist; put them back in
    // reading order: language, the remove buttons, then Add Languages.
    QWidget *previous = ocrLanguageCombo_;
    for (QToolButton *button : removeButtons) {
        QWidget::setTabOrder(previous, button);
        previous = button;
    }
    if (ocrAddButton_)
        QWidget::setTabOrder(previous, ocrAddButton_);

    // After a removal by keyboard, keep focus on the list instead of losing it
    // with the deleted button.
    if (focusRow >= 0) {
        QToolButton *next = nullptr;
        for (int i = std::min(focusRow, int(removeButtons.size()) - 1); i >= 0 && !next; --i)
            if (removeButtons.at(i)->isEnabled())
                next = removeButtons.at(i);
        if (next)
            next->setFocus(Qt::OtherFocusReason);
        else if (ocrAddButton_)
            ocrAddButton_->setFocus(Qt::OtherFocusReason);
    }
    refreshButtons(); // a removed default falls back to another language
}

void SettingsDialog::removeOcrLanguage(const QString &code)
{
    // Removal deletes the model file at once, so Cancel cannot undo it: ask first.
    const QString name = mervin::TessdataManager::languageName(code);
    const auto answer = QMessageBox::question(
        this, tr("Remove OCR Language"),
        //: %1 is the name of an OCR language. Add Languages is the button of that name.
        tr("Remove %1? You can download it again with Add Languages.").arg(name),
        QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel);
    if (answer != QMessageBox::Yes)
        return;
    const int row = int(mervin::TessdataManager::installedLanguages().indexOf(code));
    const QString path =
        QDir(mervin::TessdataManager::directory()).filePath(code + QStringLiteral(".traineddata"));
    if (!QFile::remove(path)) {
        QMessageBox::warning(this, tr("Remove OCR Language"),
                             //: %1 is the name of an OCR language.
                             tr("Could not remove %1 from the OCR language folder.").arg(name));
        return;
    }
    // The saved default is gone with its model, so the fallback becomes a choice.
    if (code == ocrLanguage_)
        ocrLanguagePicked_ = true;
    refreshOcrLanguages(std::max(0, row));
}

void SettingsDialog::manageOcrLanguages()
{
    // The full catalog downloads only from here, so opening Settings never
    // reaches the network.
    mervin::ManageLanguagesDialog manager(ocrLanguage_, this);
    manager.exec();
    const QString chosen = manager.defaultLanguage();
    if (chosen != ocrLanguage_)
        ocrLanguagePicked_ = true;
    ocrLanguage_ = chosen;
    refreshOcrLanguages();
}

bool SettingsDialog::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == nav_ && event->type() == QEvent::KeyPress) {
        const int key = static_cast<QKeyEvent *>(event)->key();
        if (key == Qt::Key_Return || key == Qt::Key_Enter) {
            // The first control of the current page, in focus-chain order.
            QWidget *page = stack_->currentWidget();
            for (QWidget *w = page->nextInFocusChain(); w && w != page; w = w->nextInFocusChain()) {
                if (page->isAncestorOf(w) && w->isEnabled() && w->isVisibleTo(page)
                    && (w->focusPolicy() & Qt::TabFocus)) {
                    w->setFocus(Qt::TabFocusReason);
                    break;
                }
            }
            return true; // never the dialog's default button
        }
    }
    return QDialog::eventFilter(watched, event);
}

void SettingsDialog::changeEvent(QEvent *event)
{
    // A light/dark switch can land while the dialog is open (OS auto-switch runs
    // Theme::applyApp inside this dialog's exec() loop). The app-wide QSS
    // re-skins everything except the inline-styled swatch rings and the menu
    // icons, which are pixmaps - recompute them.
    if (event->type() == QEvent::PaletteChange) {
        refreshAnnotSwatches();
        // The OS accent can differ between schemes (Windows uses another shade).
        if (systemAccentCheck_ && systemAccentCheck_->isChecked())
            accent_ = mervin::Theme::systemAccent(mervin::theme::isDark(palette()));
        updateAccentSwatch(); // the other inline sheet the app-wide QSS can't reach
        refreshNavIcons();
    }
    QDialog::changeEvent(event);
}

void SettingsDialog::refreshNavIcons()
{
    if (!nav_)
        return;
    // Explicit Selected pixmaps: left to itself, QListWidget tints the selected
    // row's icon with the palette highlight.
    const mervin::theme::Chrome t = mervin::theme::chrome(palette());
    for (int row = 0; row < nav_->count(); ++row) {
        QListWidgetItem *item = nav_->item(row);
        if (!(item->flags() & Qt::ItemIsEnabled))
            continue;
        const auto glyph = pageGlyph(item->data(kPageRole).value<Page>());
        QIcon icon;
        for (int px : {16, 20, 24, 32}) {
            icon.addPixmap(mervin::icons::glyphPixmap(glyph, t.inkSoft, px), QIcon::Normal);
            icon.addPixmap(mervin::icons::glyphPixmap(glyph, t.inkPrimary, px), QIcon::Selected);
        }
        item->setIcon(icon);
    }
}

void SettingsDialog::refreshAnnotSwatches()
{
    const bool dark = mervin::theme::isDark(palette());
    for (int i = 0; i < annotSwatches_.size() && i < annotSwatchColors_.size(); ++i) {
        const bool on = annotSwatchColors_[i] == annotColor_;
        annotSwatches_[i]->setChecked(on);
        annotSwatches_[i]->setStyleSheet(
            mervin::theme::swatchStyle(annotSwatchColors_[i], on, dark));
    }
    refreshButtons();
}

void SettingsDialog::pickAccent()
{
    const QColor picked =
        QColorDialog::getColor(accent_, this, tr("Choose accent colour"));
    if (picked.isValid()) {
        accent_ = picked;
        updateAccentSwatch();
    }
}

void SettingsDialog::updateAccentSwatch()
{
    if (!accentBtn_)
        return;
    // Inline sheet so the swatch shows the chosen colour regardless of the
    // app-wide QPushButton styling; a light/dark border keeps it visible on
    // either swatch colour.
    accentBtn_->setStyleSheet(
        QStringLiteral("QPushButton { background:%1; border:1px solid %2;"
                       " border-radius:4px; }")
            .arg(mervin::theme::css(accent_),
                 mervin::theme::css(mervin::theme::doc().colorChipEdge)));
    accentBtn_->setText(QString());
    // A custom accent shows in the theme previews; the system one resolves per scheme.
    uiThemePicker_->setAccent(systemAccentCheck_ && systemAccentCheck_->isChecked() ? QColor()
                                                                                    : accent_);
    refreshButtons();
}
