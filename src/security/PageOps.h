#pragma once

#include "security/QpdfService.h"

#include <QList>
#include <QString>
#include <QStringList>

namespace mervin {

// Structural page operations backed by qpdf. All page indices are 0-based;
// out-of-range indices are ignored. Operations never modify the source in
// place - they write a new file (or, for split, a set of files). All qpdf usage
// is confined to the .cpp. Reuses QpdfService::Status (NeedsPassword when the
// source is user-password encrypted and no password is supplied).
class PageOps
{
public:
    using Status = QpdfService::Status;

    // Number of pages, or -1 on error.
    static int pageCount(const QString &path, const QString &password = QString());

    // Page count plus a status, so a caller can tell "this file needs a password"
    // from "this file is missing or damaged" - which pageCount()'s single -1
    // cannot express. *count is written only on Ok. Reading with the same library
    // that will do the writing is deliberate: whatever this accepts, merge() can
    // then actually read.
    static Status probe(const QString &path, int *count, const QString &password = QString(),
                        QString *error = nullptr);

    // Write a copy with the given pages removed.
    static Status deletePages(const QString &inPath, const QString &outPath,
                              const QList<int> &pages, const QString &password = QString(),
                              QString *error = nullptr);

    // Write a new document containing only `pages`, in the given order. Doubles
    // as "reorder" when `pages` is a permutation of the whole document.
    static Status extractPages(const QString &inPath, const QString &outPath,
                               const QList<int> &pages, const QString &password = QString(),
                               QString *error = nullptr);

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

    // Concatenate the selected pages of `inputs`, in order, into one document.
    // On failure *failedIndex (when given) receives the index of the input that
    // could not be read, or -1 when the failure was not attributable to one
    // input (a write error, say) - so a caller can name the offending file.
    //
    // Unlike extractPages(), an out-of-range page index is an error rather than
    // being skipped: a page count that went stale between the caller's probe and
    // this write would otherwise silently produce a shorter document than the
    // caller promised its user, and still report success.
    static Status merge(const QList<MergeInput> &inputs, const QString &outPath,
                        QString *error = nullptr, int *failedIndex = nullptr);

    // Concatenate all pages of `inPaths` (in order) into one document.
    static Status merge(const QStringList &inPaths, const QString &outPath, QString *error = nullptr);

    // Write each page of the source to "<outDir>/<baseName>-NNN.pdf". The
    // produced paths are appended to `outFiles` if provided.
    static Status split(const QString &inPath, const QString &outDir, const QString &baseName,
                        const QString &password = QString(), QStringList *outFiles = nullptr,
                        QString *error = nullptr);
};

} // namespace mervin
