#pragma once

#include <QString>
#include <QStringList>

namespace mervin {

// Locates the writable Tesseract language-data folder in the user profile, lists
// installed models, and points the user at the official repository. Downloads
// and manually supplied .traineddata files share this folder.
namespace TessdataManager {

// The tessdata directory (created if missing).
QString directory();

// Installed language codes (the base names of *.traineddata), sorted.
QStringList installedLanguages();

// Friendly label for a Tesseract language code in the UI language (for
// example, "English" for "eng", "Engelska" in Swedish). Codes Mervin has no
// name for get QLocale's English name, or are returned unchanged.
QString languageName(const QString &code);

// The same label in English whatever the UI language, so a search can match
// either name.
QString englishLanguageName(const QString &code);

// Open the tessdata folder in Explorer.
void openFolder();

// Open the official best-quality model repository in the default browser.
void openRepository();

// The tessdata_best repository URL (shown / opened in the UI).
QString repositoryUrl();

} // namespace TessdataManager

} // namespace mervin
