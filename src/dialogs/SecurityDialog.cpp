#include "dialogs/SecurityDialog.h"

#include "dialogs/LayoutMinimum.h"
#include "ui/TextLayout.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontMetrics>
#include <QFormLayout>
#include <QGroupBox>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPointer>
#include <QPushButton>
#include <QTimer>
#include <QVBoxLayout>

#include <utility>

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
    //: Placeholder in the owner-password field while every permission below is
    //: allowed. If empty, the open password also becomes the owner password. The
    //: user can enter a different one.
    const QString ownerIsOpen = tr("defaults to the open password");
    //: Placeholder in the owner-password field while a permission below is not
    //: allowed. If empty, Mervin protects the permission settings with a random
    //: owner password that nobody is told. The user can enter one instead.
    const QString ownerIsRandom = tr("defaults to a random password");
    // QLineEdit's size hint ignores the placeholder, so a narrow dialog cut it
    // off. Make each field at least as wide as its longest placeholder.
    const QFontMetrics metrics = userEdit_->fontMetrics();
    userEdit_->setMinimumWidth(metrics.horizontalAdvance(userEdit_->placeholderText())
                               + kFieldPadding);
    ownerEdit_->setMinimumWidth(qMax(metrics.horizontalAdvance(ownerIsOpen),
                                     metrics.horizontalAdvance(ownerIsRandom))
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
    // A refused owner password shows its reason below the field to change.
    error_ = new QLabel(encBox);
    error_->setObjectName(QStringLiteral("securityError"));
    error_->setWordWrap(true);
    form->addRow(QString(), error_);
    encryptForm_ = form;
    setError({});
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

    // Spare height gathers above Close, keeping the fields in place during resizing.
    layout->addStretch();
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
    // The placeholder says what an empty owner password becomes, and a refusal
    // lasts until the next change to the choices it concerns.
    const auto choicesChanged = [this, ownerIsOpen, ownerIsRandom] {
        // The constructor's parameter hides choices().
        const bool allowed = this->choices().permissions.allowsEverything();
        ownerEdit_->setPlaceholderText(allowed ? ownerIsOpen : ownerIsRandom);
        setError({});
    };
    for (QLineEdit *edit : {userEdit_, ownerEdit_})
        connect(edit, &QLineEdit::textChanged, this, choicesChanged);
    for (QCheckBox *c : {allowPrint_, allowCopy_, allowModify_, allowAnnotate_})
        connect(c, &QCheckBox::toggled, this, choicesChanged);
    // The algorithm decides how much of each password counts.
    connect(algoCombo_, &QComboBox::currentIndexChanged, this, choicesChanged);
    choicesChanged();
    showInfo(info, error);
    // The layout alone sets the minimum, which grows while a refusal shows.
    followLayoutMinimum(*this, QSize());
}

void SecurityDialog::setError(const QString &text)
{
    const bool wasHidden = error_->isHidden();
    if (!text.isEmpty() && wasHidden && isVisible())
        heightBeforeError_ = height();
    error_->setText(text);
    encryptForm_->setRowVisible(error_, !text.isEmpty());
    if (text.isEmpty() && !wasHidden && heightBeforeError_ > 0) {
        const int previousHeight = std::exchange(heightBeforeError_, 0);
        // Qt updates the form's minimum asynchronously after hiding its error row.
        QTimer::singleShot(0, this, [this, previousHeight] {
            if (!error_->isHidden())
                return;
            encryptForm_->activate();
            layout()->activate();
            setMinimumHeight(minimumSizeHint().height());
            resize(width(), qMin(height(), previousHeight));
        });
    }
}

