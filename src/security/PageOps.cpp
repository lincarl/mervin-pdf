#include "security/PageOps.h"

#include <qpdf/Constants.h>
#include <qpdf/QPDF.hh>
#include <qpdf/QPDFExc.hh>
#include <qpdf/QPDFPageDocumentHelper.hh>
#include <qpdf/QPDFPageObjectHelper.hh>
#include <qpdf/QPDFWriter.hh>

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
            if (error)
                *error = QStringLiteral("A password is required to open this document.");
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
        for (int i = 0; i < static_cast<int>(all.size()); ++i)
            if (drop.count(i))
                dh.removePage(all[static_cast<size_t>(i)]);
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

PageOps::Status PageOps::extractPages(const QString &inPath, const QString &outPath,
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
        QPDF out;
        out.emptyPDF();
        QPDFPageDocumentHelper odh(out);
        for (int idx : pages)
            if (idx >= 0 && idx < static_cast<int>(all.size()))
                odh.addPage(all[static_cast<size_t>(idx)], false);
        QPDFWriter w(out, u8(outPath).c_str());
        w.setStaticID(false);
        w.write(); // `q` stays alive through write(), so stream data copies cleanly
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
        for (int idx : pages)
            if (idx >= 0 && idx < static_cast<int>(all.size()))
                all[static_cast<size_t>(idx)].rotatePage(angle, relative);
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
            if (in.pages.isEmpty()) {
                for (auto &page : all)
                    odh.addPage(page, false);
                continue;
            }
            for (int idx : in.pages) {
                if (idx < 0 || idx >= n) {
                    // The caller names the file (it has failedIndex), so this says
                    // only what the caller cannot know. No "%n page(s)": Qt leaves
                    // the "(s)" verbatim when no translator is loaded.
                    if (error)
                        *error = n == 1
                                     ? QStringLiteral("It has 1 page; page %1 does not exist.")
                                           .arg(idx + 1)
                                     : QStringLiteral("It has %1 pages; page %2 does not exist.")
                                           .arg(n)
                                           .arg(idx + 1);
                    if (failedIndex)
                        *failedIndex = i;
                    return Status::Failed;
                }
                odh.addPage(all[static_cast<size_t>(idx)], false);
            }
        }

        QPDFWriter w(out, u8(outPath).c_str());
        w.setStaticID(false);
        w.write();
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
        for (int i = 0; i < static_cast<int>(all.size()); ++i) {
            QPDF out;
            out.emptyPDF();
            QPDFPageDocumentHelper(out).addPage(all[static_cast<size_t>(i)], false);
            const QString name = QStringLiteral("%1-%2.pdf")
                                     .arg(baseName)
                                     .arg(i + 1, 3, 10, QLatin1Char('0'));
            const QString path = dir.filePath(name);
            QPDFWriter w(out, u8(path).c_str());
            w.setStaticID(false);
            w.write();
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
