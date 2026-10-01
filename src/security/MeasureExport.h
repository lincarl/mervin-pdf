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

    static Status embedMervin(const QString &inPath, const QString &outPath, const MeasureDoc &doc,
                              const QString &password = QString(), QString *error = nullptr);

    // The Mervin-only measurement blob embedded by embedMervin, or nullopt when
    // absent / unreadable / password-protected. Opens its own QPDF on the path,
    // so it is independent of any live MuPDF handle and safe on the UI thread.
    static std::optional<QByteArray> readMervinBlob(const QString &inPath,
                                                    const QString &password = QString());
};

} // namespace mervin
