#include "dialogs/OcrPopup.h"

#include "ocr/TessdataManager.h"

#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSignalBlocker>
#include <QVBoxLayout>

namespace mervin {

OcrPopup::OcrPopup(const QStringList &installedLanguages, const QString &preferredLanguage,
                   QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(tr("OCR Selection"));
    resize(520, 420);
    auto *layout = new QVBoxLayout(this);

    // Language row.
    auto *langRow = new QHBoxLayout;
    langRow->addWidget(new QLabel(tr("Language"), this));
    langCombo_ = new QComboBox(this);
    langCombo_->setObjectName(QStringLiteral("ocrLanguageCombo"));
    langCombo_->setEditable(false);
    populateLanguages(installedLanguages, preferredLanguage, false);
    connect(langCombo_, &QComboBox::currentIndexChanged, this, [this](int) {
        if (!selectedLanguages().isEmpty())
            emit recognizeRequested(selectedLanguages());
    });
    langRow->addWidget(langCombo_, 1);
    auto *manageBtn = new QPushButton(tr("Manage Languages"), this);
    connect(manageBtn, &QPushButton::clicked, this, &OcrPopup::manageLanguagesRequested);
    langRow->addWidget(manageBtn);
    layout->addLayout(langRow);

    edit_ = new QPlainTextEdit(this);
    edit_->setPlaceholderText(tr("Recognised text will appear here; you can edit it before copying."));
    layout->addWidget(edit_, 1);

    auto *optRow = new QHBoxLayout;
    preserveBreaks_ = new QCheckBox(tr("Preserve line breaks"), this);
    preserveBreaks_->setChecked(true);
    autoTrim_ = new QCheckBox(tr("Auto-trim"), this);
    autoTrim_->setChecked(true);
    connect(preserveBreaks_, &QCheckBox::toggled, this, &OcrPopup::renderDisplay);
    connect(autoTrim_, &QCheckBox::toggled, this, &OcrPopup::renderDisplay);
    optRow->addWidget(preserveBreaks_);
    optRow->addWidget(autoTrim_);
    optRow->addStretch(1);
    layout->addLayout(optRow);

    auto *buttons = new QDialogButtonBox(this);
    auto *copyBtn = buttons->addButton(tr("Copy"), QDialogButtonBox::AcceptRole);
    buttons->addButton(QDialogButtonBox::Close);
    connect(copyBtn, &QPushButton::clicked, this, &OcrPopup::copyText);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);
}

QStringList OcrPopup::selectedLanguages() const
{
    const QString code = langCombo_->currentData().toString();
    return code.isEmpty() ? QStringList{} : QStringList{code};
}

void OcrPopup::refreshLanguages(const QStringList &installedLanguages,
                                const QString &preferredLanguage)
{
    populateLanguages(installedLanguages, preferredLanguage, true);
}

void OcrPopup::populateLanguages(const QStringList &installedLanguages,
                                 const QString &preferredLanguage, bool requestRecognition)
{
    const QString previous = langCombo_->currentData().toString();
    const QSignalBlocker blocker(langCombo_);
    langCombo_->clear();
    for (const QString &code : installedLanguages)
        langCombo_->addItem(TessdataManager::languageName(code), code);

    QString target = preferredLanguage;
    if (target.isEmpty() || !installedLanguages.contains(target))
        target = installedLanguages.contains(previous) ? previous : QString();
    const int targetIndex = target.isEmpty() ? (langCombo_->count() ? 0 : -1)
                                             : langCombo_->findData(target);
    langCombo_->setCurrentIndex(targetIndex);
    langCombo_->setEnabled(langCombo_->count() > 0);

    const QString current = langCombo_->currentData().toString();
    if (requestRecognition && !current.isEmpty() && current != previous)
        emit recognizeRequested(QStringList{current});
}

void OcrPopup::setRawText(const QString &raw)
{
    rawText_ = raw;
    renderDisplay();
}

void OcrPopup::renderDisplay()
{
    QString t = rawText_;
    if (!preserveBreaks_->isChecked())
        t.replace(QLatin1Char('\n'), QLatin1Char(' '));
    if (autoTrim_->isChecked())
        t = t.trimmed();
    edit_->setPlainText(t);
}

void OcrPopup::copyText()
{
    QApplication::clipboard()->setText(edit_->toPlainText());
}

} // namespace mervin
