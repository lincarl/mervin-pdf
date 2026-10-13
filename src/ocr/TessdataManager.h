#pragma once

#include <QByteArray>
#include <QList>
#include <QString>
#include <QStringList>

#include <functional>

class QObject;

namespace mervin {

// Locates the writable Tesseract language-data folder in the user profile, lists
// installed models, and points the user at the official repository. Downloads
// and manually supplied .traineddata files share this folder.
namespace TessdataManager {

// The tessdata directory in the profile. It may not exist yet: whatever writes
// to it creates it.
QString directory();

// An installed model: its language code (the base name of its .traineddata file)
// and the file's size.
struct Model
{
    QString code;
    qint64 bytes = 0;
};

// The installed models in `dir`, sorted by code. Reads the folder, so the UI thread
// uses listModels() instead.
QList<Model> installedModels(const QString &dir = directory());

// The calls below work on the folder on one background thread, in the order asked,
// as the profile can be on a share that stopped answering. Each hands its result,
// with the models installed afterwards, to `done` on the UI thread unless
// `context` is gone by then.
void listModels(QObject *context, std::function<void(const QList<Model> &models)> done);
// Removes `code`'s model. `removed` is false when the file could not be removed.
void removeModel(const QString &code, QObject *context,
                 std::function<void(bool removed, const QList<Model> &models)> done);
// How saveModel() went.
enum class Saved
{
    Ok,
    Failed,  // the file could not be written
    Invalid, // the data is not a usable model, so it was removed; `detail` says why
};
// Writes `data` as `code`'s model, creating the folder, and checks it.
void saveModel(const QString &code, const QByteArray &data, QObject *context,
               std::function<void(Saved saved, const QString &detail,
                                  const QList<Model> &models)> done);
// Runs `work` on the same thread after the calls asked for before, then `done` on
// the UI thread unless `context` is gone, for another writer to the folder such as
// the first-run download.
void runInFolder(std::function<void()> work, QObject *context = nullptr,
                 std::function<void()> done = {});
// For tests: a call or work asked for has not finished, or has not handed over its
// result yet.
bool busy();

// Friendly label for a Tesseract language code in the UI language (for
// example, "English" for "eng", "Engelska" in Swedish). Codes Mervin has no
// name for get QLocale's English name, or are returned unchanged.
QString languageName(const QString &code);

// The same label in English whatever the UI language, so a search can match
// either name.
QString englishLanguageName(const QString &code);

// Creates the tessdata folder if it is missing, then opens it in the file manager.
void openFolder();

// Open the official best-quality model repository in the default browser.
void openRepository();

// The tessdata_best repository URL (shown / opened in the UI).
QString repositoryUrl();

} // namespace TessdataManager

} // namespace mervin
