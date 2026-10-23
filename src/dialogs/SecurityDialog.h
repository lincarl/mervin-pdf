#pragma once

#include "security/QpdfService.h"

#include <QDialog>
#include <QString>

#include <optional>

class QLabel;
class QLineEdit;
class QComboBox;
class QCheckBox;
class QFormLayout;

namespace mervin {

// Shows previously read security information and returns a request to write a
// separate copy. The dialog never reads or writes the document itself.
class SecurityDialog : public QDialog
{
    Q_OBJECT

public:
    enum class Action { Encrypt, Decrypt, StripRestrictions };
    /// Captures the chosen action, destination, passwords and permission settings.
    struct Request
    {
        Action action = Action::Encrypt;
        QString output;
        QString userPassword;
        QString ownerPassword;
        QpdfService::Algorithm algorithm = QpdfService::Algorithm::AES256;
        QpdfService::Permissions permissions;
    };

    // A failed write supplies its request to preserve choices and the suggested
    // destination. `error` describes an unsuccessful information read.
    SecurityDialog(const QString &documentPath, const QpdfService::Info &info,
                   const QString &error = {}, std::optional<Request> choices = std::nullopt,
                   QWidget *parent = nullptr);
    /// Returns the captured write request for use after the dialog is accepted.
    const std::optional<Request> &request() const { return request_; }

private:
    /// Displays the supplied security facts or escaped information read error.
    void showInfo(const QpdfService::Info &info, const QString &error);
    /// Chooses an output and accepts an encryption request without writing a file.
    /// Refuses below the owner field, before the picker, an owner password keyed with the
    /// same bytes as the open password (QpdfService::keyBytes) while a permission is
    /// restricted. Passwords that differ only after the keyed part get a reason that names
    /// the algorithm's limit. Refuses unencodable passwords before opening the picker.
    void doEncrypt();
    /// Confirms restriction removal when needed and accepts a decrypted-copy request.
    void doDecrypt(bool stripRestrictions); // false = remove password, true = strip
    /// Uses a save picker with owner checks and the destination retained for this action.
    QString chooseOutput(Action action, const QString &suffix);
    /// Captures the current passwords, encryption algorithm and permissions.
    Request choices() const;
    /// Shows `text` below the owner password, or hides that line when it is empty.
    void setError(const QString &text);

    QString path_;
    std::optional<Request> request_;

    QLabel *infoLabel_ = nullptr;
    QFormLayout *encryptForm_ = nullptr;
    QLabel *error_ = nullptr;
    int heightBeforeError_ = 0;
    QLineEdit *userEdit_ = nullptr;
    QLineEdit *ownerEdit_ = nullptr;
    QComboBox *algoCombo_ = nullptr;
    QCheckBox *allowPrint_ = nullptr;
    QCheckBox *allowCopy_ = nullptr;
    QCheckBox *allowModify_ = nullptr;
    QCheckBox *allowAnnotate_ = nullptr;
};

} // namespace mervin
