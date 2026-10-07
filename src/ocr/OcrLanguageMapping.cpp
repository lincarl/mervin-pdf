#include "ocr/OcrLanguageMapping.h"

#include <QLocale>

namespace mervin::ocr {
namespace {

struct LanguageModel
{
    const char *language;
    const char *script;
    const char *model;
};

// Fixed matches to the official tessdata_best language files. The table covers
// the app's UI languages and other supported OS languages. Montenegrin (cnr)
// and Romansh (rm) have no matching file. Script variants need their own match.
constexpr LanguageModel kModels[] = {
    {"af", "Latn", "afr"},
    {"am", "Ethi", "amh"},
    {"ar", "Arab", "ara"},
    {"as", "Beng", "asm"},
    {"az", "Latn", "aze"},
    {"az", "Cyrl", "aze_cyrl"},
    {"be", "Cyrl", "bel"},
    {"bg", "Cyrl", "bul"},
    {"bn", "Beng", "ben"},
    {"bo", "Tibt", "bod"},
    {"br", "Latn", "bre"},
    {"bs", "Latn", "bos"},
    {"ca", "Latn", "cat"},
    {"ceb", "Latn", "ceb"},
    {"chr", "Cher", "chr"},
    {"co", "Latn", "cos"},
    {"cs", "Latn", "ces"},
    {"cy", "Latn", "cym"},
    {"da", "Latn", "dan"},
    {"de", "Latn", "deu"},
    {"dv", "Thaa", "div"},
    {"dz", "Tibt", "dzo"},
    {"el", "Grek", "ell"},
    {"en", "Latn", "eng"},
    {"eo", "Latn", "epo"},
    {"es", "Latn", "spa"},
    {"et", "Latn", "est"},
    {"eu", "Latn", "eus"},
    {"fa", "Arab", "fas"},
    {"fi", "Latn", "fin"},
    {"fil", "Latn", "fil"},
    {"fo", "Latn", "fao"},
    {"fr", "Latn", "fra"},
    {"fy", "Latn", "fry"},
    {"ga", "Latn", "gle"},
    {"gd", "Latn", "gla"},
    {"gl", "Latn", "glg"},
    {"gu", "Gujr", "guj"},
    {"he", "Hebr", "heb"},
    {"hi", "Deva", "hin"},
    {"hr", "Latn", "hrv"},
    {"ht", "Latn", "hat"},
    {"hu", "Latn", "hun"},
    {"hy", "Armn", "hye"},
    {"id", "Latn", "ind"},
    {"is", "Latn", "isl"},
    {"it", "Latn", "ita"},
    {"iu", "Cans", "iku"},
    {"ja", "Jpan", "jpn"},
    {"jv", "Latn", "jav"},
    {"ka", "Geor", "kat"},
    {"kk", "Cyrl", "kaz"},
    {"km", "Khmr", "khm"},
    {"kn", "Knda", "kan"},
    {"ko", "Kore", "kor"},
    {"ku", "Latn", "kmr"},
    {"ky", "Cyrl", "kir"},
    {"la", "Latn", "lat"},
    {"lb", "Latn", "ltz"},
    {"lo", "Laoo", "lao"},
    {"lt", "Latn", "lit"},
    {"lv", "Latn", "lav"},
    {"mi", "Latn", "mri"},
    {"mk", "Cyrl", "mkd"},
    {"ml", "Mlym", "mal"},
    {"mn", "Cyrl", "mon"},
    {"mr", "Deva", "mar"},
    {"ms", "Latn", "msa"},
    {"mt", "Latn", "mlt"},
    {"my", "Mymr", "mya"},
    {"nb", "Latn", "nor"},
    {"ne", "Deva", "nep"},
    {"nl", "Latn", "nld"},
    {"nn", "Latn", "nor"},
    {"oc", "Latn", "oci"},
    {"or", "Orya", "ori"},
    {"pa", "Guru", "pan"},
    {"pl", "Latn", "pol"},
    {"ps", "Arab", "pus"},
    {"pt", "Latn", "por"},
    {"qu", "Latn", "que"},
    {"ro", "Latn", "ron"},
    {"ru", "Cyrl", "rus"},
    {"sa", "Deva", "san"},
    {"sd", "Arab", "snd"},
    {"si", "Sinh", "sin"},
    {"sk", "Latn", "slk"},
    {"sl", "Latn", "slv"},
    {"sq", "Latn", "sqi"},
    {"sr", "Cyrl", "srp"},
    {"sr", "Latn", "srp_latn"},
    {"su", "Latn", "sun"},
    {"sv", "Latn", "swe"},
    {"sw", "Latn", "swa"},
    {"syr", "Syrc", "syr"},
    {"ta", "Taml", "tam"},
    {"te", "Telu", "tel"},
    {"tg", "Cyrl", "tgk"},
    {"th", "Thai", "tha"},
    {"ti", "Ethi", "tir"},
    {"to", "Latn", "ton"},
    {"tr", "Latn", "tur"},
    {"tt", "Cyrl", "tat"},
    {"ug", "Arab", "uig"},
    {"uk", "Cyrl", "ukr"},
    {"ur", "Arab", "urd"},
    {"uz", "Latn", "uzb"},
    {"uz", "Cyrl", "uzb_cyrl"},
    {"vi", "Latn", "vie"},
    {"yi", "Hebr", "yid"},
    {"yo", "Latn", "yor"},
    {"zh", "Hans", "chi_sim"},
    {"zh", "Hant", "chi_tra"},
};

} // namespace

QString modelForLanguage(const QString &localeTag)
{
    QString normalized = localeTag.trimmed();
    normalized.replace(u'_', u'-');
    const QStringList parts = normalized.split(u'-');
    QString languageCode = parts.first().toLower();
    // Do not let Qt's nearest locale turn an unsupported language into another
    // language. Normalize only known ISO language codes, never the system default.
    if (languageCode == QStringLiteral("cnr") || languageCode.isEmpty())
        return {};
    if (languageCode == QStringLiteral("no"))
        languageCode = QStringLiteral("nb");
    const auto language = QLocale::codeToLanguage(languageCode);
    if (language == QLocale::AnyLanguage || language == QLocale::C)
        return {};
    languageCode = QLocale::languageToCode(language, QLocale::ISO639Part1);
    if (languageCode.isEmpty())
        languageCode = QLocale::languageToCode(language, QLocale::ISO639Part3);

    QString script;
    if (parts.size() > 1 && parts.at(1).size() == 4)
        script = parts.at(1);
    else
        script = QLocale::scriptToCode(QLocale(normalized).script());

    for (const auto &match : kModels) {
        if (languageCode == QLatin1String(match.language)
            && script.compare(QLatin1String(match.script), Qt::CaseInsensitive) == 0)
            return QString::fromLatin1(match.model);
    }
    return {};
}

QStringList initialModels(const QString &selectedLanguage, const QString &osLanguage)
{
    QStringList result;
    for (const QString &locale : {selectedLanguage, osLanguage}) {
        const QString model = modelForLanguage(locale);
        if (!model.isEmpty() && !result.contains(model))
            result.append(model);
    }
    return result;
}

} // namespace mervin::ocr
