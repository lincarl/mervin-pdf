#pragma once

#include "render/MeasureContent.h"
#include <QByteArray>
#include <QList>
#include <QString>
#include <optional>

class QPDF;
class QPDFObjectHandle;

namespace mervin::measurementPages {
// Decodes the blob `stream` with qpdf. nullopt when the stream has a filter other than a
// single /FlateDecode, has /DecodeParms, has /F or /DP (MuPDF's abbreviations for /Filter
// and /DecodeParms), decodes to more than kMaxMeasurementBlobBytes, or qpdf cannot decode
// it.
std::optional<QByteArray> decodeBlob(QPDFObjectHandle &stream);
MeasureDoc read(QPDF &pdf);
// The translated reason store() and write() give for measurements whose blob would pass
// kMaxMeasurementBlobBytes.
QString tooLargeError();
// Stores `data` as the catalog's /Mervin_Measurements blob, even when it holds no
// measurements. Throws std::runtime_error with tooLargeError() when the blob would pass
// kMaxMeasurementBlobBytes, which the readers refuse, so call it before writing output.
void store(QPDF &pdf, const MeasureDoc &data);
// Like store(), but removes the blob when `data` has no measurements and no scales.
void write(QPDF &pdf, const MeasureDoc &data);
// Append in output order; duplicates become independent measurements.
void append(MeasureDoc &output, const MeasureDoc &source, const QList<int> &pages, int offset);
}
