#pragma once

#include "security/QpdfService.h"

#include <QString>

#include <memory>
#include <stdexcept>

class QPDF;

namespace mervin {

// Per Flate decoder, not a process memory budget. This applies before predictor
// processing and does not limit streams encoded with other compression methods.
inline constexpr unsigned long long kMaxQpdfFlateBytes = 64ULL * 1024 * 1024;

/// Retains source I/O failures even when qpdf converts them to warnings and retries.
struct QpdfReadState
{
    QString path;
    QString error;

    /// Rejects publication after a failed source read, including a swallowed exception.
    void throwIfFailed() const;
};

/// A source failure whose path survives output construction and deferred reads.
class QpdfReadError final : public std::runtime_error
{
public:
    QpdfReadError(const QString &path, const QString &message);
    const QString &path() const { return path_; }

private:
    QString path_;
};

/// Gets a structured source filename when qpdf or its input source provided one.
QString qpdfFailurePath(const std::exception &error);

/// Rejects Flate-limit warnings that qpdf otherwise recovers from by dropping objects
/// or copying encoded streams. Consumes recorded warnings, leaving benign repair
/// warnings accepted. Check every source and copied output before publication.
/// With state, the failure remains recorded even if a caller catches the exception.
void checkQpdfWarnings(QPDF &pdf, QpdfReadState *state = nullptr);

// Opens `path` in a new QPDF once job::reading(path) approves, and sets `pdf` on Ok.
// qpdf reads objects and stream data only when they are used, so most of a source is
// read while an AtomicPdfWriter writes. Each read reports job::Step::Reading for `path`
// again when the thread reported another step since (job::switchTo), so a job waiting
// for the source names that file and not the folder it writes to.
// Installs the process-wide Flate decoder limit once. Check subsequent lazy reads
// with checkQpdfWarnings, and retain readState through the precommit callback so
// a failed source read cannot be swallowed by qpdf and published as partial output.
//
// qpdf uses password bytes as given. A password goes first as UTF-8, which AES-256 uses
// and which Mervin also wrote for AES-128 and RC4-128 before it converted passwords.
// When qpdf rejects it, a fresh QPDF tries its PDFDocEncoding, which the PDF format
// requires for RC4 and AES-128 (R2 to R4). A password neither opens gives NeedsPassword
// with QpdfService's message. Other errors give Failed with job::notRunError() or qpdf's
// reason.
QpdfService::Status openQpdf(const QString &path, const QString &password,
                             std::unique_ptr<QPDF> &pdf, QString *error,
                             std::shared_ptr<QpdfReadState> *readState = nullptr);

} // namespace mervin
