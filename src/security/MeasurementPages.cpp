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
// Collects decoded bytes and throws once they would pass kMaxMeasurementBlobBytes. qpdf
// catches the exception and stops decoding.
class CappedSink final : public Pipeline
{
public:
    explicit CappedSink(QByteArray &bytes) : Pipeline("measurement blob", nullptr), bytes_(bytes) {}

    void write(unsigned char const *data, size_t length) override
    {
        if (length > size_t(kMaxMeasurementBlobBytes - bytes_.size()))
            throw std::length_error("measurement blob exceeds its size limit");
        bytes_.append(reinterpret_cast<const char *>(data), qsizetype(length));
    }

    void finish() override {}

private:
    QByteArray &bytes_;
};

// Whether the blob `dict` names only the encoding Mervin writes: no filter, or a single
// /FlateDecode as a name or a one-element array, and no /DecodeParms. qpdf ignores /F and
// /DP, MuPDF's abbreviations for /Filter and /DecodeParms, so they are refused too, and
// the open-time reader (Document::mervinMeasurements) refuses the same streams.
bool plainFlate(QPDFObjectHandle dict)
{
    for (const char *key : {"/DecodeParms", "/DP", "/F"}) {
        if (!dict.getKey(key).isNull())
            return false;
    }
    QPDFObjectHandle filter = dict.getKey("/Filter");
    if (filter.isArray() && filter.getArrayNItems() == 1)
        filter = filter.getArrayItem(0);
    return filter.isNull() || (filter.isName() && filter.getName() == "/FlateDecode");
}
} // namespace

std::optional<QByteArray> decodeBlob(QPDFObjectHandle &stream)
{
    // qpdf builds a filter's buffers from its parameters before any data reaches the sink.
    if (!plainFlate(stream.getDict()))
        return std::nullopt;
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

QString tooLargeError()
{
    //: Reason a save, copy or page operation fails without writing anything. The
    //: measurements Mervin would store in the PDF pass the 16 MiB it reads back, as can
    //: happen when merging files that carry many measurements.
    return QCoreApplication::translate("mervin::measurementPages",
                                       "The measurements are too large to save.");
}

void store(QPDF &pdf, const MeasureDoc &data)
{
    const QByteArray json = serializeMeasurements(data);
    // The readers would refuse a larger blob and drop the measurements on the next open.
    if (json.size() > kMaxMeasurementBlobBytes)
        throw std::runtime_error(tooLargeError().toStdString());
    pdf.getRoot().replaceKey("/Mervin_Measurements", pdf.newStream(json.toStdString()));
}

void write(QPDF &pdf, const MeasureDoc &data)
{
    if (data.measurements.empty() && data.pageScales.empty()) {
        pdf.getRoot().removeKey("/Mervin_Measurements");
        return;
    }
    store(pdf, data);
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
