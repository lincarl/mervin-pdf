#pragma once

#include <QDialog>

class QCheckBox;
class QLabel;
class QPushButton;

namespace mervin {

class LanguageCombo;

// The window Mervin shows the first time it starts, before any other window:
// pick the UI language and, on Windows, whether to make Mervin the default PDF
// viewer. Picking a language applies it to the whole app at once, so this
// window switches to it while open. Closing it keeps the language shown.
class FirstRunDialog : public QDialog
{
    Q_OBJECT

public:
    // `offerDefaultApp` adds the default-viewer checkbox (ticked).
    explicit FirstRunDialog(bool offerDefaultApp, QWidget *parent = nullptr);

    // The language the window shows, a catalog ID.
    QString language() const;
    // True when the default-viewer checkbox is shown and ticked.
    bool makeDefaultApp() const;

    LanguageCombo *languageCombo() const { return combo_; }

    // Registers Mervin for PDF files and opens Windows' Default Apps page so the
    // user can confirm. Explains the manual route if that isn't possible.
    static void setAsDefaultPdfApp(QWidget *parent);

protected:
    void changeEvent(QEvent *event) override;

private:
    void retranslate();

    LanguageCombo *combo_ = nullptr;
    QLabel *heading_ = nullptr;
    QLabel *languageLabel_ = nullptr;
    QCheckBox *defaultAppCheck_ = nullptr;
    QLabel *defaultAppHint_ = nullptr;
    QPushButton *continueButton_ = nullptr;
};

} // namespace mervin
