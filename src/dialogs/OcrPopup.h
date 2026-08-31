#pragma once

#include <QDialog>
#include <QString>
#include <QStringList>

class QCheckBox;
class QComboBox;
class QPlainTextEdit;

namespace mervin {

// Review/edit popup for Selection OCR. Shows the recognized text (editable,
// because OCR is never perfect), a language picker that re-runs OCR when changed,
// and Preserve-line-breaks / Auto-trim post-processing toggles. The caller
// performs the actual recognition and feeds results in via setRawText().
class OcrPopup : public QDialog
{
    Q_OBJECT

public:
    explicit OcrPopup(const QStringList &installedLanguages,
                      const QString &preferredLanguage = QString(),
                      QWidget *parent = nullptr);

    // Set the raw OCR output; the display re-applies the current toggles.
    void setRawText(const QString &raw);

    // The single Tesseract language code selected in the picker.
    QStringList selectedLanguages() const;

    // Repopulate the picker after the language manager closes. The preferred
    // language is selected when available; otherwise the existing selection or
    // the first installed language is retained. A changed effective selection
    // requests OCR immediately, just like a user change in the combo box.
    void refreshLanguages(const QStringList &installedLanguages,
                          const QString &preferredLanguage = QString());

signals:
    void recognizeRequested(const QStringList &languages);
    void manageLanguagesRequested();

private:
    void renderDisplay();
    void copyText();
    void populateLanguages(const QStringList &installedLanguages,
                           const QString &preferredLanguage, bool requestRecognition);

    QComboBox *langCombo_ = nullptr;
    QPlainTextEdit *edit_ = nullptr;
    QCheckBox *preserveBreaks_ = nullptr;
    QCheckBox *autoTrim_ = nullptr;
    QString rawText_;
};

} // namespace mervin
