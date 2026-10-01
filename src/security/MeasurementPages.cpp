#include "security/MeasurementPages.h"

#include <qpdf/Buffer.hh>
#include <qpdf/QPDF.hh>
#include <stdexcept>
#include <unordered_map>

namespace mervin::measurementPages {
MeasureDoc read(QPDF &pdf)
{
    MeasureDoc data;
    auto stream = pdf.getRoot().getKey("/Mervin_Measurements");
    if (stream.isNull())
        return data;
    if (!stream.isStream())
        throw std::runtime_error("Invalid measurement metadata.");
    auto bytes = stream.getStreamData();
    if (!parseMeasurements(QByteArray(reinterpret_cast<const char *>(bytes->getBuffer()),
                                      bytes->getSize()), &data) || data.version != 1)
        throw std::runtime_error("Unsupported measurement metadata.");
    return data;
}

void write(QPDF &pdf, const MeasureDoc &data)
{
    if (data.measurements.empty() && data.pageScales.empty()) {
        pdf.getRoot().removeKey("/Mervin_Measurements");
        return;
    }
    pdf.getRoot().replaceKey("/Mervin_Measurements",
                            pdf.newStream(serializeMeasurements(data).toStdString()));
}

void append(MeasureDoc &output, const MeasureDoc &source, const QList<int> &pages, int offset)
{
    if (output.measurements.empty() && output.pageScales.empty()) {
        output.unit = source.unit;
        output.precision = source.precision;
        output.lineWidth = source.lineWidth;
    }
    std::unordered_map<int, std::vector<const Measurement *>> marks;
    std::unordered_map<int, std::vector<const PageScale *>> scales;
    for (const auto &mark : source.measurements)
        marks[mark.page].push_back(&mark);
    for (const auto &scale : source.pageScales)
        scales[scale.page].push_back(&scale);
    for (int dest = 0; dest < pages.size(); ++dest) {
        if (auto it = marks.find(pages[dest]); it != marks.end())
            for (const auto *original : it->second) {
                auto mark = *original;
                mark.page = offset + dest;
                output.measurements.push_back(std::move(mark));
            }
        if (auto it = scales.find(pages[dest]); it != scales.end())
            for (const auto *original : it->second) {
                auto scale = *original;
                scale.page = offset + dest;
                output.pageScales.push_back(std::move(scale));
            }
    }
}
}
