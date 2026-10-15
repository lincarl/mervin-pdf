#include "io/DestinationConsent.h"

#include "io/JobContext.h"
#include "recent/PathKey.h"

#include <QCryptographicHash>
#include <QFile>
#include <QFileInfo>

#include <filesystem>

namespace mervin {

/// Captures exact contents with bounded memory and checks Stop between filesystem reads.
std::optional<DestinationConsent> DestinationConsent::read(const QString &path, QString *error)
{
    if (error)
        error->clear();
    const auto fail = [error](const QString &message) -> std::optional<DestinationConsent> {
        if (error)
            *error = message;
        return std::nullopt;
    };
    if (!job::reading(path))
        return fail(job::notRunError());
    const QFileInfo info(path);
    const auto native = info.filesystemFilePath();
    std::error_code statusError;
    const auto status = std::filesystem::status(native, statusError);
    if (job::stopped())
        return fail(job::notRunError());
    DestinationConsent consent;
    // Only a missing directory entry grants absence. Permission errors, failed
    // reads and dangling links must never authorize replacement as a new file.
    if (status.type() == std::filesystem::file_type::not_found) {
        std::error_code linkError;
        const auto link = std::filesystem::symlink_status(native, linkError);
        if (job::stopped())
            return fail(job::notRunError());
        if (link.type() != std::filesystem::file_type::not_found
            || (linkError && linkError != std::errc::no_such_file_or_directory))
            return fail(QString::fromStdString(
                (linkError ? linkError : std::make_error_code(std::errc::no_such_file_or_directory))
                    .message()));
        consent.key = fileKey(info);
        if (job::stopped())
            return fail(job::notRunError());
        return consent;
    }
    if (statusError)
        return fail(QString::fromStdString(statusError.message()));
    if (!std::filesystem::is_regular_file(status))
        return fail(QString::fromStdString(std::make_error_code(std::errc::invalid_argument).message()));

    consent.key = fileKey(info);
    if (job::stopped())
        return fail(job::notRunError());
    consent.exists = true;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return fail(file.errorString());
    if (job::stopped())
        return fail(job::notRunError());
    QCryptographicHash hash(QCryptographicHash::Sha256);
    QByteArray buffer(1024 * 1024, Qt::Uninitialized);
    for (;;) {
        if (!job::reading(path))
            return fail(job::notRunError());
        const qint64 count = file.read(buffer.data(), buffer.size());
        if (count < 0)
            return fail(file.errorString());
        if (count == 0)
            break;
        hash.addData(QByteArrayView(buffer.constData(), count));
        job::progressed();
    }
    if (job::stopped())
        return fail(job::notRunError());
    consent.sha256 = hash.result();
    return consent;
}

} // namespace mervin
