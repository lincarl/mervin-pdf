#include "security/PageOps.h"
#include "security/AtomicPdfWriter.h"
#include "security/MeasurementPages.h"
#include <algorithm>
#include <stdexcept>

#include <qpdf/Constants.h>
#include <qpdf/QPDF.hh>
#include <qpdf/QPDFExc.hh>
#include <qpdf/QPDFPageDocumentHelper.hh>
#include <qpdf/QPDFPageObjectHelper.hh>

#include <QDir>

#include <exception>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace mervin {

namespace {

std::string u8(const QString &s)
{
    const QByteArray b = s.toUtf8();
    return std::string(b.constData(), static_cast<size_t>(b.size()));
}

PageOps::Status open(QPDF &q, const QString &path, const QString &password, QString *error)
{
    try {
        const std::string pw = u8(password);
        q.processFile(u8(path).c_str(), password.isEmpty() ? nullptr : pw.c_str());
        return PageOps::Status::Ok;
    } catch (const QPDFExc &e) {
        if (e.getErrorCode() == qpdf_e_password) {
            // QpdfService's message, so translators see it once.
            if (error)
                *error = QpdfService::tr("A password is required to open this document.");
            return PageOps::Status::NeedsPassword;
        }
        if (error)
            *error = QString::fromUtf8(e.what());
        return PageOps::Status::Failed;
    } catch (const std::exception &e) {
        if (error)
            *error = QString::fromUtf8(e.what());
        return PageOps::Status::Failed;
    }
}

} // namespace

int PageOps::pageCount(const QString &path, const QString &password)
{
    QPDF q;
    if (open(q, path, password, nullptr) != Status::Ok)
        return -1;
    try {
        return static_cast<int>(QPDFPageDocumentHelper(q).getAllPages().size());
    } catch (const std::exception &) {
        return -1;
    }
}

PageOps::Status PageOps::probe(const QString &path, int *count, const QString &password,
                               QString *error)
{
    QPDF q;
    const Status st = open(q, path, password, error);
    if (st != Status::Ok)
        return st;
    try {
        if (count)
            *count = static_cast<int>(QPDFPageDocumentHelper(q).getAllPages().size());
        return Status::Ok;
    } catch (const std::exception &e) {
        if (error)
            *error = QString::fromUtf8(e.what());
        return Status::Failed;
    }
}

PageOps::Status PageOps::deletePages(const QString &inPath, const QString &outPath,
                                     const QList<int> &pages, const QString &password,
                                     QString *error)
{
    QPDF q;
    const Status st = open(q, inPath, password, error);
    if (st != Status::Ok)
        return st;
    try {
        QPDFPageDocumentHelper dh(q);
        auto all = dh.getAllPages();
        const std::set<int> drop(pages.begin(), pages.end());
        QList<int> kept;
        for (int index : drop)
            if (index < 0 || index >= int(all.size()))
                throw std::runtime_error(tr("Page does not exist.").toStdString());
        if (drop.size() == all.size())
            throw std::runtime_error(tr("Cannot delete every page.").toStdString());
        for (int i = 0; i < static_cast<int>(all.size()); ++i)
            if (drop.count(i))
                dh.removePage(all[static_cast<size_t>(i)]);
            else
                kept.append(i);
        MeasureDoc mapped;
        measurementPages::append(mapped, measurementPages::read(q), kept, 0);
        measurementPages::write(q, mapped);
        AtomicPdfWriter output(q, outPath);
        output.write();
        return Status::Ok;
    } catch (const std::exception &e) {
        if (error)
            *error = QString::fromUtf8(e.what());
        return Status::Failed;
    }
}

PageOps::Status PageOps::rotatePages(const QString &inPath, const QString &outPath,
                                     const QList<int> &pages, int angle, bool relative,
                                     const QString &password, QString *error)
{
    QPDF q;
    const Status st = open(q, inPath, password, error);
    if (st != Status::Ok)
        return st;
    try {
        QPDFPageDocumentHelper dh(q);
        auto all = dh.getAllPages();
        auto data = measurementPages::read(q);
        if (angle % 90 != 0)
            throw std::runtime_error(
                tr("Rotation must be a multiple of 90 degrees.").toStdString());
        for (int idx : pages) {
            if (idx < 0 || idx >= int(all.size()))
                throw std::runtime_error(tr("Page does not exist.").toStdString());
            auto &page = all[idx];
            auto rotation = page.getAttribute("/Rotate", false);
            const int before = rotation.isInteger() ? int(rotation.getIntValue()) : 0;
            auto box = page.getCropBox().getArrayAsRectangle();
            double width = box.urx - box.llx, height = box.ury - box.lly;
            auto unit = page.getObjectHandle().getKey("/UserUnit");
            const double factor = unit.isNumber() ? unit.getNumericValue() : 1.0;
            width *= factor;
            height *= factor;
            if ((before % 180 + 180) % 180)
                std::swap(width, height);
            page.rotatePage(angle, relative);
            const int after = int(page.getAttribute("/Rotate", false).getIntValue());
            const int delta = ((after - before) % 360 + 360) % 360;
            const auto turn = [=](QPointF point) {
                switch (delta) {
                case 90: return QPointF(height - point.y(), point.x());
                case 180: return QPointF(width - point.x(), height - point.y());
                case 270: return QPointF(point.y(), width - point.x());
                default: return point;
                }
            };
            for (auto &mark : data.measurements)
                if (mark.page == idx) {
                    for (auto &point : mark.pts)
                        point = turn(point);
                    if (mark.hasLabelPos)
                        mark.labelPos = turn(mark.labelPos);
                }
            if (delta == 90 || delta == 270)
                for (auto &scale : data.pageScales)
                    if (scale.page == idx)
                        std::swap(scale.mmPerPointX, scale.mmPerPointY);
        }
        measurementPages::write(q, data);
        AtomicPdfWriter output(q, outPath);
        output.write();
        return Status::Ok;
    } catch (const std::exception &e) {
        if (error)
            *error = QString::fromUtf8(e.what());
        return Status::Failed;
    }
}

