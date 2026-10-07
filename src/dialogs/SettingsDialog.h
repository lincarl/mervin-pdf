#pragma once

#include "config/Settings.h"

#include <QColor>
#include <QDateTime>
#include <QDialog>
#include <QList>

#include <optional>

class QComboBox;
class QCheckBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
class QSpinBox;
class QStackedWidget;
class QToolButton;
class QVBoxLayout;

namespace mervin {
class DocumentThemePicker;
class LanguageCombo;
class UiThemePicker;
}

// Edits application defaults and shows About, one page at a time behind a menu on
// the left. Window geometry/state are preserved unchanged. Apply and OK hand the
// pending changes to MainWindow through applyRequested; Apply keeps the dialog
// open and is enabled only while a control differs from the last applied value.
// Cancel discards changes made since the last Apply. Actions on files act at once:
// removing or adding OCR languages, and Set as Default PDF App.
class SettingsDialog : public QDialog
{
    Q_OBJECT

public:
    // The pages, in menu order.
    enum class Page { General, Appearance, Viewing, Annotations, Ocr, Measuring, Forms, Shortcuts, About };
    Q_ENUM(Page)

    // What the General page shows about updates. The dialog does not talk to the
    // Updater itself: it emits checkForUpdatesRequested and is told the result.
    struct UpdateInfo
    {
        bool checkAvailable = false; // an Updater exists, so Check for Updates is offered
        bool canSelfUpdate = false;  // this copy downloads and installs updates itself
        QDateTime lastCheck;         // last check with a definite answer (UTC); invalid = never
    };

    explicit SettingsDialog(const mervin::Settings &current, QWidget *parent = nullptr);
    SettingsDialog(const mervin::Settings &current, const UpdateInfo &updates, Page page,
                   QWidget *parent = nullptr);

    mervin::Settings settings() const;

    // The language picked differs from the one Mervin is showing. It takes effect at
    // startup, so OK or Apply then closes the dialog and the caller restarts Mervin.
    bool restartNeeded() const;

    void showPage(Page page);
    Page currentPage() const;

public slots:
    void accept() override;

    // Another window changed automatic updates while this dialog is open.
    // Keeps OK from writing the old value back.
    void setAutoUpdate(bool on);
    void setLastUpdateCheck(const QDateTime &utc);
    // The applyRequested receiver couldn't save the settings. OK and Apply then
    // show `error` and keep the dialog open, so no restart happens either.
    void reportSaveFailure(const QString &error);

signals:
    void checkForUpdatesRequested();
    // Apply, or OK with changes pending: save `settings` and put them into effect
    // now. Later edits are compared with these values. A receiver that can't save
    // calls reportSaveFailure before returning and leaves everything as it was.
    void applyRequested(const mervin::Settings &settings);

protected:
    void changeEvent(QEvent *event) override; // re-skin swatches and menu icons on light/dark switch
    bool eventFilter(QObject *watched, QEvent *event) override; // Enter on the menu

private:
    QWidget *buildGeneralPage();
    QWidget *buildAppearancePage();
    QWidget *buildViewingPage();
    QWidget *buildAnnotationsPage();
    QWidget *buildOcrPage();
    QWidget *buildMeasuringPage();
    QWidget *buildFormsPage();
    QWidget *buildShortcutsPage();
    QWidget *buildAboutPage();
    void addPage(Page page, const QString &title, QWidget *content);
    void watchForEdits();        // any control change refreshes the buttons
    // Hands pending changes to applyRequested. False, with the interval focused,
    // while the unload interval is invalid.
    bool applyChanges();
    void refreshButtons();       // OK needs valid input; Apply also needs a change

    void pickAccent();           // open the colour dialog
    void updateAccentSwatch();   // repaint the swatch button to the current accent
    void refreshAnnotSwatches(); // re-check the default-highlight swatch row
    void refreshNavIcons();      // tint the menu icons for the current palette
    void refreshLastCheck();     // "Last checked ..." under Check for Updates
    std::optional<int> unloadMinutes() const; // validates the numeric duration unless Never is checked
    void refreshUnloadHint();
    // Re-read the installed OCR models; focusRow moves focus to that row's remove
    // button (or the nearest one left) after a removal.
    void refreshOcrLanguages(int focusRow = -1);
    bool selfUpdating() const;   // this copy checks for and installs updates itself
    void removeOcrLanguage(const QString &code);
    void manageOcrLanguages();   // the full catalog: Manage OCR Languages

    mervin::Settings base_; // preserves fields not exposed in the UI
    mervin::Settings applied_; // settings() as last applied, or as opened
    std::optional<QString> saveError_; // set by reportSaveFailure during applyRequested
    UpdateInfo updates_;
    QColor accent_;         // current accent choice ("#RRGGBB")
    QColor annotColor_;     // current default highlight/comment colour
    QString ocrLanguage_;   // default OCR language shown (Tesseract code); empty if none installed
    bool ocrLanguagePicked_ = false; // the user chose ocrLanguage_ here, so settings() saves it

    QListWidget *nav_ = nullptr;
    QStackedWidget *stack_ = nullptr;

    mervin::LanguageCombo *languageCombo_ = nullptr;
    QString languageAtOpen_;      // the language the picker showed when the dialog opened
    QLabel *restartHint_ = nullptr; // "Mervin will restart." while restartNeeded()
    mervin::UiThemePicker *uiThemePicker_ = nullptr; // chrome light/dark scheme
    QComboBox *zoomCombo_ = nullptr;
    QComboBox *pageModeCombo_ = nullptr;
    QCheckBox *twoPageSpreadCheck_ = nullptr;
    mervin::DocumentThemePicker *docThemePicker_ = nullptr;
    QComboBox *openBehaviorCombo_ = nullptr;
    QCheckBox *restoreSessionCheck_ = nullptr;
    QSpinBox *unloadInactiveSpin_ = nullptr;
    QCheckBox *neverUnloadCheck_ = nullptr;
    QLabel *unloadHint_ = nullptr;
    QCheckBox *closeToTrayCheck_ = nullptr;
    QPushButton *okButton_ = nullptr;
    QPushButton *applyButton_ = nullptr;
    QSpinBox *visibleSpin_ = nullptr;
    QSpinBox *retentionSpin_ = nullptr;
    QComboBox *recentSearchCombo_ = nullptr;
    QCheckBox *keepMissingCheck_ = nullptr;
    QCheckBox *updatesCheck_ = nullptr;
    QLabel *lastCheckLabel_ = nullptr;
    QPushButton *accentBtn_ = nullptr;
    QCheckBox *systemAccentCheck_ = nullptr;
    QList<QToolButton *> annotSwatches_; // default-highlight colour chips
    QList<QColor> annotSwatchColors_;    // the palette, in swatch order
    QLineEdit *authorEdit_ = nullptr;
    QComboBox *ocrLanguageCombo_ = nullptr;
    QVBoxLayout *ocrInstalledLayout_ = nullptr; // one row per installed model
    QPushButton *ocrAddButton_ = nullptr;
    QCheckBox *snapCheck_ = nullptr;
    QComboBox *measureTypeCombo_ = nullptr;
    QComboBox *measureUnitCombo_ = nullptr;
    QSpinBox *measurePrecisionSpin_ = nullptr;
    QComboBox *measureLineWidthCombo_ = nullptr;
    QCheckBox *highlightFormFieldsCheck_ = nullptr;
    QCheckBox *autoFormFillCheck_ = nullptr;
};
