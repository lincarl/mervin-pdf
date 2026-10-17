#pragma once

#include "security/QpdfService.h"

#include <QCoreApplication>
#include <QList>
#include <QString>
#include <QStringList>

#include <functional>

namespace mervin {

// Structural page operations backed by qpdf. All page indices are 0-based;
// out-of-range indices fail the operation. Operations never modify the source in
// place - they write a new file (or, for split, a set of files). All qpdf usage
// is confined to the .cpp. Reuses QpdfService::Status (NeedsPassword when the
// source is user-password encrypted and no password is supplied). Each source
// reports job::reading() when it opens and again when qpdf reads it while writing
// (processReportedFile), and each output goes through AtomicPdfWriter, so inside a
// job::Scope a stopped operation opens and replaces nothing more. A split stopped
// part way keeps the files it wrote before.
//
// Every output keeps the source's encryption, its passwords and permission flags,
// when all its pages come from one source: Rotate, Delete, Extract, each Split part,
// and a merge whose inputs all name one path. A merge of several files is not
// encrypted. qpdf writes V4 copies with AES, so a V4 RC4 source, such as Mervin's
// RC4-128 option writes, gives AES-128.
class PageOps
{
    Q_DECLARE_TR_FUNCTIONS(mervin::PageOps)

public:
    using Status = QpdfService::Status;

    // Number of pages, or -1 on error.
    static int pageCount(const QString &path, const QString &password = QString());

    // Page count plus a status, so a caller can tell "this file needs a password"
    // from "this file is missing or damaged" - which pageCount()'s single -1
    // cannot express. *count is written only on Ok. Reading with the same library
    // that will do the writing is deliberate: whatever this accepts, merge() can
    // then actually read. *encrypted, also written only on Ok, says whether the file
    // is encrypted, which includes a file with only an owner password that opens
    // without one.
    static Status probe(const QString &path, int *count, const QString &password = QString(),
                        QString *error = nullptr, bool *encrypted = nullptr);

    // Write a copy with the given pages removed. The copy keeps the source's structure and
    // encryption but leaves out what only the removed pages used: the pages, their
    // annotations, images and form fields, and the outline items, destination names and
    // structure elements that lead only to them. Other references to the pages become null.
    // On failure, failedIndex is 0 when the source could not be opened or read, including
    // measurement data it cannot read, as merge() names its input, or -1 otherwise.
    static Status deletePages(const QString &inPath, const QString &outPath,
                              const QList<int> &pages, const QString &password = QString(),
                              QString *error = nullptr, int *failedIndex = nullptr);

    // Rotate `pages` by `angle` (a multiple of 90). relative=true adds to the
    // existing rotation; false sets it absolutely.
    static Status rotatePages(const QString &inPath, const QString &outPath,
                              const QList<int> &pages, int angle, bool relative = true,
                              const QString &password = QString(), QString *error = nullptr);

    // One source in a merge plan. `pages` are 0-based indices in the order they
    // should appear; an empty list means every page. Duplicates are honoured, so
    // a file may contribute the same page more than once, and the same file may
    // appear as several inputs. `password` is used only if the source is
    // user-password encrypted.
    struct MergeInput
    {
        QString path;
        QList<int> pages;
        QString password;
    };

    // Concatenate selected pages in input order (one input performs extraction). Reject invalid
    // page indices to avoid silently shortening output. Form fields come along with their
    // pages, without the fields and values of pages left out. On failure, failedIndex names
    // the input that could not be opened or read, including a missing page and measurement
    // data it cannot read, or is -1 for errors while building or writing the output. qpdf
    // reads most stream data while writing, so a damaged stream in an input also gives -1.
    // Optional mayCommit runs under the output lock after flushing and before commit admission.
    // The output is encrypted like its source when every input names the same path.
    static Status merge(const QList<MergeInput> &inputs, const QString &outPath,
                        QString *error = nullptr, int *failedIndex = nullptr,
                        const std::function<bool()> &mayCommit = {});

    // Concatenate all pages of `inPaths` (in order) into one document.
    static Status merge(const QStringList &inPaths, const QString &outPath, QString *error = nullptr);

    // Write each page of the source to "<outDir>/<baseName>-NNN.pdf", with the form fields
    // of that page as merge() copies them. The
    // produced paths are appended to `outFiles` if provided. `mayWrite`, when given,
    // gets every output path once the source is read, before the first write. When it
    // answers false, nothing is written and the split fails without an error, as the
    // caller knows why. `mayCommit`, when given, gets each output path under that
    // output's destination lock, after the part is flushed and before it replaces the
    // file. When it answers false, that file and every later one stay as they are, the
    // parts committed before stay, and the split fails. Inside a stopped job::Scope no
    // further file is written.
    static Status split(const QString &inPath, const QString &outDir, const QString &baseName,
                        const QString &password = QString(), QStringList *outFiles = nullptr,
                        QString *error = nullptr,
                        const std::function<bool(const QStringList &)> &mayWrite = {},
                        const std::function<bool(const QString &)> &mayCommit = {});
};

} // namespace mervin
