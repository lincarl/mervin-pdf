#include "dialogs/SettingsDialog.h"

#include "render/AnnotTypes.h"
#include "ui/Theme.h"
#include "ui/ThemeTokens.h"

#ifdef Q_OS_WIN
#  include "config/ConfigPaths.h"
#  include "platform/PlatformIntegration.h"
#endif

#include <QCheckBox>
#include <QColorDialog>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QEvent>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPalette>
#include <QPushButton>
#include <QSizePolicy>
#include <QSpinBox>
#include <QToolButton>
#include <QVBoxLayout>

#ifdef Q_OS_WIN
#  include <QMessageBox>
#endif

namespace {

void selectByData(QComboBox *combo, const QString &value)
{
    const int idx = combo->findData(value);
    combo->setCurrentIndex(idx >= 0 ? idx : 0);
}

// A QFormLayout on the Windows/Fusion styles defaults to AllNonFixedFieldsGrow,
// which stretched every combo and spin box across the whole row - several times
// wider than the longest text it can ever hold. FieldsStayAtSizeHint gives each
// field exactly its sizeHint and left-aligns it, with the label column still
// aligned and the label/field buddy (and its mnemonic) intact - which a wrapper
// widget would have broken.
QFormLayout *snugForm(QWidget *parent)
{
    auto *form = new QFormLayout(parent);
    form->setFieldGrowthPolicy(QFormLayout::FieldsStayAtSizeHint);
    return form;
}

} // namespace

