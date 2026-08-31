#pragma once

#include <QString>

namespace mervin {

// Wraps qpdf for the document-security operations (Document -> Security). All
// qpdf usage is confined to the .cpp so this header stays dependency-free and
// the class is unit-testable. Every method operates on file paths and never
// modifies the source in place - operations write a new file.
class QpdfService
{
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
    };

    struct Info
    {
        bool encrypted = false;
        QString algorithm;     // "AES-256" | "AES-128" | "RC4" | "None"
        int keyLengthBits = 0; // 0 when not encrypted
        int revision = 0;      // R
        Permissions permissions; // all true when not encrypted
    };

    // Read encryption / permission metadata. `password` may be empty.
    Status readInfo(const QString &path, const QString &password, Info &out, QString *error = nullptr);

    // Write an unencrypted copy (removes the password AND all owner
    // restrictions). `password` opens the source if it has a user password.
    Status decrypt(const QString &inPath, const QString &outPath, const QString &password,
                   QString *error = nullptr);

    // Write an encrypted copy. `currentPassword` opens the source (empty if the
    // source is unencrypted). `userPassword` is required to open the result
    // (may be empty for owner-only encryption); `ownerPassword` governs
    // permission changes (defaults to userPassword if empty). Use for both "add
    // password" (currentPassword empty) and "change password".
    Status encrypt(const QString &inPath, const QString &outPath, const QString &currentPassword,
                   const QString &userPassword, const QString &ownerPassword, Algorithm algo,
                   const Permissions &perms, QString *error = nullptr);
};

} // namespace mervin
