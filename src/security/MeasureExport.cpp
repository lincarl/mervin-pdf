#include "security/MeasureExport.h"
#include "security/AtomicPdfWriter.h"
#include "security/MeasurementPages.h"
#include "security/QpdfSource.h"

#include <qpdf/QPDF.hh>
#include <qpdf/QPDFObjectHandle.hh>
#include <qpdf/QPDFPageDocumentHelper.hh>
#include <qpdf/QPDFPageObjectHelper.hh>

#include <QPointF>

#include <algorithm>
#include <exception>
#include <initializer_list>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace mervin {

namespace {

// Makes the writer copy the stored /Mervin_Measurements stream data as it is. Otherwise qpdf
// decodes and compresses it again, which changes its bytes and has no size limit.
void keepStoredMeasurements(QPDF &q)
{
    QPDFObjectHandle blob = q.getRoot().getKey("/Mervin_Measurements");
    if (blob.isStream())
        blob.setFilterOnWrite(false);
}

// Copy the font dictionary before adding a private name: resources may be inherited/shared.
std::string addHelvetica(QPDF &q, QPDFObjectHandle resources)
{
    auto fonts = resources.getKey("/Font");
    fonts = fonts.isDictionary() ? fonts.shallowCopy() : QPDFObjectHandle::newDictionary();
    resources.replaceKey("/Font", fonts);
    std::string name = kMeasureFontResource;
    for (int suffix = 1; fonts.hasKey("/" + name); ++suffix)
        name = std::string(kMeasureFontResource) + std::to_string(suffix);
    auto font = QPDFObjectHandle::newDictionary();
    font.replaceKey("/Type", QPDFObjectHandle::newName("/Font"));
    font.replaceKey("/Subtype", QPDFObjectHandle::newName("/Type1"));
    font.replaceKey("/BaseFont", QPDFObjectHandle::newName("/Helvetica"));
    font.replaceKey("/Encoding", QPDFObjectHandle::newName("/WinAnsiEncoding"));
    fonts.replaceKey("/" + name, q.makeIndirectObject(font));
    return name;
}

} // namespace

MeasureExport::Status MeasureExport::flatten(const QString &inPath, const QString &outPath,
                                             const std::vector<RenderMeasurement> &marks,
                                             const QString &password, QString *error,
                                             bool keepStoredBlob)
{
    std::unique_ptr<QPDF> source;
    const Status st = openQpdf(inPath, password, source, error);
    if (st != Status::Ok)
        return st;
    QPDF &q = *source;
    try {
        QPDFPageDocumentHelper dh(q);
        auto pages = dh.getAllPages();
        const int n = static_cast<int>(pages.size());
        std::map<int, std::vector<const RenderMeasurement *>> byPage;
        for (const auto &mark : marks)
            if (mark.page >= 0 && mark.page < n && mark.pts.size() >= 2)
                byPage[mark.page].push_back(&mark);
        for (const auto &[i, pageMarks] : byPage) {
            auto &page = pages[static_cast<size_t>(i)];
            auto resources = page.getAttribute("/Resources", true);
            if (!resources.isDictionary()) {
                resources = QPDFObjectHandle::newDictionary();
                page.getObjectHandle().replaceKey("/Resources", resources);
            }
            const std::string fontName = addHelvetica(q, resources);
            std::string content;
            for (const RenderMeasurement *mark : pageMarks) {
                EmitStyle style;
                if (mark->lineWidth > 0.0)
                    style.lineWidth = mark->lineWidth;
                content += emitMeasurementOps(*mark, style, fontName);
            }
            if (content.empty())
                continue;

            // Bracket the existing content in q/Q (prepend a save, append a
            // restore) so our marks draw from the default graphics state, then
            // append the marks. This is the standard stamping idiom.
            pages[static_cast<size_t>(i)].addPageContents(q.newStream("q\n"), true);
            pages[static_cast<size_t>(i)].addPageContents(q.newStream("Q\n" + content), false);
        }
        // Flattened marks are baked into the page content as plain vector graphics;
        // the editable Mervin blob (if the source carried one from "Save edits")
        // must not travel with the exported copy. Drop it so the export is a clean,
        // measurements-burned-in PDF with no Mervin-private data. A blob Mervin could not
        // read stays, because the export may replace the file it came from.
        if (keepStoredBlob)
            keepStoredMeasurements(q);
        else if (q.getRoot().hasKey("/Mervin_Measurements"))
            q.getRoot().removeKey("/Mervin_Measurements");
        AtomicPdfWriter output(q, outPath);
        output.write();
        return Status::Ok;
    } catch (const std::exception &e) {
        if (error)
            *error = QString::fromUtf8(e.what());
        return Status::Failed;
    }
}

MeasureExport::Status MeasureExport::embedMervin(const QString &inPath, const QString &outPath,
                                                 const MeasureDoc &doc, const QString &password,
                                                 QString *error)
{
    std::unique_ptr<QPDF> source;
    const Status st = openQpdf(inPath, password, source, error);
    if (st != Status::Ok)
        return st;
    QPDF &q = *source;
    try {
        measurementPages::store(q, doc); // throws before anything is written when too large
        AtomicPdfWriter output(q, outPath);
        output.write();
        return Status::Ok;
    } catch (const std::exception &e) {
        if (error)
            *error = QString::fromUtf8(e.what());
        return Status::Failed;
    }
}

MeasureExport::Status MeasureExport::keepMervin(const QString &inPath, const QString &outPath,
                                                const QString &password, QString *error)
{
    std::unique_ptr<QPDF> source;
    const Status st = openQpdf(inPath, password, source, error);
    if (st != Status::Ok)
        return st;
    try {
        keepStoredMeasurements(*source);
        AtomicPdfWriter output(*source, outPath);
        output.write();
        return Status::Ok;
    } catch (const std::exception &e) {
        if (error)
            *error = QString::fromUtf8(e.what());
        return Status::Failed;
    }
}

std::optional<QByteArray> MeasureExport::readMervinBlob(const QString &inPath,
                                                        const QString &password)
{
    std::unique_ptr<QPDF> source;
    if (openQpdf(inPath, password, source, nullptr) != Status::Ok)
        return std::nullopt;
    try {
        auto root = source->getRoot();
        if (!root.hasKey("/Mervin_Measurements"))
            return std::nullopt;
        auto blob = root.getKey("/Mervin_Measurements");
        // Mervin writes only streams, so a string or any other object gives no data.
        if (blob.isStream())
            return measurementPages::decodeBlob(blob);
        return std::nullopt;
    } catch (const std::exception &) {
        return std::nullopt;
    }
}

} // namespace mervin
