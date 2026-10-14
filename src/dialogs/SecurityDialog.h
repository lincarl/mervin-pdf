#pragma once

#include "security/QpdfService.h"

#include <QDialog>
#include <QString>

#include <optional>

class QLabel;
class QLineEdit;
class QComboBox;
class QCheckBox;

namespace mervin {

// Shows previously read security information and returns a request to write a
// separate copy. The dialog never reads or writes the document itself.
class SecurityDialog : public QDialog
{
    Q_OBJECT

public:
    enum class Action { Encrypt, Decrypt, StripRestrictions };
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
    const std::optional<Request> &request() const { return request_; }

private:
    void showInfo(const QpdfService::Info &info, const QString &error);
    void doEncrypt();
    void doDecrypt(bool stripRestrictions); // false = remove password, true = strip
    QString chooseOutput(Action action, const QString &suffix);
    Request choices() const;

    QString path_;
    std::optional<Request> request_;

    QLabel *infoLabel_ = nullptr;
    QLineEdit *userEdit_ = nullptr;
    QLineEdit *ownerEdit_ = nullptr;
    QComboBox *algoCombo_ = nullptr;
    QCheckBox *allowPrint_ = nullptr;
    QCheckBox *allowCopy_ = nullptr;
    QCheckBox *allowModify_ = nullptr;
    QCheckBox *allowAnnotate_ = nullptr;
};

} // namespace mervin