SettingsDialog::SettingsDialog(const mervin::Settings &current, QWidget *parent)
    : QDialog(parent)
    , base_(current)
    , accent_(current.accentColor)
{
    // "system" (or empty) means follow the OS accent; show it as the swatch.
    const bool useSystemAccent =
        base_.accentColor.isEmpty()
        || base_.accentColor.compare(QLatin1String("system"), Qt::CaseInsensitive) == 0;
    if (useSystemAccent) {
        accent_ = mervin::Theme::accentColor(QStringLiteral("system"), palette());
    } else if (!accent_.isValid()) {
        accent_ = mervin::theme::defaultAccent(mervin::theme::isDark(palette()));
    }
    setWindowTitle(tr("Settings"));
    setMinimumWidth(420);

    auto *layout = new QVBoxLayout(this);

    // ── Appearance ──────────────────────────────────────────────────────────
    auto *appearanceBox = new QGroupBox(tr("Appearance"), this);
    auto *appearanceForm = snugForm(appearanceBox);

    // UI theme: the application chrome's light/dark scheme. Dark is the default
    // and therefore sits first, so a config with an unknown value lands on it.
    // "Follow system" tracks the OS setting, including live auto-switches.
    uiThemeCombo_ = new QComboBox(appearanceBox);
    uiThemeCombo_->addItem(tr("Dark"), QStringLiteral("dark"));
    uiThemeCombo_->addItem(tr("Light"), QStringLiteral("light"));
    uiThemeCombo_->addItem(tr("Follow system"), QStringLiteral("system"));
    selectByData(uiThemeCombo_, current.colorScheme);
    appearanceForm->addRow(tr("UI theme:"), uiThemeCombo_);

    auto *accentRow = new QHBoxLayout;
    accentRow->setContentsMargins(0, 0, 0, 0);
    accentRow->setSpacing(8);
    accentBtn_ = new QPushButton(this);
    accentBtn_->setFixedSize(48, 24);
    accentBtn_->setToolTip(tr("Choose accent colour"));
    connect(accentBtn_, &QPushButton::clicked, this, &SettingsDialog::pickAccent);
    auto *accentReset = new QPushButton(tr("Reset"), this);
    connect(accentReset, &QPushButton::clicked, this, [this] {
        accent_ = mervin::theme::defaultAccent(mervin::theme::isDark(palette()));
        updateAccentSwatch();
    });
    accentRow->addWidget(accentBtn_);
    accentRow->addWidget(accentReset);
    accentRow->addStretch();
    appearanceForm->addRow(tr("Accent colour:"), accentRow);

    systemAccentCheck_ = new QCheckBox(tr("Use the system accent colour"), this);
    systemAccentCheck_->setChecked(useSystemAccent);
    appearanceForm->addRow(QString(), systemAccentCheck_);
    accentBtn_->setEnabled(!useSystemAccent);
    accentReset->setEnabled(!useSystemAccent);
    connect(systemAccentCheck_, &QCheckBox::toggled, this, [this, accentReset](bool sys) {
        accentBtn_->setEnabled(!sys);
        accentReset->setEnabled(!sys);
        if (sys) {
            QColor s = palette().color(QPalette::Accent);
            if (!s.isValid()) s = palette().color(QPalette::Highlight);
            if (s.isValid()) accent_ = s; // preview the live system accent
        }
        updateAccentSwatch();
    });
    updateAccentSwatch();

    layout->addWidget(appearanceBox);

    auto *viewBox = new QGroupBox(tr("Viewing"), this);
    auto *viewForm = snugForm(viewBox);

    zoomCombo_ = new QComboBox(viewBox);
    zoomCombo_->addItem(tr("Fit Width"), QStringLiteral("fit-width"));
    zoomCombo_->addItem(tr("Fit Page"), QStringLiteral("fit-page"));
    for (const char *p : {"50", "75", "100", "125", "150", "200"})
        zoomCombo_->addItem(QStringLiteral("%1%").arg(p), QString::fromLatin1(p));
    selectByData(zoomCombo_, current.defaultZoom);
    viewForm->addRow(tr("Default zoom:"), zoomCombo_);

    // Scrolling and the spread are separate rows because they are separate
    // choices: a two-page spread can be scrolled continuously or turned one
    // spread at a time, and asking for one must not silently pick the other.
    pageModeCombo_ = new QComboBox(viewBox);
    pageModeCombo_->addItem(tr("Continuous scroll"), QStringLiteral("continuous"));
    pageModeCombo_->addItem(tr("Single page"), QStringLiteral("single"));
    selectByData(pageModeCombo_, current.pageMode);
    viewForm->addRow(tr("Default scrolling:"), pageModeCombo_);

    twoPageSpreadCheck_ = new QCheckBox(tr("Show pages as two-page spreads"), viewBox);
    twoPageSpreadCheck_->setChecked(current.twoPageSpread);
    viewForm->addRow(QString(), twoPageSpreadCheck_);

    // How pages are tinted, independent of the UI light/dark scheme. The
    // toolbar's moon/sun button still flips Traditional <-> Comfort in one click
    // while reading; this row is where the choice (including Inverted) lives.
    docThemeCombo_ = new QComboBox(viewBox);
    docThemeCombo_->addItem(tr("Traditional"), QStringLiteral("light"));
    docThemeCombo_->addItem(tr("Inverted"), QStringLiteral("dark"));
    docThemeCombo_->addItem(tr("Comfort"), QStringLiteral("comfort"));
    // "follow-ui" is a hand-edited config value that is not offered as a choice.
    // Surface it only when it is already in force, so opening Settings and
    // pressing OK cannot silently rewrite it to Traditional.
    if (docThemeCombo_->findData(current.documentTheme) < 0)
        docThemeCombo_->addItem(tr("Follow UI theme"), current.documentTheme);
    docThemeCombo_->setToolTip(
        tr("Traditional shows pages as authored; Inverted flips page colours;"
           " Comfort darkens the page but keeps photos readable"));
    selectByData(docThemeCombo_, current.documentTheme);
    viewForm->addRow(tr("Document theme:"), docThemeCombo_);

    layout->addWidget(viewBox);

    auto *behaviorBox = new QGroupBox(tr("Behaviour"), this);
    auto *behaviorForm = snugForm(behaviorBox);

    openBehaviorCombo_ = new QComboBox(behaviorBox);
    openBehaviorCombo_->addItem(tr("New tab in current window"), QStringLiteral("new-tab"));
    openBehaviorCombo_->addItem(tr("New window"), QStringLiteral("new-window"));
    selectByData(openBehaviorCombo_, current.openBehavior);
    behaviorForm->addRow(tr("When opening a PDF:"), openBehaviorCombo_);

    // Both counts are typed, not stepped: the useful values are round numbers
    // hundreds apart, so the stepper arrows were only ever visual noise (nudging
    // 500 by one at a time is not a real interaction). Typing, Up/Down and the
    // wheel all still work - see Theme::useTypedSpinBox.
    visibleSpin_ = new QSpinBox(behaviorBox);
    visibleSpin_->setRange(1, 100000);
    visibleSpin_->setValue(current.recentVisibleCount);
    mervin::Theme::useTypedSpinBox(visibleSpin_);
    behaviorForm->addRow(tr("Recent files shown:"), visibleSpin_);

    retentionSpin_ = new QSpinBox(behaviorBox);
    retentionSpin_->setRange(1, 1000000);
    retentionSpin_->setValue(current.recentRetention);
    mervin::Theme::useTypedSpinBox(retentionSpin_);
    behaviorForm->addRow(tr("Recent history kept:"), retentionSpin_);

    updatesCheck_ = new QCheckBox(tr("Check for updates on startup"), this);
    updatesCheck_->setChecked(current.checkUpdatesOnStartup);
    behaviorForm->addRow(QString(), updatesCheck_);

    layout->addWidget(behaviorBox);

    // ── Measuring ─────────────────────────────────────────────────────────────
    auto *measuringBox = new QGroupBox(tr("Measuring"), this);
    auto *measuringForm = new QFormLayout(measuringBox);
    snapCheck_ = new QCheckBox(tr("Snap to vertices && edges"), this);
    snapCheck_->setToolTip(
        tr("Snap endpoints to the drawing's lines for precise picks on CAD geometry"));
    snapCheck_->setChecked(current.measurementSnap);
    measuringForm->addRow(QString(), snapCheck_);
    layout->addWidget(measuringBox);

    // ── Forms ─────────────────────────────────────────────────────────────────
    auto *formsBox = new QGroupBox(tr("Forms"), this);
    auto *formsForm = new QFormLayout(formsBox);
    autoFormFillCheck_ = new QCheckBox(tr("Open documents with forms in Fill Forms mode"), this);
    autoFormFillCheck_->setToolTip(
        tr("Automatically enter Fill Forms mode when a PDF has fillable fields"));
    autoFormFillCheck_->setChecked(current.autoFormFill);
    formsForm->addRow(QString(), autoFormFillCheck_);
    highlightFormFieldsCheck_ = new QCheckBox(tr("Highlight fillable form fields"), this);
    highlightFormFieldsCheck_->setToolTip(
        tr("Tint fillable form fields while the Fill Forms tool is active"));
    highlightFormFieldsCheck_->setChecked(current.highlightFormFields);
    formsForm->addRow(QString(), highlightFormFieldsCheck_);
    layout->addWidget(formsBox);

    // ── Annotations ────────────────────────────────────────────────────────────
    // The default colour for new highlights and sticky-note comments. Individual
    // marks are still recoloured from the swatches in their own comment card; this
    // sets the colour each new mark starts from (yellow out of the box).
    auto *annotBox = new QGroupBox(tr("Annotations"), this);
    auto *annotForm = new QFormLayout(annotBox);
    annotColor_ = QColor(current.annotationColor);
    if (!annotColor_.isValid())
        annotColor_ = mervin::annot::defaultColor();
    auto *annotColorRow = new QHBoxLayout;
    annotColorRow->setContentsMargins(0, 0, 0, 0);
    annotColorRow->setSpacing(6);
    for (const QColor &c : mervin::annot::palette()) {
        auto *b = new QToolButton(this);
        b->setFixedSize(22, 22);
        b->setCheckable(true);
        b->setToolTip(c.name());
        b->setStyleSheet(
            mervin::theme::swatchStyle(c, c == annotColor_, mervin::theme::isDark(palette())));
        connect(b, &QToolButton::clicked, this, [this, c] {
            annotColor_ = c;
            refreshAnnotSwatches();
        });
        annotSwatches_.append(b);
        annotSwatchColors_.append(c);
        annotColorRow->addWidget(b);
    }
    annotColorRow->addStretch(1);
    auto *annotColorLabel = new QLabel(tr("Default highlight colour:"), this);
    annotColorLabel->setToolTip(tr("Colour applied to new highlights and comments"));
    annotForm->addRow(annotColorLabel, annotColorRow);
    layout->addWidget(annotBox);

    // Windows requires the user to confirm default-app changes in system Settings.
#ifdef Q_OS_WIN
    auto *winBox = new QGroupBox(tr("System integration"), this);
    auto *winForm = new QFormLayout(winBox);
    auto *defaultBtn = new QPushButton(tr("Set as Default PDF App"), this);
    defaultBtn->setObjectName(QStringLiteral("setDefaultPdfAppButton"));
    // A --profile (dev/test) instance must not touch the machine's file-type
    // registration - it would point the .pdf handler at the dev executable.
    if (!mervin::ConfigPaths::overrideDir().isEmpty()) {
        defaultBtn->setEnabled(false);
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
    winForm->addRow(QString(), defaultBtn);
    layout->addWidget(winBox);
#endif

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);
}

