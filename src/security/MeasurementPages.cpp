#include "security/MeasurementPages.h"

#include <QCoreApplication>
#include <qpdf/Constants.h>
#include <qpdf/Pipeline.hh>
#include <qpdf/QPDF.hh>
#include <exception>
#include <stdexcept>
#include <unordered_map>

namespace mervin::measurementPages {
namespace {
// Collects decoded bytes and throws once they would pass kMaxBlobBytes. qpdf catches
// the exception and stops decoding.
class CappedSink final : public Pipeline
{
public:
    explicit CappedSink(QByteArray &bytes) : Pipeline("measurement blob", nullptr), bytes_(bytes) {}

    void write(unsigned char const *data, size_t length) override
    {
        if (length > size_t(kMaxBlobBytes - bytes_.size()))
            throw std::length_error("measurement blob exceeds its size limit");
        bytes_.append(reinterpret_cast<const char *>(data), qsizetype(length));
    }

    void finish() override {}

private:
    QByteArray &bytes_;
};
} // namespace

std::optional<QByteArray> decodeBlob(QPDFObjectHandle &stream)
{
    QByteArray bytes;
    CappedSink sink(bytes);
    bool decoded = false;
    try {
        // qpdf prints no warning, since the caller reports a failure as missing or
        // invalid metadata.
        if (!stream.pipeStreamData(&sink, &decoded, 0, qpdf_dl_generalized, true) || !decoded)
            return std::nullopt;
    } catch (const std::exception &) {
        return std::nullopt;
    }
    return bytes;
}

MeasureDoc read(QPDF &pdf)
{
    MeasureDoc data;
    auto stream = pdf.getRoot().getKey("/Mervin_Measurements");
    if (stream.isNull())
        return data;
    const auto bytes = stream.isStream() ? decodeBlob(stream) : std::nullopt;
    // Page operations show these to the user.
    if (!bytes) {
        //: Page operation error. The measurements Mervin saved in this PDF are damaged
        //: or too large to read.
        throw std::runtime_error(QCoreApplication::translate(
            "mervin::measurementPages", "Invalid measurement metadata.").toStdString());
    }
    if (!parseMeasurements(*bytes, &data) || data.version != 1) {
        //: Page operation error. The measurements Mervin saved in this PDF are damaged or
        //: come from a newer version.
        throw std::runtime_error(QCoreApplication::translate(
            "mervin::measurementPages", "Unsupported measurement metadata.").toStdString());
    }
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
