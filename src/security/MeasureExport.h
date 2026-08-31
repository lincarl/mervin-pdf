#pragma once

#include "render/MeasureContent.h"
#include "security/QpdfService.h"

#include <QByteArray>
#include <QString>

#include <optional>
#include <vector>

namespace mervin {

// Writes measurements into PDFs via qpdf (MuPDF stays read-only). Like PageOps,
// every method takes file paths and writes a NEW file - it never mutates the
// source in place. All qpdf usage is confined to the .cpp. Reuses
// QpdfService::Status (NeedsPassword when the source is user-password encrypted
// and no password is supplied).
//
// Two output shapes share one geometry primitive (render::emitMeasurementOps):
//   flatten   - burns the marks into each page's content as vector graphics
//               (visible in any viewer; not editable afterward).
//   embedMervin - stores a Mervin-only JSON blob in the catalog (invisible to
//               other viewers; reopened and made editable by Mervin).
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