void SecurityDialog::showInfo(const QpdfService::Info &info, const QString &error)
{
    if (!error.isEmpty()) {
        //: %1 is the reason.
        infoLabel_->setText(tr("<b>Could not read security information.</b><br>%1")
                                .arg(error.toHtmlEscaped()));
        return;
    }

    //: Permission state, fills %3 to %6 of the encryption summary.
    const QString allowed = tr("Allowed");
    //: Permission state, fills %3 to %6 of the encryption summary.
    const QString notAllowed = tr("Not allowed");
    // A limited state replaces Not allowed when the file allows part of the action.
    const auto state = [&](bool full, bool limited = false, const QString &limitedText = {}) {
        return full ? allowed : limited ? limitedText : notAllowed;
    };
    if (!info.encrypted) {
        infoLabel_->setText(tr("This document is <b>not encrypted</b>."));
    } else {
        //: Printing state, fills %3 of the encryption summary. The PDF allows
        //: printing, but only in low resolution, possibly of degraded quality.
        const QString lowResolution = tr("Low resolution only");
        //: Modifying state, fills %5 of the encryption summary. The PDF allows only
        //: page assembly, such as inserting, rotating or deleting pages, and no other
        //: changes to its content.
        const QString pageAssembly = tr("Page assembly only");
        //: %1 is the encryption algorithm, such as AES-256. %2 is the key length in bits.
        //: %3 is Allowed, Low resolution only or Not allowed. %5 is Allowed, Page
        //: assembly only or Not allowed. %4 and %6 are Allowed or Not allowed.
        infoLabel_->setText(tr("<b>Encrypted</b> · %1 (%2-bit)<br>"
                               "Printing: %3 · Copying: %4 · Modifying: %5 · Annotating: %6<br>"
                               "<i>Note: Mervin never enforces these permission flags.</i>")
                                .arg(info.algorithm.toHtmlEscaped())
                                .arg(info.keyLengthBits)
                                .arg(state(info.permissions.canPrint, info.lowResolutionPrintOnly,
                                           lowResolution),
                                     state(info.permissions.canCopy),
                                     state(info.permissions.canModify, info.pageAssemblyOnly,
                                           pageAssembly),
                                     state(info.permissions.canAnnotate)));
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
    // The worker captures the destination before the app asks to replace it.
    // A native confirmation here would approve contents that have not been checked.
    picker->setOption(QFileDialog::DontConfirmOverwrite);
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
    const Request chosen = choices();
    // An owner password keyed with the same bytes as the open password gives anyone with
    // the open password owner rights, which ignore the restrictions. AES-128 and RC4-128
    // key only the first 32 characters, so longer passwords can match without being equal.
    const auto userKey = QpdfService::keyBytes(chosen.userPassword, chosen.algorithm);
    const auto ownerKey = QpdfService::keyBytes(chosen.ownerPassword, chosen.algorithm);
    if (!userKey || !ownerKey) {
        setError(QpdfService::passwordEncodingError());
        QLineEdit *invalid = userKey ? ownerEdit_ : userEdit_;
        invalid->setFocus();
        invalid->selectAll();
        return;
    }
    if (!chosen.permissions.allowsEverything() && !chosen.ownerPassword.isEmpty()
        && userKey == ownerKey) {
        if (chosen.ownerPassword == chosen.userPassword) {
            //: Shown below the owner-password field when it is the same as the open
            //: password while a permission is not allowed. Anyone with the open password
            //: could then lift the restrictions. An empty owner field is also accepted.
            setError(tr("Use a different owner password to restrict permissions."));
        } else if (chosen.algorithm == QpdfService::Algorithm::AES256) {
            //: Shown below the owner-password field when it differs from the open password
            //: only after their first 127 bytes, while a permission is not allowed and
            //: AES-256 is chosen. Anyone with the open password could then lift the
            //: restrictions. A byte is a unit of stored text. Letters without accents,
            //: digits and common punctuation take one byte each, accented letters two, and
            //: most other scripts three or four. Keep the algorithm name and the number.
            setError(tr("AES-256 uses only the first 127 bytes of a password. Make the two "
                        "passwords differ within them."));
        } else {
            //: Shown below the owner-password field when it differs from the open password
            //: only after their first 32 characters, while a permission is not allowed and
            //: AES-128 or RC4-128 is chosen. Anyone with the open password could then lift
            //: the restrictions. Keep the algorithm names and the number.
            setError(tr("AES-128 and RC4-128 use only the first 32 characters of a password. "
                        "Make the two passwords differ within them."));
        }
        ownerEdit_->setFocus();
        ownerEdit_->selectAll();
        return;
    }
    const QPointer<SecurityDialog> self(this);
    const QString out = chooseOutput(Action::Encrypt, QStringLiteral("-encrypted"));
    if (!self || out.isEmpty())
        return;
    request_ = chosen;
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
