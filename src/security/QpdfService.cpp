#include "security/QpdfService.h"
#include "security/AtomicPdfWriter.h"
#include "security/QpdfSource.h"

#include <qpdf/Constants.h>
#include <qpdf/QPDF.hh>
#include <qpdf/QUtil.hh>

#include <QByteArray>
#include <QRandomGenerator>

#include <array>
#include <exception>
#include <memory>
#include <string>

namespace mervin {

namespace {

// 32 random bytes as 64 hex digits, for an owner password nobody is told. R4 uses the
// first 32 bytes of a password, which still hold 128 random bits.
std::string randomOwnerPassword()
{
    std::array<quint32, 8> words;
    QRandomGenerator::system()->fillRange(words.data(), qsizetype(words.size()));
    return QByteArray(reinterpret_cast<const char *>(words.data()), sizeof words)
        .toHex()
        .toStdString();
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
        //: Shown as the encryption algorithm when there is none.
        return QpdfService::tr("None");
    case QPDF::e_unknown:
        break;
    }
    //: Shown as the encryption algorithm when qpdf cannot identify it.
    return QpdfService::tr("Unknown");
}

} // namespace

QpdfService::Status QpdfService::readInfo(const QString &path, const QString &password, Info &out,
                                          QString *error)
{
    std::unique_ptr<QPDF> source;
    const Status st = openQpdf(path, password, source, error);
    if (st != Status::Ok)
        return st;
    QPDF &q = *source;

    try {
        out = Info{};
        out.encrypted = q.isEncrypted();
        if (!out.encrypted) {
            out.algorithm = methodName(QPDF::e_none);
            return Status::Ok;
        }

        int R = 0, P = 0, V = 0;
        QPDF::encryption_method_e streams = QPDF::e_none, strings = QPDF::e_none,
                                  files = QPDF::e_none;
        q.isEncrypted(R, P, V, streams, strings, files);
        out.revision = R;
        // qpdf reads the methods only for V4 and V5. V1 and V2 always use RC4.
        out.algorithm = methodName(V < 4 ? QPDF::e_rc4 : streams);
        out.keyLengthBits = static_cast<int>(q.getEncryptionKey().size()) * 8;

        // qpdf reads the flags whichever password matched. High-resolution printing
        // needs bit 12 from R3 on. Bit 4 permits every modification, page assembly
        // included, and from R3 on bit 11 permits page assembly alone.
        out.permissions.canPrint = q.allowPrintHighRes();
        out.permissions.canCopy = q.allowExtractAll();
        out.permissions.canModify = q.allowModifyOther();
        out.permissions.canAnnotate = q.allowModifyAnnotation();
        out.lowResolutionPrintOnly = !out.permissions.canPrint && q.allowPrintLowRes();
        out.pageAssemblyOnly = !out.permissions.canModify && q.allowModifyAssembly();
        return Status::Ok;
    } catch (const std::exception &e) {
        if (error)
            *error = QString::fromUtf8(e.what());
        return Status::Failed;
    }
}

/// Writes an unencrypted copy after any destination validator approves publication.
QpdfService::Status QpdfService::decrypt(const QString &inPath, const QString &outPath,
                                         const QString &password, QString *error,
                                         const std::function<bool()> &mayCommit)
{
    std::unique_ptr<QPDF> source;
    const Status st = openQpdf(inPath, password, source, error);
    if (st != Status::Ok)
        return st;

    try {
        AtomicPdfWriter output(*source, outPath);
        output.options().setPreserveEncryption(false); // drop encryption AND all owner restrictions
        output.write(mayCommit);
        return Status::Ok;
    } catch (const std::exception &e) {
        if (error)
            *error = QString::fromUtf8(e.what());
        return Status::Failed;
    }
}

/// Writes the chosen encryption and permissions after any destination validator approves.
QpdfService::Status QpdfService::encrypt(const QString &inPath, const QString &outPath,
                                         const QString &currentPassword, const QString &userPassword,
                                         const QString &ownerPassword, Algorithm algo,
                                         const Permissions &perms, QString *error,
                                         const std::function<bool()> &mayCommit)
{
    std::string user = userPassword.toStdString();
    std::string owner = ownerPassword.toStdString();
    // Owner rights ignore the permission flags. Restrictions without an owner password
    // get one that nobody knows, so the open password cannot lift them. With everything
    // allowed the open password stays the owner password, so other programs still let
    // its holder change the security settings.
    if (owner.empty())
        owner = perms.allowsEverything() ? user : randomOwnerPassword();
    // R4 keys use PDFDocEncoding and R6 keys UTF-8, which QString gives. qpdf takes the
    // bytes as given.
    if (algo != Algorithm::AES256) {
        std::string pdfDocUser, pdfDocOwner;
        if (!QUtil::utf8_to_pdf_doc(user, pdfDocUser)
            || !QUtil::utf8_to_pdf_doc(owner, pdfDocOwner)) {
            if (error) {
                //: Reason shown after "The operation failed." when encrypting. AES-128 and
                //: RC4-128 PDFs store passwords in PDFDocEncoding. It has ASCII, Western
                //: European letters such as é and ü, and common symbols, but not, for example,
                //: Polish ą or Greek, Cyrillic or Chinese characters. AES-256 accepts every
                //: character. Keep the algorithm names as they are.
                *error = tr("A password contains characters that AES-128 and RC4-128 cannot "
                            "use. Choose AES-256.");
            }
            return Status::Failed;
        }
        user = std::move(pdfDocUser);
        owner = std::move(pdfDocOwner);
    }

    std::unique_ptr<QPDF> source;
    const Status st = openQpdf(inPath, currentPassword, source, error);
    if (st != Status::Ok)
        return st;
    QPDF &q = *source;

    try {
        const qpdf_r3_print_e print = perms.canPrint ? qpdf_r3p_full : qpdf_r3p_none;
        const bool accessibility = true; // never block accessibility
        const bool extract = perms.canCopy;
        const bool assemble = perms.canModify;
        const bool annotateForm = perms.canAnnotate;
        const bool formFill = perms.canAnnotate;
        const bool modifyOther = perms.canModify;

        AtomicPdfWriter output(q, outPath);
        auto &writer = output.options();
        switch (algo) {
        case Algorithm::AES256:
            writer.setR6EncryptionParameters(user.c_str(), owner.c_str(), accessibility, extract,
                                        assemble, annotateForm, formFill, modifyOther, print,
                                        /*encrypt_metadata_aes=*/true);
            break;
        case Algorithm::AES128:
            writer.setR4EncryptionParametersInsecure(user.c_str(), owner.c_str(), accessibility, extract,
                                                assemble, annotateForm, formFill, modifyOther, print,
                                                /*encrypt_metadata=*/true, /*use_aes=*/true);
            break;
        case Algorithm::RC4_128:
            writer.setR4EncryptionParametersInsecure(user.c_str(), owner.c_str(), accessibility, extract,
                                                assemble, annotateForm, formFill, modifyOther, print,
                                                /*encrypt_metadata=*/true, /*use_aes=*/false);
            break;
        }
        output.write(mayCommit);
        return Status::Ok;
    } catch (const std::exception &e) {
        if (error)
            *error = QString::fromUtf8(e.what());
        return Status::Failed;
    }
}

} // namespace mervin
