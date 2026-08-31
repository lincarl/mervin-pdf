#include "security/QpdfService.h"

#include <qpdf/Constants.h>
#include <qpdf/QPDF.hh>
#include <qpdf/QPDFExc.hh>
#include <qpdf/QPDFWriter.hh>

#include <exception>
#include <string>

namespace mervin {

namespace {

std::string u8(const QString &s)
{
    const QByteArray b = s.toUtf8();
    return std::string(b.constData(), static_cast<size_t>(b.size()));
}

// Open a source file, mapping qpdf's password error to NeedsPassword.
QpdfService::Status open(QPDF &q, const QString &path, const QString &password, QString *error)
{
    try {
        const std::string pw = u8(password);
        q.processFile(u8(path).c_str(), password.isEmpty() ? nullptr : pw.c_str());
        return QpdfService::Status::Ok;
    } catch (const QPDFExc &e) {
        if (e.getErrorCode() == qpdf_e_password) {
            if (error)
                *error = QStringLiteral("A password is required to open this document.");
            return QpdfService::Status::NeedsPassword;
        }
        if (error)
            *error = QString::fromUtf8(e.what());
        return QpdfService::Status::Failed;
    } catch (const std::exception &e) {
        if (error)
            *error = QString::fromUtf8(e.what());
        return QpdfService::Status::Failed;
    }
}

QString methodName(QPDF::encryption_method_e m)
{
    switch (m) {
    case QPDF::e_aesv3:
        return QStringLiteral("AES-256");
    case QPDF::e_aes:
        return QStringLiteral("AES-128");
    case QPDF::e_rc4:
        return QStringLiteral("RC4");
    case QPDF::e_none:
        return QStringLiteral("None");
    case QPDF::e_unknown:
        break;
    }
    return QStringLiteral("Unknown");
}

} // namespace

QpdfService::Status QpdfService::readInfo(const QString &path, const QString &password, Info &out,
                                          QString *error)
{
    QPDF q;
    const Status st = open(q, path, password, error);
    if (st != Status::Ok)
        return st;

    try {
        out = Info{};
        out.encrypted = q.isEncrypted();
        if (!out.encrypted) {
            out.algorithm = QStringLiteral("None");
            return Status::Ok;
        }

        int R = 0, P = 0, V = 0;
        QPDF::encryption_method_e sm = QPDF::e_none, strm = QPDF::e_none, fm = QPDF::e_none;
        q.isEncrypted(R, P, V, sm, strm, fm);
        out.revision = R;
        out.algorithm = methodName(fm);
        out.keyLengthBits = static_cast<int>(q.getEncryptionKey().size()) * 8;

        out.permissions.canPrint = q.allowPrintLowRes();
        out.permissions.canCopy = q.allowExtractAll();
        out.permissions.canModify = q.allowModifyOther() || q.allowModifyAssembly();
        out.permissions.canAnnotate = q.allowModifyAnnotation();
        return Status::Ok;
    } catch (const std::exception &e) {
        if (error)
            *error = QString::fromUtf8(e.what());
        return Status::Failed;
    }
}

QpdfService::Status QpdfService::decrypt(const QString &inPath, const QString &outPath,
                                         const QString &password, QString *error)
{
    QPDF q;
    const Status st = open(q, inPath, password, error);
    if (st != Status::Ok)
        return st;

    try {
        QPDFWriter w(q, u8(outPath).c_str());
        w.setStaticID(false);
        w.setPreserveEncryption(false); // drop encryption AND all owner restrictions
        w.write();
        return Status::Ok;
    } catch (const std::exception &e) {
        if (error)
            *error = QString::fromUtf8(e.what());
        return Status::Failed;
    }
}

QpdfService::Status QpdfService::encrypt(const QString &inPath, const QString &outPath,
                                         const QString &currentPassword, const QString &userPassword,
                                         const QString &ownerPassword, Algorithm algo,
                                         const Permissions &perms, QString *error)
{
    QPDF q;
    const Status st = open(q, inPath, currentPassword, error);
    if (st != Status::Ok)
        return st;

    try {
        const std::string user = u8(userPassword);
        const std::string owner = u8(ownerPassword.isEmpty() ? userPassword : ownerPassword);
        const qpdf_r3_print_e print = perms.canPrint ? qpdf_r3p_full : qpdf_r3p_none;
        const bool accessibility = true; // never block accessibility
        const bool extract = perms.canCopy;
        const bool assemble = perms.canModify;
        const bool annotateForm = perms.canAnnotate;
        const bool formFill = perms.canAnnotate;
        const bool modifyOther = perms.canModify;

        QPDFWriter w(q, u8(outPath).c_str());
        w.setStaticID(false);
        switch (algo) {
        case Algorithm::AES256:
            w.setR6EncryptionParameters(user.c_str(), owner.c_str(), accessibility, extract,
                                        assemble, annotateForm, formFill, modifyOther, print,
                                        /*encrypt_metadata_aes=*/true);
            break;
        case Algorithm::AES128:
            w.setR4EncryptionParametersInsecure(user.c_str(), owner.c_str(), accessibility, extract,
                                                assemble, annotateForm, formFill, modifyOther, print,
                                                /*encrypt_metadata=*/true, /*use_aes=*/true);
            break;
        case Algorithm::RC4_128:
            w.setR4EncryptionParametersInsecure(user.c_str(), owner.c_str(), accessibility, extract,
                                                assemble, annotateForm, formFill, modifyOther, print,
                                                /*encrypt_metadata=*/true, /*use_aes=*/false);
            break;
        }
        w.write();
        return Status::Ok;
    } catch (const std::exception &e) {
        if (error)
            *error = QString::fromUtf8(e.what());
        return Status::Failed;
    }
}

} // namespace mervin
