#include "dialogs/FirstRunDialog.h"

#include "i18n/UiLanguage.h"
#include "platform/PlatformIntegration.h"
#include "ui/Icons.h"
#include "ui/LanguageCombo.h"

#include "mervin_version.h"

#include <QCheckBox>
#include <QEvent>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QVBoxLayout>

namespace mervin {

FirstRunDialog::FirstRunDialog(bool offerDefaultApp, QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(QStringLiteral(MERVIN_APP_NAME));
    setMinimumWidth(500);

    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(22, 22, 22, 16);
    outer->setSpacing(20);
    // Fits the text of whichever language is shown, without wrapping.
    outer->setSizeConstraint(QLayout::SetFixedSize);

    auto *top = new QHBoxLayout;
    top->setSpacing(18);
    auto *icon = new QLabel(this);
    icon->setPixmap(icons::applicationIcon().pixmap(48, 48));
    icon->setFixedSize(48, 48);
    top->addWidget(icon, 0, Qt::AlignTop);

    auto *column = new QVBoxLayout;
    column->setSpacing(14);
    heading_ = new QLabel(this);
    heading_->setObjectName(QStringLiteral("firstRunHeading"));
    QFont headingFont = heading_->font();
    headingFont.setPointSizeF(headingFont.pointSizeF() * 1.3);
    headingFont.setWeight(QFont::DemiBold);
    heading_->setFont(headingFont);
    column->addWidget(heading_);

    auto *form = new QFormLayout;
    form->setFieldGrowthPolicy(QFormLayout::FieldsStayAtSizeHint);
    combo_ = new LanguageCombo(this);
    combo_->setObjectName(QStringLiteral("uiLanguage"));
    combo_->setMinimumWidth(250);
    combo_->setLanguage(i18n::current());
    languageLabel_ = new QLabel(this);
    languageLabel_->setBuddy(combo_);
    form->addRow(languageLabel_, combo_);
    column->addLayout(form);

    if (offerDefaultApp) {
        defaultAppCheck_ = new QCheckBox(this);
        defaultAppCheck_->setObjectName(QStringLiteral("makeDefaultPdfApp"));
        defaultAppCheck_->setChecked(true);
        column->addSpacing(4);
        column->addWidget(defaultAppCheck_);
        defaultAppHint_ = new QLabel(this);
        defaultAppHint_->setObjectName(QStringLiteral("settingsHint"));
        defaultAppHint_->setContentsMargins(24, 0, 0, 0);
        column->addWidget(defaultAppHint_);
    }
    top->addLayout(column, 1);
    outer->addLayout(top);

    auto *buttons = new QHBoxLayout;
    buttons->addStretch(1);
    continueButton_ = new QPushButton(this);
    continueButton_->setDefault(true);
    continueButton_->setAutoDefault(true);
    buttons->addWidget(continueButton_);
    outer->addLayout(buttons);

    connect(continueButton_, &QPushButton::clicked, this, &QDialog::accept);
    // The whole app switches language at once; this window follows through
    // LanguageChange (see changeEvent).
    connect(combo_, &LanguageCombo::languagePicked, this, [](const QString &code) {
        if (code != i18n::current())
            i18n::apply(code);
    });

    retranslate();
    combo_->setFocus(Qt::OtherFocusReason);
}

QString FirstRunDialog::language() const
{
    return combo_->language();
}

bool FirstRunDialog::makeDefaultApp() const
{
    return defaultAppCheck_ && defaultAppCheck_->isChecked();
}

void FirstRunDialog::changeEvent(QEvent *event)
{
    if (event->type() == QEvent::LanguageChange)
        retranslate();
    QDialog::changeEvent(event);
}

void FirstRunDialog::retranslate()
{
    heading_->setText(tr("Welcome to Mervin PDF"));
    languageLabel_->setText(tr("Display language:"));
    //: Accessible name of the picker for the language of Mervin's own text.
    combo_->setAccessibleName(tr("Display language"));
    if (defaultAppCheck_) {
        defaultAppCheck_->setText(tr("Make Mervin PDF my default PDF viewer"));
        //: Shown under the default PDF viewer checkbox. Windows Settings is the system app.
        defaultAppHint_->setText(tr("Windows Settings opens so you can confirm."));
    }
    continueButton_->setText(tr("Continue"));
}

void FirstRunDialog::setAsDefaultPdfApp(QWidget *parent)
{
    if (PlatformIntegration::registerPdfHandlerAndPromptDefault())
        return;
    QMessageBox::information(
        parent, QStringLiteral(MERVIN_APP_NAME),
        tr("Mervin couldn't set itself as your default PDF viewer automatically.\n\n"
           "Open your system's Settings → Default Applications (or right-click a "
           "PDF → Open With) and choose Mervin PDF for PDF files."));
}

} // namespace mervin
