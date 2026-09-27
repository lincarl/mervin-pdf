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
    // `password` is the one that opened the document in its tab (empty when it is
    // not encrypted). It is tried first, so the dialog prompts only if it fails.
    SecurityDialog(const QString &documentPath, const QString &password = QString(),
                   QWidget *parent = nullptr);

signals:
    void openRequested(const QString &path); // user chose to open a written copy

private:
    void refreshInfo();           // read + display current security info
    bool ensurePassword();        // prompt for the open password; only after password_ failed
    void doEncrypt();
    void doDecrypt(bool stripRestrictions); // false = remove password, true = strip
    QString chooseOutput(const QString &suffix);
    void reportResult(QpdfService::Status st, const QString &error, const QString &outPath);

    QString path_;
    QString password_; // the open password supplied so far (the tab's, or typed; may be empty)
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
