#pragma once

#include "config/Settings.h"

#include <QColor>
#include <QDialog>
#include <QList>

class QComboBox;
class QCheckBox;
class QPushButton;
class QSpinBox;
class QToolButton;

// Edits application defaults. Window geometry/state are preserved unchanged.
class SettingsDialog : public QDialog
{
    Q_OBJECT

public:
    explicit SettingsDialog(const mervin::Settings &current, QWidget *parent = nullptr);

    mervin::Settings settings() const;

protected:
    void changeEvent(QEvent *event) override; // re-skin swatch rings on light/dark switch

private:
    void pickAccent();         // open the colour dialog
    void updateAccentSwatch(); // repaint the swatch button to the current accent
    void refreshAnnotSwatches(); // re-check the default-highlight swatch row

    mervin::Settings base_; // preserves fields not exposed in the UI
    QColor accent_;         // current accent choice ("#RRGGBB")
    QColor annotColor_;     // current default highlight/comment colour

    QComboBox *uiThemeCombo_ = nullptr; // chrome light/dark scheme
    QComboBox *zoomCombo_ = nullptr;
    QComboBox *pageModeCombo_ = nullptr;
    QCheckBox *twoPageSpreadCheck_ = nullptr;
    QComboBox *docThemeCombo_ = nullptr; // page tint: Traditional / Inverted / Comfort
    QComboBox *openBehaviorCombo_ = nullptr;
    QCheckBox *updatesCheck_ = nullptr;
    QCheckBox *snapCheck_ = nullptr;
    QCheckBox *highlightFormFieldsCheck_ = nullptr;
    QCheckBox *autoFormFillCheck_ = nullptr;
    QSpinBox *visibleSpin_ = nullptr;
    QSpinBox *retentionSpin_ = nullptr;
    QPushButton *accentBtn_ = nullptr;
    QCheckBox *systemAccentCheck_ = nullptr;
    QList<QToolButton *> annotSwatches_; // default-highlight colour chips
    QList<QColor> annotSwatchColors_;    // the palette, in swatch order
};
