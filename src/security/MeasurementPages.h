#pragma once

#include "render/MeasureContent.h"
#include <QList>

class QPDF;

namespace mervin::measurementPages {
MeasureDoc read(QPDF &pdf);
void write(QPDF &pdf, const MeasureDoc &data);
// Append in output order; duplicates become independent measurements.
void append(MeasureDoc &output, const MeasureDoc &source, const QList<int> &pages, int offset);
}
