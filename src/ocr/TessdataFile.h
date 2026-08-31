#pragma once

#include <QString>
#include <QStringList>

namespace mervin {

// Structural validation of Tesseract .traineddata language files, run before
// any of them is handed to MuPDF's OCR device.
//
// Why this exists: Tesseract reacts to damaged language data with ASSERT_HOST ->
// abort(), or by writing past the end of a std::vector. Neither is a C++
// exception, so the fz_try/fz_catch around the OCR device cannot intercept it -
// a single bad file takes the whole process down with it, losing every open
// tab's unsaved edits. Since users install languages by dropping files into the
// tessdata folder by hand, that has to be caught before Tesseract sees it.
//
// The checks mirror Tesseract's own container parse (TessdataManager::
// LoadMemBuffer) and then verify the one invariant its unicharset loader assumes
// but never checks: that every line of a unicharset introduces a *new* unichar.
// A duplicate silently fails to grow the vector the loader then indexes into.
namespace TessdataFile {

// True if `path` is language data Tesseract can load safely. On failure returns
// false and, if `error` is non-null, sets it to a reason fit to show a user.
bool validate(const QString &path, QString *error = nullptr);

// Validate `<dir>/<lang>.traineddata` for each of `languages`. Names with no
// file present are skipped deliberately: Tesseract reports a missing language
// cleanly through its return value, so there is nothing to guard against, and
// second-guessing its name resolution here would reject setups that work.
bool validateLanguages(const QString &dir, const QStringList &languages,
                       QString *error = nullptr);

} // namespace TessdataFile

} // namespace mervin
