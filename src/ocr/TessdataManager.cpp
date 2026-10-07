#include "ocr/TessdataManager.h"

#include "config/ConfigPaths.h"

#include <QCoreApplication>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLocale>
#include <QUrl>

namespace mervin {

namespace {

#ifndef Q_OS_WIN
// Read-only locations that may ship language data with the app: the bundle next
// to the executable, an AppImage ($APPDIR) or snap ($SNAP) mount, and the system
// package dir used by the .deb/.rpm. These are searched only to seed the writable
// per-user dir on first run (on Windows the installer seeds %APPDATA% instead).
QStringList bundledTessdataDirs()
{
    QStringList dirs;
    const QString appDir = QCoreApplication::applicationDirPath();
    dirs << QDir(appDir).filePath(QStringLiteral("../share/mervin-pdf/tessdata"));
    if (const QString snap = qEnvironmentVariable("SNAP"); !snap.isEmpty())
        dirs << QDir(snap).filePath(QStringLiteral("usr/share/mervin-pdf/tessdata"));
    if (const QString appimg = qEnvironmentVariable("APPDIR"); !appimg.isEmpty())
        dirs << QDir(appimg).filePath(QStringLiteral("usr/share/mervin-pdf/tessdata"));
    dirs << QStringLiteral("/usr/share/mervin-pdf/tessdata");
    dirs << QStringLiteral("/usr/local/share/mervin-pdf/tessdata");
    return dirs;
}

// First run only (no *.traineddata yet in the writable dir): copy whatever the
// app bundle / system package shipped so OCR works out of the box. Keeps the
// single-datadir contract - callers still pass directory() to the OCR engine,
// and the user can drop more languages into that same writable folder.
void seedFromBundleIfEmpty(const QString &writableDir)
{
    QDir wdir(writableDir);
    if (!wdir.entryList({QStringLiteral("*.traineddata")}, QDir::Files).isEmpty())
        return; // already has at least one language
    for (const QString &cand : bundledTessdataDirs()) {
        QDir src(cand);
        if (!src.exists())
            continue;
        const QStringList langs = src.entryList({QStringLiteral("*.traineddata")}, QDir::Files);
        if (langs.isEmpty())
            continue;
        for (const QString &f : langs)
            QFile::copy(src.filePath(f), wdir.filePath(f));
        return; // first bundle that has data wins
    }
}
#endif

// English names for Tesseract model codes, marked for translation. QLocale can
// name most of these languages, but only in English. A code without an entry
// falls back to QLocale's English name, or to the code itself.
struct ModelName
{
    const char *code;
    const char *name;
};

constexpr ModelName kModelNames[] = {
    //: OCR language model name.
    {"chi_sim", QT_TRANSLATE_NOOP("mervin::TessdataManager", "Chinese (Simplified)")},
    //: OCR language model name.
    {"chi_tra", QT_TRANSLATE_NOOP("mervin::TessdataManager", "Chinese (Traditional)")},
    //: OCR language model name. Fraktur is the German blackletter typeface.
    {"deu_frak", QT_TRANSLATE_NOOP("mervin::TessdataManager", "German Fraktur")},
    //: OCR model name. The model finds math formulas instead of reading a language.
    {"equ", QT_TRANSLATE_NOOP("mervin::TessdataManager", "Math / equation detection")},
    //: OCR model name. The model finds page orientation and writing system instead
    //: of reading a language.
    {"osd", QT_TRANSLATE_NOOP("mervin::TessdataManager", "Orientation and script detection")},
    // The tessdata_best languages, with the English names QLocale gives them.
    //: OCR language name.
    {"afr", QT_TRANSLATE_NOOP("mervin::TessdataManager", "Afrikaans")},
    //: OCR language name.
    {"amh", QT_TRANSLATE_NOOP("mervin::TessdataManager", "Amharic")},
    //: OCR language name.
    {"ara", QT_TRANSLATE_NOOP("mervin::TessdataManager", "Arabic")},
    //: OCR language name.
    {"asm", QT_TRANSLATE_NOOP("mervin::TessdataManager", "Assamese")},
    //: OCR language name.
    {"aze", QT_TRANSLATE_NOOP("mervin::TessdataManager", "Azerbaijani")},
    //: OCR language name.
    {"bel", QT_TRANSLATE_NOOP("mervin::TessdataManager", "Belarusian")},
    //: OCR language name.
    {"ben", QT_TRANSLATE_NOOP("mervin::TessdataManager", "Bangla")},
    //: OCR language name.
    {"bod", QT_TRANSLATE_NOOP("mervin::TessdataManager", "Tibetan")},
    //: OCR language name.
    {"bos", QT_TRANSLATE_NOOP("mervin::TessdataManager", "Bosnian")},
    //: OCR language name.
    {"bre", QT_TRANSLATE_NOOP("mervin::TessdataManager", "Breton")},
    //: OCR language name.
    {"bul", QT_TRANSLATE_NOOP("mervin::TessdataManager", "Bulgarian")},
    //: OCR language name.
    {"cat", QT_TRANSLATE_NOOP("mervin::TessdataManager", "Catalan")},
    //: OCR language name.
    {"ceb", QT_TRANSLATE_NOOP("mervin::TessdataManager", "Cebuano")},
    //: OCR language name.
    {"ces", QT_TRANSLATE_NOOP("mervin::TessdataManager", "Czech")},
    //: OCR language name.
    {"chr", QT_TRANSLATE_NOOP("mervin::TessdataManager", "Cherokee")},
    //: OCR language name.
    {"cos", QT_TRANSLATE_NOOP("mervin::TessdataManager", "Corsican")},
    //: OCR language name.
    {"cym", QT_TRANSLATE_NOOP("mervin::TessdataManager", "Welsh")},
    //: OCR language name.
    {"dan", QT_TRANSLATE_NOOP("mervin::TessdataManager", "Danish")},
    //: OCR language name.
    {"deu", QT_TRANSLATE_NOOP("mervin::TessdataManager", "German")},
    //: OCR language name.
    {"div", QT_TRANSLATE_NOOP("mervin::TessdataManager", "Divehi")},
    //: OCR language name.
    {"dzo", QT_TRANSLATE_NOOP("mervin::TessdataManager", "Dzongkha")},
    //: OCR language name.
    {"ell", QT_TRANSLATE_NOOP("mervin::TessdataManager", "Greek")},
    //: OCR language name.
    {"eng", QT_TRANSLATE_NOOP("mervin::TessdataManager", "English")},
    //: OCR language name.
    {"epo", QT_TRANSLATE_NOOP("mervin::TessdataManager", "Esperanto")},
    //: OCR language name.
    {"est", QT_TRANSLATE_NOOP("mervin::TessdataManager", "Estonian")},
    //: OCR language name.
    {"eus", QT_TRANSLATE_NOOP("mervin::TessdataManager", "Basque")},
    //: OCR language name.
    {"fao", QT_TRANSLATE_NOOP("mervin::TessdataManager", "Faroese")},
    //: OCR language name.
    {"fas", QT_TRANSLATE_NOOP("mervin::TessdataManager", "Persian")},
    //: OCR language name.
    {"fil", QT_TRANSLATE_NOOP("mervin::TessdataManager", "Filipino")},
    //: OCR language name.
    {"fin", QT_TRANSLATE_NOOP("mervin::TessdataManager", "Finnish")},
    //: OCR language name.
    {"fra", QT_TRANSLATE_NOOP("mervin::TessdataManager", "French")},
    //: OCR language name.
    {"fry", QT_TRANSLATE_NOOP("mervin::TessdataManager", "Western Frisian")},
    //: OCR language name.
    {"gla", QT_TRANSLATE_NOOP("mervin::TessdataManager", "Scottish Gaelic")},
    //: OCR language name.
    {"gle", QT_TRANSLATE_NOOP("mervin::TessdataManager", "Irish")},
    //: OCR language name.
    {"glg", QT_TRANSLATE_NOOP("mervin::TessdataManager", "Galician")},
    //: OCR language name.
    {"grc", QT_TRANSLATE_NOOP("mervin::TessdataManager", "Ancient Greek")},
    //: OCR language name.
    {"guj", QT_TRANSLATE_NOOP("mervin::TessdataManager", "Gujarati")},
    //: OCR language name.
    {"hat", QT_TRANSLATE_NOOP("mervin::TessdataManager", "Haitian Creole")},
    //: OCR language name.
    {"heb", QT_TRANSLATE_NOOP("mervin::TessdataManager", "Hebrew")},
    //: OCR language name.
    {"hin", QT_TRANSLATE_NOOP("mervin::TessdataManager", "Hindi")},
    //: OCR language name.
    {"hrv", QT_TRANSLATE_NOOP("mervin::TessdataManager", "Croatian")},
    //: OCR language name.
    {"hun", QT_TRANSLATE_NOOP("mervin::TessdataManager", "Hungarian")},
    //: OCR language name.
    {"hye", QT_TRANSLATE_NOOP("mervin::TessdataManager", "Armenian")},
    //: OCR language name.
    {"iku", QT_TRANSLATE_NOOP("mervin::TessdataManager", "Inuktitut")},
    //: OCR language name.
    {"ind", QT_TRANSLATE_NOOP("mervin::TessdataManager", "Indonesian")},
    //: OCR language name.
    {"isl", QT_TRANSLATE_NOOP("mervin::TessdataManager", "Icelandic")},
    //: OCR language name.
    {"ita", QT_TRANSLATE_NOOP("mervin::TessdataManager", "Italian")},
    //: OCR language name.
    {"jav", QT_TRANSLATE_NOOP("mervin::TessdataManager", "Javanese")},
    //: OCR language name.
    {"jpn", QT_TRANSLATE_NOOP("mervin::TessdataManager", "Japanese")},
    //: OCR language name.
    {"kan", QT_TRANSLATE_NOOP("mervin::TessdataManager", "Kannada")},
    //: OCR language name.
    {"kat", QT_TRANSLATE_NOOP("mervin::TessdataManager", "Georgian")},
    //: OCR language name.
    {"kaz", QT_TRANSLATE_NOOP("mervin::TessdataManager", "Kazakh")},
    //: OCR language name.
    {"khm", QT_TRANSLATE_NOOP("mervin::TessdataManager", "Khmer")},
    //: OCR language name.
    {"kir", QT_TRANSLATE_NOOP("mervin::TessdataManager", "Kyrgyz")},
    //: OCR language name.
    {"kor", QT_TRANSLATE_NOOP("mervin::TessdataManager", "Korean")},
    //: OCR language name.
    {"lao", QT_TRANSLATE_NOOP("mervin::TessdataManager", "Lao")},
    //: OCR language name.
    {"lat", QT_TRANSLATE_NOOP("mervin::TessdataManager", "Latin")},
    //: OCR language name.
    {"lav", QT_TRANSLATE_NOOP("mervin::TessdataManager", "Latvian")},
    //: OCR language name.
    {"lit", QT_TRANSLATE_NOOP("mervin::TessdataManager", "Lithuanian")},
    //: OCR language name.
    {"ltz", QT_TRANSLATE_NOOP("mervin::TessdataManager", "Luxembourgish")},
    //: OCR language name.
    {"mal", QT_TRANSLATE_NOOP("mervin::TessdataManager", "Malayalam")},
    //: OCR language name.
    {"mar", QT_TRANSLATE_NOOP("mervin::TessdataManager", "Marathi")},
    //: OCR language name.
    {"mkd", QT_TRANSLATE_NOOP("mervin::TessdataManager", "Macedonian")},
    //: OCR language name.
    {"mlt", QT_TRANSLATE_NOOP("mervin::TessdataManager", "Maltese")},
    //: OCR language name.
    {"mon", QT_TRANSLATE_NOOP("mervin::TessdataManager", "Mongolian")},
    //: OCR language name.
    {"mri", QT_TRANSLATE_NOOP("mervin::TessdataManager", "Māori")},
    //: OCR language name.
    {"msa", QT_TRANSLATE_NOOP("mervin::TessdataManager", "Malay")},
    //: OCR language name.
    {"mya", QT_TRANSLATE_NOOP("mervin::TessdataManager", "Burmese")},
    //: OCR language name.
    {"nep", QT_TRANSLATE_NOOP("mervin::TessdataManager", "Nepali")},
    //: OCR language name.
    {"nld", QT_TRANSLATE_NOOP("mervin::TessdataManager", "Dutch")},
    //: OCR language name.
    {"oci", QT_TRANSLATE_NOOP("mervin::TessdataManager", "Occitan")},
    //: OCR language name.
    {"ori", QT_TRANSLATE_NOOP("mervin::TessdataManager", "Odia")},
    //: OCR language name.
    {"pan", QT_TRANSLATE_NOOP("mervin::TessdataManager", "Punjabi")},
    //: OCR language name.
    {"pol", QT_TRANSLATE_NOOP("mervin::TessdataManager", "Polish")},
    //: OCR language name.
    {"por", QT_TRANSLATE_NOOP("mervin::TessdataManager", "Portuguese")},
    //: OCR language name.
    {"pus", QT_TRANSLATE_NOOP("mervin::TessdataManager", "Pashto")},
    //: OCR language name.
    {"que", QT_TRANSLATE_NOOP("mervin::TessdataManager", "Quechua")},
    //: OCR language name.
    {"ron", QT_TRANSLATE_NOOP("mervin::TessdataManager", "Romanian")},
    //: OCR language name.
    {"rus", QT_TRANSLATE_NOOP("mervin::TessdataManager", "Russian")},
    //: OCR language name.
    {"san", QT_TRANSLATE_NOOP("mervin::TessdataManager", "Sanskrit")},
    //: OCR language name.
    {"sin", QT_TRANSLATE_NOOP("mervin::TessdataManager", "Sinhala")},
    //: OCR language name.
    {"slk", QT_TRANSLATE_NOOP("mervin::TessdataManager", "Slovak")},
    //: OCR language name.
    {"slv", QT_TRANSLATE_NOOP("mervin::TessdataManager", "Slovenian")},
    //: OCR language name.
    {"snd", QT_TRANSLATE_NOOP("mervin::TessdataManager", "Sindhi")},
    //: OCR language name.
    {"spa", QT_TRANSLATE_NOOP("mervin::TessdataManager", "Spanish")},
    //: OCR language name.
    {"sqi", QT_TRANSLATE_NOOP("mervin::TessdataManager", "Albanian")},
    //: OCR language name.
    {"srp", QT_TRANSLATE_NOOP("mervin::TessdataManager", "Serbian")},
    //: OCR language name.
    {"sun", QT_TRANSLATE_NOOP("mervin::TessdataManager", "Sundanese")},
    //: OCR language name.
    {"swa", QT_TRANSLATE_NOOP("mervin::TessdataManager", "Swahili")},
    //: OCR language name.
    {"swe", QT_TRANSLATE_NOOP("mervin::TessdataManager", "Swedish")},
    //: OCR language name.
    {"syr", QT_TRANSLATE_NOOP("mervin::TessdataManager", "Syriac")},
    //: OCR language name.
    {"tam", QT_TRANSLATE_NOOP("mervin::TessdataManager", "Tamil")},
    //: OCR language name.
    {"tat", QT_TRANSLATE_NOOP("mervin::TessdataManager", "Tatar")},
    //: OCR language name.
    {"tel", QT_TRANSLATE_NOOP("mervin::TessdataManager", "Telugu")},
    //: OCR language name.
    {"tgk", QT_TRANSLATE_NOOP("mervin::TessdataManager", "Tajik")},
    //: OCR language name.
    {"tha", QT_TRANSLATE_NOOP("mervin::TessdataManager", "Thai")},
    //: OCR language name.
    {"tir", QT_TRANSLATE_NOOP("mervin::TessdataManager", "Tigrinya")},
    //: OCR language name.
    {"ton", QT_TRANSLATE_NOOP("mervin::TessdataManager", "Tongan")},
    //: OCR language name.
    {"tur", QT_TRANSLATE_NOOP("mervin::TessdataManager", "Turkish")},
    //: OCR language name.
    {"uig", QT_TRANSLATE_NOOP("mervin::TessdataManager", "Uyghur")},
    //: OCR language name.
    {"ukr", QT_TRANSLATE_NOOP("mervin::TessdataManager", "Ukrainian")},
    //: OCR language name.
    {"urd", QT_TRANSLATE_NOOP("mervin::TessdataManager", "Urdu")},
    //: OCR language name.
    {"uzb", QT_TRANSLATE_NOOP("mervin::TessdataManager", "Uzbek")},
    //: OCR language name.
    {"vie", QT_TRANSLATE_NOOP("mervin::TessdataManager", "Vietnamese")},
    //: OCR language name.
    {"yid", QT_TRANSLATE_NOOP("mervin::TessdataManager", "Yiddish")},
    //: OCR language name.
    {"yor", QT_TRANSLATE_NOOP("mervin::TessdataManager", "Yoruba")},
};

} // namespace

QString TessdataManager::directory()
{
    const QString dir = QDir(ConfigPaths::configDir()).filePath(QStringLiteral("tessdata"));
    QDir().mkpath(dir);
#ifndef Q_OS_WIN
    seedFromBundleIfEmpty(dir);
#endif
    return dir;
}

QStringList TessdataManager::installedLanguages()
{
    QDir dir(directory());
    QStringList langs;
    for (const QString &f : dir.entryList({QStringLiteral("*.traineddata")}, QDir::Files, QDir::Name))
        langs << QFileInfo(f).completeBaseName();
    langs.sort();
    return langs;
}

QString TessdataManager::languageName(const QString &code)
{
    for (const ModelName &model : kModelNames) {
        if (code == QLatin1String(model.code))
            return QCoreApplication::translate("mervin::TessdataManager", model.name);
    }
    return englishLanguageName(code);
}

QString TessdataManager::englishLanguageName(const QString &code)
{
    for (const ModelName &model : kModelNames) {
        if (code == QLatin1String(model.code))
            return QString::fromUtf8(model.name);
    }
    const QLocale::Language language = QLocale::codeToLanguage(code);
    return language == QLocale::AnyLanguage || language == QLocale::C
        ? code
        : QLocale::languageToString(language);
}

void TessdataManager::openFolder()
{
    QDesktopServices::openUrl(QUrl::fromLocalFile(directory()));
}

void TessdataManager::openRepository()
{
    QDesktopServices::openUrl(QUrl(repositoryUrl()));
}

QString TessdataManager::repositoryUrl()
{
    return QStringLiteral("https://github.com/tesseract-ocr/tessdata_best");
}

} // namespace mervin
