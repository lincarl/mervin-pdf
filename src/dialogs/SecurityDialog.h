#pragma once

#include "security/QpdfService.h"

#include <QDialog>
#include <QString>

class QLabel;
class QLineEdit;
class QComboBox;
class QCheckBox;

namespace mervin {

// Document -> Security: shows the document's current encryption / permission
// metadata and offers the qpdf-backed operations - encrypt / change password
// (selectable algorithm + permissions), remove password (decrypt), and strip
// owner restrictions. Every operation writes a NEW file (never in place) and
// can offer to open the result via openRequested().
class SecurityDialog : public QDialog
{
    Q_OBJECT

public:
    SecurityDialog(const QString &documentPath, QWidget *parent = nullptr);

signals:
    void openRequested(const QString &path); // user chose to open a written copy

private:
    void refreshInfo();           // read + display current security info
    bool ensurePassword();        // prompt for the open password if required
    void doEncrypt();
    void doDecrypt(bool stripRestrictions); // false = remove password, true = strip
    QString chooseOutput(const QString &suffix);
    void reportResult(QpdfService::Status st, const QString &error, const QString &outPath);

    QString path_;
    QString password_; // the open password supplied so far (may be empty)
    QpdfService svc_;

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
