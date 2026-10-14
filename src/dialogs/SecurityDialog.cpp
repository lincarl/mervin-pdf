#include "dialogs/SecurityDialog.h"

#include "ui/TextLayout.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPointer>
#include <QPushButton>
#include <QVBoxLayout>

namespace mervin {

namespace {

// A field's frame and padding around its text (Theme: 1px border, 8px padding,
// plus the line edit's own 2px margin, on each side).
constexpr int kFieldPadding = 24;

} // namespace

SecurityDialog::SecurityDialog(const QString &documentPath, const QpdfService::Info &info,
                               const QString &error, std::optional<Request> choices,
                               QWidget *parent)
    : QDialog(parent)
    , path_(documentPath)
    , request_(std::move(choices))
{
    setWindowTitle(tr("Document Security"));
    auto *layout = new QVBoxLayout(this);

    infoLabel_ = new QLabel(this);
    infoLabel_->setTextFormat(Qt::RichText);
    auto *infoBox = new QGroupBox(tr("Current security"), this);
    auto *infoLay = new QVBoxLayout(infoBox);
    infoLay->addWidget(infoLabel_);
    layout->addWidget(infoBox);

    // --- Encrypt / change password -------------------------------------------
    auto *encBox = new QGroupBox(tr("Encrypt / set password"), this);
    auto *form = new QFormLayout(encBox);
    userEdit_ = new QLineEdit(encBox);
    userEdit_->setObjectName(QStringLiteral("securityUserPassword"));
    userEdit_->setEchoMode(QLineEdit::Password);
    //: Placeholder in the open-password field. An empty value means the saved
    //: PDF can be opened without entering a password.
    userEdit_->setPlaceholderText(tr("leave empty for no open password"));
    ownerEdit_ = new QLineEdit(encBox);
    ownerEdit_->setObjectName(QStringLiteral("securityOwnerPassword"));
    ownerEdit_->setEchoMode(QLineEdit::Password);
    //: Placeholder in the owner-password field. If empty, use the open password
    //: to protect the PDF permission settings. The user can enter a different one.
    ownerEdit_->setPlaceholderText(tr("defaults to the open password"));
    // QLineEdit's size hint ignores the placeholder, so a narrow dialog cut it
    // off. Make each field at least as wide as its placeholder.
    for (QLineEdit *edit : {userEdit_, ownerEdit_})
        edit->setMinimumWidth(edit->fontMetrics().horizontalAdvance(edit->placeholderText())
                              + kFieldPadding);
    algoCombo_ = new QComboBox(encBox);
    algoCombo_->setObjectName(QStringLiteral("securityAlgorithm"));
    //: Encryption algorithm choice. Keep the algorithm name as it is.
    algoCombo_->addItem(tr("AES-256 (recommended)"), int(QpdfService::Algorithm::AES256));
    //: Encryption algorithm choice. Keep the algorithm name as it is.
    algoCombo_->addItem(tr("AES-128"), int(QpdfService::Algorithm::AES128));
    //: Encryption algorithm choice. Keep the algorithm name as it is.
    algoCombo_->addItem(tr("RC4-128 (weak)"), int(QpdfService::Algorithm::RC4_128));
    fitComboText(*algoCombo_);
    allowPrint_ = new QCheckBox(tr("Allow printing"), encBox);
    allowCopy_ = new QCheckBox(tr("Allow copying text"), encBox);
    allowModify_ = new QCheckBox(tr("Allow modifying"), encBox);
    allowAnnotate_ = new QCheckBox(tr("Allow annotating"), encBox);
    allowPrint_->setObjectName(QStringLiteral("securityAllowPrint"));
    allowCopy_->setObjectName(QStringLiteral("securityAllowCopy"));
    allowModify_->setObjectName(QStringLiteral("securityAllowModify"));
    allowAnnotate_->setObjectName(QStringLiteral("securityAllowAnnotate"));
    for (QCheckBox *c : {allowPrint_, allowCopy_, allowModify_, allowAnnotate_})
        c->setChecked(true);
    //: The password needed to open the document.
    form->addRow(tr("Open password"), userEdit_);
    //: The password that guards the permission settings below.
    form->addRow(tr("Owner password"), ownerEdit_);
    //: Label for the encryption algorithm picker.
    form->addRow(tr("Algorithm"), algoCombo_);
    form->addRow(allowPrint_);
    form->addRow(allowCopy_);
    form->addRow(allowModify_);
    form->addRow(allowAnnotate_);
    auto *encBtn = new QPushButton(tr("Encrypt and Save As"), encBox);
    encBtn->setObjectName(QStringLiteral("securityEncrypt"));
    connect(encBtn, &QPushButton::clicked, this, &SecurityDialog::doEncrypt);
    form->addRow(encBtn);
    layout->addWidget(encBox);

    // --- Remove encryption ---------------------------------------------------
    auto *remBox = new QGroupBox(tr("Remove encryption"), this);
    auto *remLay = new QVBoxLayout(remBox);
    auto *removeBtn = new QPushButton(tr("Remove Password (Save Decrypted Copy)"), remBox);
    removeBtn->setObjectName(QStringLiteral("securityDecrypt"));
    connect(removeBtn, &QPushButton::clicked, this, [this] { doDecrypt(false); });
    auto *stripBtn = new QPushButton(tr("Strip Owner Restrictions"), remBox);
    stripBtn->setObjectName(QStringLiteral("securityStrip"));
    connect(stripBtn, &QPushButton::clicked, this, [this] { doDecrypt(true); });
    remLay->addWidget(removeBtn);
    remLay->addWidget(stripBtn);
    layout->addWidget(remBox);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);

