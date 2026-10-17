#pragma once

#include "render/MeasureContent.h"
#include <QByteArray>
#include <QList>
#include <optional>

class QPDF;
class QPDFObjectHandle;

namespace mervin::measurementPages {
// Decodes the blob `stream` with qpdf. nullopt when the stream has a filter other than up
// to kMaxMeasurementBlobFilters /FlateDecode, has filter parameters (/DecodeParms or the
// /DP and /F abbreviations MuPDF reads), decodes to more than kMaxMeasurementBlobBytes,
// or qpdf cannot decode it.
std::optional<QByteArray> decodeBlob(QPDFObjectHandle &stream);
MeasureDoc read(QPDF &pdf);
void write(QPDF &pdf, const MeasureDoc &data);
// Append in output order; duplicates become independent measurements.
void append(MeasureDoc &output, const MeasureDoc &source, const QList<int> &pages, int offset);
}