mervin::Settings SettingsDialog::settings() const
{
    mervin::Settings s = base_; // keep window geometry/state etc.
    s.defaultZoom = zoomCombo_->currentData().toString();
    s.pageMode = pageModeCombo_->currentData().toString();
    s.twoPageSpread = twoPageSpreadCheck_->isChecked();
    s.documentTheme = docThemeCombo_->currentData().toString();
    s.colorScheme = uiThemeCombo_->currentData().toString();
    s.openBehavior = openBehaviorCombo_->currentData().toString();
    s.recentVisibleCount = visibleSpin_->value();
    s.recentRetention = retentionSpin_->value();
    s.checkUpdatesOnStartup = updatesCheck_->isChecked();
    s.measurementSnap = snapCheck_->isChecked();
    s.highlightFormFields = highlightFormFieldsCheck_->isChecked();
    s.autoFormFill = autoFormFillCheck_->isChecked();
    s.accentColor = systemAccentCheck_->isChecked()
                        ? QStringLiteral("system")
                        : accent_.name(QColor::HexRgb).toUpper();
    if (annotColor_.isValid())
        s.annotationColor = annotColor_.name(QColor::HexRgb).toUpper();
    return s;
}

void SettingsDialog::changeEvent(QEvent *event)
{
    // A light/dark switch can land while the dialog is open (OS auto-switch runs
    // Theme::applyApp inside this dialog's exec() loop). The app-wide QSS
    // re-skins everything except the inline-styled swatch rings - recompute them.
    if (event->type() == QEvent::PaletteChange) {
        refreshAnnotSwatches();
        updateAccentSwatch(); // the other inline sheet the app-wide QSS can't reach
    }
    QDialog::changeEvent(event);
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
}