    if (request_) {
        userEdit_->setText(request_->userPassword);
        ownerEdit_->setText(request_->ownerPassword);
        algoCombo_->setCurrentIndex(algoCombo_->findData(int(request_->algorithm)));
        allowPrint_->setChecked(request_->permissions.canPrint);
        allowCopy_->setChecked(request_->permissions.canCopy);
        allowModify_->setChecked(request_->permissions.canModify);
        allowAnnotate_->setChecked(request_->permissions.canAnnotate);
    }
    showInfo(info, error);
}

void SecurityDialog::showInfo(const QpdfService::Info &info, const QString &error)
{
    if (!error.isEmpty()) {
        //: %1 is the reason.
        infoLabel_->setText(tr("<b>Could not read security information.</b><br>%1")
                                .arg(error.toHtmlEscaped()));
        return;
    }

    auto yn = [this](bool v) {
        //: Permission state, fills %3 to %6 of the encryption summary.
        const QString allowed = tr("Allowed");
        //: Permission state, fills %3 to %6 of the encryption summary.
        const QString notAllowed = tr("Not allowed");
        return v ? allowed : notAllowed;
    };
    if (!info.encrypted) {
        infoLabel_->setText(tr("This document is <b>not encrypted</b>."));
    } else {
        //: %1 is the encryption algorithm, such as AES-256. %2 is the key length in bits.
        //: %3 to %6 are Allowed or Not allowed.
        infoLabel_->setText(tr("<b>Encrypted</b> · %1 (%2-bit)<br>"
                               "Printing: %3 · Copying: %4 · Modifying: %5 · Annotating: %6<br>"
                               "<i>Note: Mervin never enforces these permission flags.</i>")
                                .arg(info.algorithm.toHtmlEscaped())
                                .arg(info.keyLengthBits)
                                .arg(yn(info.permissions.canPrint), yn(info.permissions.canCopy),
                                     yn(info.permissions.canModify), yn(info.permissions.canAnnotate)));
    }
}

QString SecurityDialog::chooseOutput(Action action, const QString &suffix)
{
    const QFileInfo fi(path_);
    const QString suggested = request_ && request_->action == action
        ? request_->output
        : fi.absolutePath() + QLatin1Char('/') + fi.completeBaseName() + suffix + QStringLiteral(".pdf");
    const QPointer<SecurityDialog> self(this);
    const QPointer<QFileDialog> picker(new QFileDialog(this, tr("Save As"), suggested,
                                        //: File type filter. Keep "(*.pdf)" as it is.
                                        tr("PDF documents (*.pdf)")));
    picker->setAcceptMode(QFileDialog::AcceptSave);
    picker->setWindowModality(Qt::WindowModal);
    const bool accepted = picker->exec() == QDialog::Accepted;
    const QStringList files = picker && accepted ? picker->selectedFiles() : QStringList();
    delete picker.data();
    return self && !files.isEmpty() ? files.front() : QString();
}

SecurityDialog::Request SecurityDialog::choices() const
{
    Request result;
    result.userPassword = userEdit_->text();
    result.ownerPassword = ownerEdit_->text();
    result.algorithm = static_cast<QpdfService::Algorithm>(algoCombo_->currentData().toInt());
    result.permissions.canPrint = allowPrint_->isChecked();
    result.permissions.canCopy = allowCopy_->isChecked();
    result.permissions.canModify = allowModify_->isChecked();
    result.permissions.canAnnotate = allowAnnotate_->isChecked();
    return result;
}

void SecurityDialog::doEncrypt()
{
    const QPointer<SecurityDialog> self(this);
    const QString out = chooseOutput(Action::Encrypt, QStringLiteral("-encrypted"));
    if (!self || out.isEmpty())
        return;
    request_ = choices();
    request_->action = Action::Encrypt;
    request_->output = out;
    QDialog::accept();
}

void SecurityDialog::doDecrypt(bool stripRestrictions)
{
    const QPointer<SecurityDialog> self(this);
    if (stripRestrictions) {
        const QPointer<QMessageBox> question(new QMessageBox(QMessageBox::Question,
            tr("Strip Owner Restrictions"),
            tr("This writes a copy with all owner-password restrictions removed "
               "(printing, copying, and editing become permitted).\n\nContinue?"),
            QMessageBox::Yes | QMessageBox::No, this));
        question->setDefaultButton(QMessageBox::No);
        question->setWindowModality(Qt::WindowModal);
        const int answer = question->exec();
        delete question.data();
        if (!self || answer != QMessageBox::Yes)
            return;
    }
    const Action action = stripRestrictions ? Action::StripRestrictions : Action::Decrypt;
    const QString out = chooseOutput(action, stripRestrictions ? QStringLiteral("-unrestricted")
                                                              : QStringLiteral("-decrypted"));
    if (!self || out.isEmpty())
        return;
    request_ = choices();
    request_->action = action;
    request_->output = out;
    QDialog::accept();
}

} // namespace mervin
