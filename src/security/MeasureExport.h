#pragma once

#include "render/MeasureContent.h"
#include "security/QpdfService.h"

#include <QByteArray>
#include <QString>

#include <optional>
#include <vector>

namespace mervin {

// qpdf measurement output with atomic replacement. flatten burns vectors into page content;
// embedMervin stores editable JSON in the catalog. Uses QpdfService::Status for password/I/O
// failures; no MuPDF access.
class MeasureExport
{
public:
    using Status = QpdfService::Status;

    static Status flatten(const QString &inPath, const QString &outPath,
                          const std::vector<RenderMeasurement> &marks,
                          const QString &password = QString(), QString *error = nullptr);

    // Writes a copy whose catalog holds `doc` as editable JSON. Fails without writing, with
    // PageOps::measurementsTooLargeError(), when the JSON would pass kMaxMeasurementBlobBytes.
    static Status embedMervin(const QString &inPath, const QString &outPath, const MeasureDoc &doc,
                              const QString &password = QString(), QString *error = nullptr);

    // The Mervin-only measurement blob embedded by embedMervin, read with qpdf, or nullopt
    // when absent / unreadable / password-protected / larger than kMaxMeasurementBlobBytes /
    // encoded other than Mervin writes it (measurementPages::decodeBlob). Opens its own QPDF
    // on the path, so it is independent of any live MuPDF handle. The viewer reads the blob
    // through Document::mervinMeasurements instead; tests use this to check written files.
    static std::optional<QByteArray> readMervinBlob(const QString &inPath,
                                                    const QString &password = QString());
};

} // namespace mervin
