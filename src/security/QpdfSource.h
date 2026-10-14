#pragma once

#include <QString>

class QPDF;

namespace mervin {

// Loads `path` into `pdf` as QPDF::processFile() does. qpdf reads objects and stream
// data only when they are used, so most of a source is read while an AtomicPdfWriter
// writes. Each read reports job::Step::Reading for `path` again when the thread
// reported another step since (job::switchTo), so a job waiting for the source names
// that file and not the folder it writes to.
void processReportedFile(QPDF &pdf, const QString &path, const char *password);

} // namespace mervin
