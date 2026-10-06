#pragma once

#include <QString>
#include <QStringList>

namespace mervin {

// Manages the Tesseract language-data folder (%APPDATA%/MervinPDF/tessdata/).
// The app never downloads data itself (no-network-by-default posture); the user
// drops .traineddata files into the folder. This class just locates the folder,
// lists installed languages, and points the user at the official repository.
namespace TessdataManager {

// The tessdata directory (created if missing).
QString directory();

// Installed language codes (the base names of *.traineddata), sorted.
QStringList installedLanguages();

// Friendly label for a Tesseract language code (for example, "English" for
// "eng"). Unknown codes are returned unchanged.
QString languageName(const QString &code);

// Open the tessdata folder in Explorer.
void openFolder();

// Open the official best-quality model repository in the default browser.
void openRepository();

// The tessdata_best repository URL (shown / opened in the UI).
QString repositoryUrl();

} // namespace TessdataManager

} // namespace mervin