PageOps::Status PageOps::merge(const QList<MergeInput> &inputs, const QString &outPath,
                               QString *error, int *failedIndex)
{
    if (failedIndex)
        *failedIndex = -1;
    try {
        QPDF out;
        out.emptyPDF();
        QPDFPageDocumentHelper odh(out);
        // Sources must outlive write() so their stream data can be copied. The
        // same path may appear as several inputs (the same file contributing two
        // different page ranges), so each distinct path is opened exactly once
        // and reused - both to save the parse and because two QPDFs over one file
        // would copy its shared objects into the output twice.
        std::vector<std::unique_ptr<QPDF>> sources;
        std::map<QString, QPDF *> opened;
        MeasureDoc mergedMeasurements;
        int outputPage = 0;

        for (int i = 0; i < inputs.size(); ++i) {
            const MergeInput &in = inputs.at(i);
            auto it = opened.find(in.path);
            if (it == opened.end()) {
                auto src = std::make_unique<QPDF>();
                const Status st = open(*src, in.path, in.password, error);
                if (st != Status::Ok) {
                    if (failedIndex)
                        *failedIndex = i;
                    return st;
                }
                it = opened.emplace(in.path, src.get()).first;
                sources.push_back(std::move(src));
            }

            auto all = QPDFPageDocumentHelper(*it->second).getAllPages();
            const int n = static_cast<int>(all.size());
            QList<int> selected = in.pages;
            if (selected.isEmpty())
                for (int page = 0; page < n; ++page)
                    selected.append(page);
            for (int idx : selected) {
                if (idx < 0 || idx >= n) {
                    // The caller names the file (it has failedIndex), so this says
                    // only what the caller cannot know.
                    if (error) {
                        //: Merge or extract error, shown below a line that names the file.
                        //: %n is the file's page count, %1 the missing page number.
                        *error = tr("It has %n page(s); page %1 does not exist.", nullptr, n)
                                     .arg(idx + 1);
                    }
                    if (failedIndex)
                        *failedIndex = i;
                    return Status::Failed;
                }
                odh.addPage(all[static_cast<size_t>(idx)], false);
            }
            measurementPages::append(mergedMeasurements, measurementPages::read(*it->second),
                                     selected, outputPage);
            outputPage += selected.size();
        }
        measurementPages::write(out, mergedMeasurements);

        AtomicPdfWriter output(out, outPath);
        output.write();
        return Status::Ok;
    } catch (const std::exception &e) {
        if (error)
            *error = QString::fromUtf8(e.what());
        return Status::Failed;
    }
}

PageOps::Status PageOps::merge(const QStringList &inPaths, const QString &outPath, QString *error)
{
    QList<MergeInput> inputs;
    inputs.reserve(inPaths.size());
    for (const QString &p : inPaths)
        inputs.append(MergeInput{p, {}, QString()});
    return merge(inputs, outPath, error, nullptr);
}

PageOps::Status PageOps::split(const QString &inPath, const QString &outDir, const QString &baseName,
                               const QString &password, QStringList *outFiles, QString *error)
{
    QPDF q;
    const Status st = open(q, inPath, password, error);
    if (st != Status::Ok)
        return st;
    try {
        QPDFPageDocumentHelper dh(q);
        auto all = dh.getAllPages();
        const QDir dir(outDir);
        const auto data = measurementPages::read(q);
        for (int i = 0; i < static_cast<int>(all.size()); ++i) {
            QPDF out;
            out.emptyPDF();
            QPDFPageDocumentHelper(out).addPage(all[static_cast<size_t>(i)], false);
            MeasureDoc selected;
            measurementPages::append(selected, data, {i}, 0);
            measurementPages::write(out, selected);
            const QString name = QStringLiteral("%1-%2.pdf")
                                     .arg(baseName)
                                     .arg(i + 1, 3, 10, QLatin1Char('0'));
            const QString path = dir.filePath(name);
            AtomicPdfWriter output(out, path);
            output.write();
            if (outFiles)
                outFiles->append(path);
        }
        return Status::Ok;
    } catch (const std::exception &e) {
        if (error)
            *error = QString::fromUtf8(e.what());
        return Status::Failed;
    }
}

} // namespace mervin
