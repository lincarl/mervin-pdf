#pragma once

#include <QString>
#include <QStringList>

namespace mervin {

// Validate .traineddata before MuPDF/Tesseract reads it. Corrupt containers can trigger abort()
// or unchecked vector access, beyond fz_try/fz_catch recovery. Mirror
// TessdataManager::LoadMemBuffer and require unique unicharset entries: duplicates fail to grow
// the vector subsequently indexed by the loader.
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
