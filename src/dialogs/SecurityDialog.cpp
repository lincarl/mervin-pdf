#include "dialogs/SecurityDialog.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QVBoxLayout>

namespace mervin {

namespace {

// A field's frame and padding around its text (Theme: 1px border, 8px padding,
// plus the line edit's own 2px margin, on each side).
constexpr int kFieldPadding = 24;

} // namespace

SecurityDialog::SecurityDialog(const QString &documentPath, const QString &password,
                               QWidget *parent)
    : QDialog(parent)
    , path_(documentPath)
    , password_(password)
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
    userEdit_->setEchoMode(QLineEdit::Password);
    userEdit_->setPlaceholderText(tr("leave empty for no open password"));
    ownerEdit_ = new QLineEdit(encBox);
    ownerEdit_->setEchoMode(QLineEdit::Password);
    ownerEdit_->setPlaceholderText(tr("defaults to the open password"));
    // QLineEdit's size hint ignores the placeholder, so a narrow dialog cut it
    // off. Make each field at least as wide as its placeholder.
    for (QLineEdit *edit : {userEdit_, ownerEdit_})
        edit->setMinimumWidth(edit->fontMetrics().horizontalAdvance(edit->placeholderText())
                              + kFieldPadding);
    algoCombo_ = new QComboBox(encBox);
    //: Encryption algorithm choice. Keep the algorithm name as it is.
    algoCombo_->addItem(tr("AES-256 (recommended)"), int(QpdfService::Algorithm::AES256));
    //: Encryption algorithm choice. Keep the algorithm name as it is.
    algoCombo_->addItem(tr("AES-128"), int(QpdfService::Algorithm::AES128));
    //: Encryption algorithm choice. Keep the algorithm name as it is.
    algoCombo_->addItem(tr("RC4-128 (weak)"), int(QpdfService::Algorithm::RC4_128));
    allowPrint_ = new QCheckBox(tr("Allow printing"), encBox);
    allowCopy_ = new QCheckBox(tr("Allow copying text"), encBox);
    allowModify_ = new QCheckBox(tr("Allow modifying"), encBox);
    allowAnnotate_ = new QCheckBox(tr("Allow annotating"), encBox);
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
    connect(encBtn, &QPushButton::clicked, this, &SecurityDialog::doEncrypt);
    form->addRow(encBtn);
    layout->addWidget(encBox);

    // --- Remove encryption ---------------------------------------------------
    auto *remBox = new QGroupBox(tr("Remove encryption"), this);
    auto *remLay = new QVBoxLayout(remBox);
    auto *removeBtn = new QPushButton(tr("Remove Password (Save Decrypted Copy)"), remBox);
    connect(removeBtn, &QPushButton::clicked, this, [this] { doDecrypt(false); });
    auto *stripBtn = new QPushButton(tr("Strip Owner Restrictions"), remBox);
    connect(stripBtn, &QPushButton::clicked, this, [this] { doDecrypt(true); });
    remLay->addWidget(removeBtn);
    remLay->addWidget(stripBtn);
    layout->addWidget(remBox);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);

    refreshInfo();
}

void SecurityDialog::refreshInfo()
{
    QpdfService::Info info;
    QString err;
    QpdfService::Status st = svc_.readInfo(path_, password_, info, &err);
    if (st == QpdfService::Status::NeedsPassword) {
        if (ensurePassword())
            st = svc_.readInfo(path_, password_, info, &err);
    }

    if (st != QpdfService::Status::Ok) {
        //: %1 is the reason.
        infoLabel_->setText(tr("<b>Could not read security information.</b><br>%1")
                                .arg(st == QpdfService::Status::NeedsPassword
                                         ? tr("A password is required.")
                                         : err.toHtmlEscaped()));
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
                                .arg(info.algorithm)
                                .arg(info.keyLengthBits)
                                .arg(yn(info.permissions.canPrint), yn(info.permissions.canCopy),
                                     yn(info.permissions.canModify), yn(info.permissions.canAnnotate)));
    }
}

bool SecurityDialog::ensurePassword()
{
    bool ok = false;
    const QString pw = QInputDialog::getText(this, tr("Password Required"),
                                             tr("Enter the password for this document:"),
                                             QLineEdit::Password, QString(), &ok);
    if (!ok)
        return false;
    password_ = pw;
    return true;
}

QString SecurityDialog::chooseOutput(const QString &suffix)
{
    const QFileInfo fi(path_);
    const QString suggested =
        fi.absolutePath() + QLatin1Char('/') + fi.completeBaseName() + suffix + QStringLiteral(".pdf");
    return QFileDialog::getSaveFileName(this, tr("Save As"), suggested,
                                        //: File type filter. Keep "(*.pdf)" as it is.
                                        tr("PDF documents (*.pdf)"));
}

void SecurityDialog::doEncrypt()
{
    const QString out = chooseOutput(QStringLiteral("-encrypted"));
    if (out.isEmpty())
        return;
    QpdfService::Permissions perms;
    perms.canPrint = allowPrint_->isChecked();
    perms.canCopy = allowCopy_->isChecked();
    perms.canModify = allowModify_->isChecked();
    perms.canAnnotate = allowAnnotate_->isChecked();
    const auto algo = static_cast<QpdfService::Algorithm>(algoCombo_->currentData().toInt());
    QString err;
    const auto st = svc_.encrypt(path_, out, password_, userEdit_->text(), ownerEdit_->text(),
                                 algo, perms, &err);
    if (st == QpdfService::Status::NeedsPassword && ensurePassword()) {
        const auto st2 = svc_.encrypt(path_, out, password_, userEdit_->text(), ownerEdit_->text(),
                                      algo, perms, &err);
        reportResult(st2, err, out);
        return;
    }
    reportResult(st, err, out);
}

void SecurityDialog::doDecrypt(bool stripRestrictions)
{
    if (stripRestrictions) {
        const auto choice = QMessageBox::question(
            this, tr("Strip Owner Restrictions"),
            tr("This writes a copy with all owner-password restrictions removed "
               "(printing, copying, and editing become permitted).\n\nContinue?"),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
        if (choice != QMessageBox::Yes)
            return;
    }
    const QString out = chooseOutput(stripRestrictions ? QStringLiteral("-unrestricted")
                                                       : QStringLiteral("-decrypted"));
    if (out.isEmpty())
        return;
    QString err;
    auto st = svc_.decrypt(path_, out, password_, &err);
    if (st == QpdfService::Status::NeedsPassword && ensurePassword())
        st = svc_.decrypt(path_, out, password_, &err);
    reportResult(st, err, out);
}

void SecurityDialog::reportResult(QpdfService::Status st, const QString &error, const QString &outPath)
{
    if (st != QpdfService::Status::Ok) {
        // Separate statements: lupdate before Qt 6.11 drops a //: comment placed in
        // front of the ':' branch of a ternary.
        QString message;
        if (st == QpdfService::Status::NeedsPassword) {
            message = tr("The correct password is required.");
        } else {
            //: %1 is the reason.
            message = tr("The operation failed.\n\n%1").arg(error);
        }
        QMessageBox::warning(this, tr("Operation failed"), message);
        return;
    }
    refreshInfo();
    const auto open = QMessageBox::information(
        //: Message box title after a copy was saved.
        this, tr("Done"),
        //: %1 is the path of the saved copy.
        tr("Saved to:\n%1\n\nOpen it now?").arg(QDir::toNativeSeparators(outPath)),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes);
    if (open == QMessageBox::Yes)
        emit openRequested(outPath);
}

} // namespace mervin
