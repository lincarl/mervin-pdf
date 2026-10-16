#pragma once

#include <QCoreApplication>
#include <QString>

#include <functional>

namespace mervin {

// Wraps qpdf for the document-security operations (Document -> Security). All
// qpdf usage is confined to the .cpp so this header stays dependency-free and
// the class is unit-testable. Every method operates on file paths and never
// modifies the source in place - operations write a new file. Error texts of its
// own are translated; qpdf's messages pass through unchanged.
class QpdfService
{
    Q_DECLARE_TR_FUNCTIONS(mervin::QpdfService)

public:
    enum class Status {
        Ok,
        NeedsPassword, // the file is encrypted with a user password we weren't given
        Failed         // corrupt file, I/O error, etc. (see the error string)
    };

    // Encryption algorithm for encrypt(). AES-256 is the default and the only
    // format still endorsed by the PDF 2.0 spec; the others are offered for
    // compatibility and flagged as weaker in the UI.
    enum class Algorithm {
        AES256, // R6 - recommended
        AES128, // R4 + AES
        RC4_128 // R4 + RC4 (weak; legacy compatibility)
    };

    // Owner-password permission flags. These are advisory in the PDF spec and
    // are NEVER enforced by Mervin's viewer; they only control what the written
    // copy declares.
    struct Permissions
    {
        bool canPrint = true;
        bool canCopy = true;   // extract text / graphics
        bool canModify = true; // assemble + modify content
        bool canAnnotate = true;

        /// Whether no flag restricts anything.
        bool allowsEverything() const { return canPrint && canCopy && canModify && canAnnotate; }
    };

    struct Info
    {
        bool encrypted = false;
        QString algorithm;     // "AES-256" | "AES-128" | "RC4", or a translated "None" | "Unknown"
        int keyLengthBits = 0; // 0 when not encrypted
        int revision = 0;      // R
        // All true when not encrypted. canPrint and canModify are true only when the file
        // allows every kind of printing or modification.
        Permissions permissions;
        bool lowResolutionPrintOnly = false; // printing allowed, at low resolution only
        bool pageAssemblyOnly = false;       // only inserting, rotating or deleting pages allowed
    };

    // Read encryption / permission metadata. `password` may be empty.
    Status readInfo(const QString &path, const QString &password, Info &out, QString *error = nullptr);

    // Write an unencrypted copy (removes the password AND all owner
    // restrictions). `password` opens the source if it has a user password.
    // Optional mayCommit validates the destination under its write lock before publication.
    Status decrypt(const QString &inPath, const QString &outPath, const QString &password,
                   QString *error = nullptr, const std::function<bool()> &mayCommit = {});

    // Write an encrypted copy. `currentPassword` opens the source (empty if the
    // source is unencrypted). `userPassword` is required to open the result
    // (may be empty for owner-only encryption); `ownerPassword` governs
    // permission changes. An empty `ownerPassword` becomes `userPassword` when
    // `perms` allows everything, and otherwise a random password nobody is told,
    // so the open password cannot lift the restrictions. decrypt() still removes
    // them with the open password. AES-128 and RC4-128 store both passwords in
    // PDFDocEncoding and fail without writing when a character has no PDFDocEncoding
    // form; AES-256 stores UTF-8. Use for both "add password" (currentPassword
    // empty) and "change password". Optional mayCommit validates the destination
    // under its write lock before publication.
    Status encrypt(const QString &inPath, const QString &outPath, const QString &currentPassword,
                   const QString &userPassword, const QString &ownerPassword, Algorithm algo,
                   const Permissions &perms, QString *error = nullptr,
                   const std::function<bool()> &mayCommit = {});
};

} // namespace mervin
