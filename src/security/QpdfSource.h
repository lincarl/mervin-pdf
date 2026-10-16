#pragma once

#include "security/QpdfService.h"

#include <QString>

#include <memory>

class QPDF;

namespace mervin {

// Opens `path` in a new QPDF once job::reading(path) approves, and sets `pdf` on Ok.
// qpdf reads objects and stream data only when they are used, so most of a source is
// read while an AtomicPdfWriter writes. Each read reports job::Step::Reading for `path`
// again when the thread reported another step since (job::switchTo), so a job waiting
// for the source names that file and not the folder it writes to.
//
// qpdf uses password bytes as given. A password goes first as UTF-8, which AES-256 uses
// and which Mervin also wrote for AES-128 and RC4-128 before it converted passwords.
// When qpdf rejects it, a fresh QPDF tries its PDFDocEncoding, which the PDF format
// requires for RC4 and AES-128 (R2 to R4). A password neither opens gives NeedsPassword
// with QpdfService's message. Other errors give Failed with job::notRunError() or qpdf's
// reason.
QpdfService::Status openQpdf(const QString &path, const QString &password,
                             std::unique_ptr<QPDF> &pdf, QString *error);

} // namespace mervin
