#pragma once

#include "render/MeasureContent.h"
#include <QByteArray>
#include <QList>
#include <optional>

class QPDF;
class QPDFObjectHandle;

namespace mervin::measurementPages {
// The most decoded bytes a /Mervin_Measurements blob may hold. Mervin's JSON stays far
// smaller, and the limit keeps a small, highly compressed stream from filling memory.
inline constexpr qsizetype kMaxBlobBytes = 16 * 1024 * 1024;
// Decodes the blob `stream` and stops once it passes kMaxBlobBytes. nullopt when qpdf
// cannot decode it or it is larger.
std::optional<QByteArray> decodeBlob(QPDFObjectHandle &stream);
MeasureDoc read(QPDF &pdf);
void write(QPDF &pdf, const MeasureDoc &data);
// Append in output order; duplicates become independent measurements.
void append(MeasureDoc &output, const MeasureDoc &source, const QList<int> &pages, int offset);
}
