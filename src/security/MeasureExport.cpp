#include "security/MeasureExport.h"

#include <qpdf/Buffer.hh>
#include <qpdf/Constants.h>
#include <qpdf/QPDF.hh>
#include <qpdf/QPDFExc.hh>
#include <qpdf/QPDFObjectHandle.hh>
#include <qpdf/QPDFPageDocumentHelper.hh>
#include <qpdf/QPDFPageObjectHelper.hh>
#include <qpdf/QPDFWriter.hh>

#include <QPointF>

#include <algorithm>
#include <exception>
#include <initializer_list>
#include <string>
#include <vector>

namespace mervin {

namespace {

using Status = MeasureExport::Status;

std::string u8(const QString &s)
{
    const QByteArray b = s.toUtf8();
    return std::string(b.constData(), static_cast<size_t>(b.size()));
}

// Mirrors PageOps' open(): NeedsPassword when the source is user-encrypted and
// no/incorrect password is supplied.
Status openQpdf(QPDF &q, const QString &path, const QString &password, QString *error)
{
    try {
        const std::string pw = u8(password);
        q.processFile(u8(path).c_str(), password.isEmpty() ? nullptr : pw.c_str());
        return Status::Ok;
    } catch (const QPDFExc &e) {
        if (e.getErrorCode() == qpdf_e_password) {
            if (error)
                *error = QStringLiteral("A password is required to open this document.");
            return Status::NeedsPassword;
        }
        if (error)
            *error = QString::fromUtf8(e.what());
        return Status::Failed;
    } catch (const std::exception &e) {
        if (error)
            *error = QString::fromUtf8(e.what());
        return Status::Failed;
    }
}

// Add a base-14 Helvetica (WinAnsi) under kMeasureFontResource to a /Resources
// dict, used by the label text the emitter draws. Idempotent.
void ensureHelvetica(QPDF &q, QPDFObjectHandle resources)
{
    auto fonts = resources.getKey("/Font");
    if (!fonts.isDictionary()) {
        fonts = QPDFObjectHandle::newDictionary();
        resources.replaceKey("/Font", fonts);
    }
    const std::string key = std::string("/") + kMeasureFontResource;
    if (!fonts.hasKey(key)) {
        auto f = QPDFObjectHandle::newDictionary();
        f.replaceKey("/Type", QPDFObjectHandle::newName("/Font"));
        f.replaceKey("/Subtype", QPDFObjectHandle::newName("/Type1"));
        f.replaceKey("/BaseFont", QPDFObjectHandle::newName("/Helvetica"));
        f.replaceKey("/Encoding", QPDFObjectHandle::newName("/WinAnsiEncoding"));
        fonts.replaceKey(key, q.makeIndirectObject(f));
    }
}

} // namespace

MeasureExport::Status MeasureExport::flatten(const QString &inPath, const QString &outPath,
                                             const std::vector<RenderMeasurement> &marks,
                                             const QString &password, QString *error)
{
    QPDF q;
    const Status st = openQpdf(q, inPath, password, error);
    if (st != Status::Ok)
        return st;
    try {
        QPDFPageDocumentHelper dh(q);
        auto pages = dh.getAllPages();
        const int n = static_cast<int>(pages.size());
        for (int i = 0; i < n; ++i) {
            std::string content;
            for (const RenderMeasurement &m : marks)
                if (m.page == i) {
                    EmitStyle style;
                    if (m.lineWidth > 0.0)
                        style.lineWidth = m.lineWidth;
                    content += emitMeasurementOps(m, style);
                }
            if (content.empty())
                continue;

            QPDFObjectHandle pageObj = pages[static_cast<size_t>(i)].getObjectHandle();
            auto res = pageObj.getKey("/Resources");
            if (!res.isDictionary()) {
                res = QPDFObjectHandle::newDictionary();
                pageObj.replaceKey("/Resources", res);
            }
            ensureHelvetica(q, res);

            // Bracket the existing content in q/Q (prepend a save, append a
            // restore) so our marks draw from the default graphics state, then
            // append the marks. This is the standard stamping idiom.
            pages[static_cast<size_t>(i)].addPageContents(q.newStream("q\n"), true);
            pages[static_cast<size_t>(i)].addPageContents(q.newStream("Q\n" + content), false);
        }
        // Flattened marks are baked into the page content as plain vector graphics;
        // the editable Mervin blob (if the source carried one from "Save edits")
        // must not travel with the exported copy. Drop it so the export is a clean,
        // measurements-burned-in PDF with no Mervin-private data.
        if (q.getRoot().hasKey("/Mervin_Measurements"))
            q.getRoot().removeKey("/Mervin_Measurements");
        QPDFWriter w(q, u8(outPath).c_str());
        w.setStaticID(false);
        w.write();
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
    QPDF q;
    const Status st = openQpdf(q, inPath, password, error);
    if (st != Status::Ok)
        return st;
    try {
        const QByteArray json = serializeMeasurements(doc);
        auto stream = q.newStream(std::string(json.constData(), static_cast<size_t>(json.size())));
        q.getRoot().replaceKey("/Mervin_Measurements", stream);
        QPDFWriter w(q, u8(outPath).c_str());
        w.setStaticID(false);
        w.write();
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
    QPDF q;
    if (openQpdf(q, inPath, password, nullptr) != Status::Ok)
        return std::nullopt;
    try {
        auto root = q.getRoot();
        if (!root.hasKey("/Mervin_Measurements"))
            return std::nullopt;
        auto blob = root.getKey("/Mervin_Measurements");
        if (blob.isStream()) {
            auto buf = blob.getStreamData();
            if (!buf)
                return std::nullopt;
            return QByteArray(reinterpret_cast<const char *>(buf->getBuffer()),
                              static_cast<int>(buf->getSize()));
        }
        if (blob.isString()) {
            const std::string sv = blob.getStringValue();
            return QByteArray(sv.data(), static_cast<int>(sv.size()));
        }
        return std::nullopt;
    } catch (const std::exception &) {
        return std::nullopt;
    }
}

} // namespace mervin
