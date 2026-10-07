#include "i18n/UiLanguage.h"

#include <QCoreApplication>
#include <QDir>
#include <QFont>
#include <QFontDatabase>
#include <QGuiApplication>
#include <QHash>
#include <QLocale>
#include <QPointer>
#include <QTranslator>

#include <algorithm>

namespace mervin::i18n {

namespace {

// Names for the languages that ship. The native name is fixed text, because
// QLocale's own names are uneven ("American English", lower-case "svenska").
// The English name is translated, so the picker's search also finds the name in
// the UI language.
struct LanguageNames
{
    const char *code;
    const char *native; // UTF-8
    const char *english;
};

constexpr LanguageNames kNames[] = {
    //: Language name. The language picker's search finds a language by this name too.
    {"en", "English", QT_TRANSLATE_NOOP("UiLanguage", "English")},
    //: Language name. The language picker's search finds a language by this name too.
    {"ar", "العربية", QT_TRANSLATE_NOOP("UiLanguage", "Arabic")},
    //: Language name. The language picker's search finds a language by this name too.
    {"az", "Azərbaycanca", QT_TRANSLATE_NOOP("UiLanguage", "Azerbaijani")},
    //: Language name. The language picker's search finds a language by this name too.
    {"be", "Беларуская", QT_TRANSLATE_NOOP("UiLanguage", "Belarusian")},
    //: Language name. The language picker's search finds a language by this name too.
    {"bg", "Български", QT_TRANSLATE_NOOP("UiLanguage", "Bulgarian")},
    //: Language name. The language picker's search finds a language by this name too.
    {"bs", "Bosanski", QT_TRANSLATE_NOOP("UiLanguage", "Bosnian")},
    //: Language name. The language picker's search finds a language by this name too.
    {"ca", "Català", QT_TRANSLATE_NOOP("UiLanguage", "Catalan")},
    //: Language name. The language picker's search finds a language by this name too.
    {"cnr", "Crnogorski", QT_TRANSLATE_NOOP("UiLanguage", "Montenegrin")},
    //: Language name. The language picker's search finds a language by this name too.
    {"cs", "Čeština", QT_TRANSLATE_NOOP("UiLanguage", "Czech")},
    //: Language name. The language picker's search finds a language by this name too.
    {"da", "Dansk", QT_TRANSLATE_NOOP("UiLanguage", "Danish")},
    //: Language name. The language picker's search finds a language by this name too.
    {"de", "Deutsch", QT_TRANSLATE_NOOP("UiLanguage", "German")},
    //: Language name. The language picker's search finds a language by this name too.
    {"el", "Ελληνικά", QT_TRANSLATE_NOOP("UiLanguage", "Greek")},
    //: Language name. The language picker's search finds a language by this name too.
    {"es", "Español", QT_TRANSLATE_NOOP("UiLanguage", "Spanish")},
    //: Language name. The language picker's search finds a language by this name too.
    {"et", "Eesti", QT_TRANSLATE_NOOP("UiLanguage", "Estonian")},
    //: Language name. The language picker's search finds a language by this name too.
    {"fi", "Suomi", QT_TRANSLATE_NOOP("UiLanguage", "Finnish")},
    //: Language name. The language picker's search finds a language by this name too.
    {"fr", "Français", QT_TRANSLATE_NOOP("UiLanguage", "French")},
    //: Language name. The language picker's search finds a language by this name too.
    {"ga", "Gaeilge", QT_TRANSLATE_NOOP("UiLanguage", "Irish")},
    //: Language name. The language picker's search finds a language by this name too.
    {"hi", "हिन्दी", QT_TRANSLATE_NOOP("UiLanguage", "Hindi")},
    //: Language name. The language picker's search finds a language by this name too.
    {"hr", "Hrvatski", QT_TRANSLATE_NOOP("UiLanguage", "Croatian")},
    //: Language name. The language picker's search finds a language by this name too.
    {"hu", "Magyar", QT_TRANSLATE_NOOP("UiLanguage", "Hungarian")},
    //: Language name. The language picker's search finds a language by this name too.
    {"hy", "Հայերեն", QT_TRANSLATE_NOOP("UiLanguage", "Armenian")},
    //: Language name. The language picker's search finds a language by this name too.
    {"id", "Bahasa Indonesia", QT_TRANSLATE_NOOP("UiLanguage", "Indonesian")},
    //: Language name. The language picker's search finds a language by this name too.
    {"is", "Íslenska", QT_TRANSLATE_NOOP("UiLanguage", "Icelandic")},
    //: Language name. The language picker's search finds a language by this name too.
    {"it", "Italiano", QT_TRANSLATE_NOOP("UiLanguage", "Italian")},
    //: Language name. The language picker's search finds a language by this name too.
    {"ja", "日本語", QT_TRANSLATE_NOOP("UiLanguage", "Japanese")},
    //: Language name. The language picker's search finds a language by this name too.
    {"ka", "ქართული", QT_TRANSLATE_NOOP("UiLanguage", "Georgian")},
    //: Language name. The language picker's search finds a language by this name too.
    {"kk", "Қазақша", QT_TRANSLATE_NOOP("UiLanguage", "Kazakh")},
    //: Language name. The language picker's search finds a language by this name too.
    {"ko", "한국어", QT_TRANSLATE_NOOP("UiLanguage", "Korean")},
    //: Language name. The language picker's search finds a language by this name too.
    {"lb", "Lëtzebuergesch", QT_TRANSLATE_NOOP("UiLanguage", "Luxembourgish")},
    //: Language name. The language picker's search finds a language by this name too.
    {"lt", "Lietuvių", QT_TRANSLATE_NOOP("UiLanguage", "Lithuanian")},
    //: Language name. The language picker's search finds a language by this name too.
    {"lv", "Latviešu", QT_TRANSLATE_NOOP("UiLanguage", "Latvian")},
    //: Language name. The language picker's search finds a language by this name too.
    {"mk", "Македонски", QT_TRANSLATE_NOOP("UiLanguage", "Macedonian")},
    //: Language name. The language picker's search finds a language by this name too.
    {"mt", "Malti", QT_TRANSLATE_NOOP("UiLanguage", "Maltese")},
    //: Language name. The language picker's search finds a language by this name too.
    {"nb", "Norsk bokmål", QT_TRANSLATE_NOOP("UiLanguage", "Norwegian Bokmål")},
    //: Language name. The language picker's search finds a language by this name too.
    {"nl", "Nederlands", QT_TRANSLATE_NOOP("UiLanguage", "Dutch")},
    //: Language name. The language picker's search finds a language by this name too.
    {"nn", "Norsk nynorsk", QT_TRANSLATE_NOOP("UiLanguage", "Norwegian Nynorsk")},
    //: Language name. The language picker's search finds a language by this name too.
    {"pl", "Polski", QT_TRANSLATE_NOOP("UiLanguage", "Polish")},
    //: Language name. The language picker's search finds a language by this name too.
    {"pt_BR", "Português do Brasil", QT_TRANSLATE_NOOP("UiLanguage", "Portuguese, Brazil")},
    //: Language name. The language picker's search finds a language by this name too.
    {"pt_PT", "Português de Portugal", QT_TRANSLATE_NOOP("UiLanguage", "Portuguese, Portugal")},
    //: Language name. The language picker's search finds a language by this name too.
    {"rm", "Rumantsch", QT_TRANSLATE_NOOP("UiLanguage", "Romansh")},
    //: Language name. The language picker's search finds a language by this name too.
    {"ro", "Română", QT_TRANSLATE_NOOP("UiLanguage", "Romanian")},
    //: Language name. The language picker's search finds a language by this name too.
    {"ru", "Русский", QT_TRANSLATE_NOOP("UiLanguage", "Russian")},
    //: Language name. The language picker's search finds a language by this name too.
    {"sk", "Slovenčina", QT_TRANSLATE_NOOP("UiLanguage", "Slovak")},
    //: Language name. The language picker's search finds a language by this name too.
    {"sl", "Slovenščina", QT_TRANSLATE_NOOP("UiLanguage", "Slovenian")},
    //: Language name. The language picker's search finds a language by this name too.
    {"sq", "Shqip", QT_TRANSLATE_NOOP("UiLanguage", "Albanian")},
    //: Language name. The language picker's search finds a language by this name too.
    {"sr", "Српски", QT_TRANSLATE_NOOP("UiLanguage", "Serbian")},
    //: Language name. The language picker's search finds a language by this name too.
    {"sv", "Svenska", QT_TRANSLATE_NOOP("UiLanguage", "Swedish")},
    //: Language name. The language picker's search finds a language by this name too.
    {"th", "ไทย", QT_TRANSLATE_NOOP("UiLanguage", "Thai")},
    //: Language name. The language picker's search finds a language by this name too.
    {"tr", "Türkçe", QT_TRANSLATE_NOOP("UiLanguage", "Turkish")},
    //: Language name. The language picker's search finds a language by this name too.
    {"uk", "Українська", QT_TRANSLATE_NOOP("UiLanguage", "Ukrainian")},
    //: Language name. The language picker's search finds a language by this name too.
    {"vi", "Tiếng Việt", QT_TRANSLATE_NOOP("UiLanguage", "Vietnamese")},
    //: Language name. The language picker's search finds a language by this name too.
    {"zh_CN", "简体中文", QT_TRANSLATE_NOOP("UiLanguage", "Chinese, Simplified")},
    //: Language name. The language picker's search finds a language by this name too.
    {"zh_TW", "繁體中文", QT_TRANSLATE_NOOP("UiLanguage", "Chinese, Traditional")},
};

const LanguageNames *namesFor(const QString &code)
{
    for (const LanguageNames &names : kNames)
        if (code == QLatin1String(names.code))
            return &names;
    return nullptr;
}

QString nativeName(const QString &code)
{
    if (const LanguageNames *names = namesFor(code))
        return QString::fromUtf8(names->native);
    // A catalog added without a names entry still gets a readable label.
    QString name = QLocale(code).nativeLanguageName();
    if (!name.isEmpty())
        name[0] = name[0].toUpper();
    return name.isEmpty() ? code : name;
}

QString englishName(const QString &code)
{
    if (const LanguageNames *names = namesFor(code))
        return QString::fromLatin1(names->english);
    return QLocale::languageToString(QLocale(code).language());
}

QString &currentLanguage()
{
    static QString language = QString::fromLatin1(kEnglish);
    return language;
}

// Han glyph shapes differ between Chinese, Japanese and Korean. Prefer a
// font for the selected UI language, including script-neutral punctuation
// that Qt may shape as part of a Latin run. Remove our previous fallback
// before switching so one language cannot change another language's glyphs.
// The primary font also supplies script-appropriate heights for controls that
// cannot size taller fallback text from their original Latin font metrics.
void preferLanguageFont(const QString &language)
{
    static QString added;
    static QList<QChar::Script> addedScripts;
    static QStringList originalFamilies;
    static QStringList selectedFamilies;
    QFont font = QGuiApplication::font();
    if (!selectedFamilies.isEmpty() && font.families() == selectedFamilies)
        font.setFamilies(originalFamilies);
    originalFamilies = font.families();
    selectedFamilies.clear();
    if (!added.isEmpty()) {
        for (QChar::Script script : addedScripts)
            QFontDatabase::removeApplicationFallbackFontFamily(script, added);
        added.clear();
        addedScripts.clear();
    }

    QStringList families;
    QList<QChar::Script> scripts{QChar::Script_Han, QChar::Script_Common};
    if (language == QLatin1String("zh_CN")) {
        families = {QStringLiteral("Microsoft YaHei UI"), QStringLiteral("Microsoft YaHei"),
                    QStringLiteral("Noto Sans CJK SC"), QStringLiteral("Source Han Sans SC"),
                    QStringLiteral("Source Han Sans CN"), QStringLiteral("WenQuanYi Micro Hei")};
    } else if (language == QLatin1String("zh_TW")) {
        families = {QStringLiteral("Microsoft JhengHei UI"), QStringLiteral("Microsoft JhengHei"),
                    QStringLiteral("Noto Sans CJK TC"), QStringLiteral("Source Han Sans TC"),
                    QStringLiteral("Source Han Sans TW")};
    } else if (language == QLatin1String("ja")) {
        families = {QStringLiteral("Yu Gothic UI"), QStringLiteral("Yu Gothic"),
                    QStringLiteral("Meiryo"), QStringLiteral("Noto Sans CJK JP"),
                    QStringLiteral("Source Han Sans JP")};
        scripts.append(QChar::Script_Hiragana);
        scripts.append(QChar::Script_Katakana);
    } else if (language == QLatin1String("ko")) {
        families = {QStringLiteral("Malgun Gothic"), QStringLiteral("Noto Sans CJK KR"),
                    QStringLiteral("Source Han Sans KR")};
        scripts.append(QChar::Script_Hangul);
    } else if (language == QLatin1String("ar")) {
        families = {QStringLiteral("Noto Sans Arabic"), QStringLiteral("Tahoma")};
        scripts.clear();
    } else if (language == QLatin1String("th")) {
        families = {QStringLiteral("Noto Sans Thai"), QStringLiteral("Leelawadee UI"),
                    QStringLiteral("Tahoma")};
        scripts.clear();
    } else {
        QGuiApplication::setFont(font);
        return;
    }
    const QStringList installed = QFontDatabase::families();
    for (const QString &family : families) {
        if (installed.contains(family)) {
            for (QChar::Script script : scripts)
                QFontDatabase::addApplicationFallbackFontFamily(script, family);
            added = family;
            addedScripts = scripts;
            // Standard controls often size themselves from the primary font's
            // height. Using the script font prevents taller fallback text from
            // being clipped while retaining the user's size and weight.
            selectedFamilies = {family};
            font.setFamilies(selectedFamilies);
            QGuiApplication::setFont(font);
            return;
        }
    }
    QGuiApplication::setFont(font);
}

// Qt's own Swedish catalog writes the standard OK button as "Ok", in
// QPlatformTheme, QDialogButtonBox and other contexts. Swedish Windows and
// macOS write "OK", so while the UI is in Swedish this translator keeps Qt's
// English text for that button. It must be installed after the catalog,
// because Qt asks the most recently installed translator first.
class SwedishOkTranslator final : public QTranslator
{
public:
    using QTranslator::QTranslator;

    QString translate(const char *, const char *sourceText, const char *, int) const override
    {
        if (qstrcmp(sourceText, "OK") == 0 || qstrcmp(sourceText, "&OK") == 0)
            return QString::fromLatin1(sourceText);
        return {};
    }

    // Qt sends no LanguageChange when it installs an empty translator, and this
    // one loads no catalog. Reporting it as not empty makes the install
    // retranslate the open windows.
    bool isEmpty() const override { return false; }
};

} // namespace

QStringList availableLanguages()
{
    QStringList others;
    const QStringList files = QDir(QStringLiteral(":/i18n"))
                                  .entryList({QStringLiteral("mervin_*.qm")}, QDir::Files);
    for (const QString &file : files) {
        const QString code = file.mid(7, file.size() - 7 - 3); // mervin_<code>.qm
        if (code != QLatin1String(kEnglish))
            others.append(code);
    }
    std::sort(others.begin(), others.end());
    return QStringList{QString::fromLatin1(kEnglish)} + others;
}

QString normalized(const QString &code, const QStringList &available)
{
    QString wanted = code.trimmed();
    wanted.replace(QLatin1Char('-'), QLatin1Char('_'));
    for (const QString &candidate : available)
        if (candidate.compare(wanted, Qt::CaseInsensitive) == 0)
            return candidate;
    return {};
}

QString suggestedLanguage(const QStringList &osLanguages, const QStringList &available)
{
    // The first tag of each language decides its script. A later tag of that
    // language in another script is a fallback that Qt added. Qt ends a Taiwan
    // list with a bare "zh", and turns the "zh" in Debian's and Ubuntu's
    // LANGUAGE=zh_TW:zh into zh-Hans-CN. Both mean Simplified Chinese.
    QHash<QLocale::Language, QLocale::Script> scripts;
    bool sawMontenegrin = false;
    bool montenegrinLatin = false;
    for (const QString &tag : osLanguages) {
        // Qt has no Montenegrin locale. The catalog uses Latin Montenegrin;
        // an explicit Cyrillic preference must not select the Latin catalog.
        const QStringList parts = tag.toLower().replace(QLatin1Char('_'), QLatin1Char('-'))
                                      .split(QLatin1Char('-'));
        if (parts.first() == QLatin1String("cnr")) {
            if (!sawMontenegrin) {
                montenegrinLatin = !parts.contains(QStringLiteral("cyrl"));
                sawMontenegrin = true;
            }
            if (montenegrinLatin && available.contains(QStringLiteral("cnr")))
                return QStringLiteral("cnr");
            continue;
        }
        const QLocale os(tag);
        if (os.language() == QLocale::C || os.language() == QLocale::AnyLanguage)
            continue;
        const auto first = scripts.constFind(os.language());
        if (first == scripts.cend())
            scripts.insert(os.language(), os.script());
        else if (*first != os.script())
            continue;
        QString best;
        bool sameTerritory = false;
        for (const QString &code : available) {
            const QLocale catalog(code);
            if (catalog.language() != os.language() || catalog.script() != os.script())
                continue;
            const bool territory = catalog.territory() == os.territory();
            if (best.isEmpty() || (territory && !sameTerritory)) {
                best = code;
                sameTerritory = territory;
            }
        }
        if (!best.isEmpty())
            return best;
    }
    return QString::fromLatin1(kEnglish);
}

QString suggestedLanguage()
{
    return suggestedLanguage(QLocale::system().uiLanguages(), availableLanguages());
}

void apply(const QString &code)
{
    QString language = normalized(code, availableLanguages());
    if (language.isEmpty())
        language = QString::fromLatin1(kEnglish);

    // Install standard widget text first so the app can override it. Both
    // catalogs live in resources; no translation files are needed at runtime.
    static QPointer<QTranslator> qtInstalled;
    static QPointer<QTranslator> qtSupplement;
    static QPointer<QTranslator> installed;
    static QPointer<QTranslator> okOverride;
    for (QTranslator *old : {okOverride.data(), installed.data(), qtSupplement.data(), qtInstalled.data()}) {
        if (old) {
            QCoreApplication::removeTranslator(old);
            delete old;
        }
    }
    const auto install = [&](const QString &base, QPointer<QTranslator> &target) {
        auto *translator = new QTranslator(QCoreApplication::instance());
        if (translator->load(QStringLiteral(":/i18n/%1_%2.qm").arg(base, language))) {
            QCoreApplication::installTranslator(translator);
            target = translator;
        } else {
            delete translator;
        }
    };
    install(QStringLiteral("qtbase"), qtInstalled);
    install(QStringLiteral("qtbase_supplement"), qtSupplement);
    install(QStringLiteral("mervin"), installed);
    if (language == QLatin1String("sv")) {
        okOverride = new SwedishOkTranslator(QCoreApplication::instance());
        QCoreApplication::installTranslator(okOverride);
    }
    currentLanguage() = language;

    if (qobject_cast<QGuiApplication *>(QCoreApplication::instance())) {
        QGuiApplication::setLayoutDirection(QLocale(language).textDirection());
        preferLanguageFont(language);
    }
}

QString current()
{
    return currentLanguage();
}

namespace {
bool g_oneRunOverride = false;
} // namespace

void setOneRunOverride(bool on)
{
    g_oneRunOverride = on;
}

bool oneRunOverride()
{
    return g_oneRunOverride;
}

QString displayName(const QString &code)
{
    const QString native = nativeName(code);
    const QString english = englishName(code);
    return native == english ? native : native + QStringLiteral(" (") + english + QLatin1Char(')');
}

QString searchText(const QString &code)
{
    const QString english = englishName(code);
    const QByteArray source = english.toLatin1();
    const QString local = QCoreApplication::translate("UiLanguage", source.constData());
    return QStringList{nativeName(code), english, local, code}.join(QLatin1Char(' '));
}

} // namespace mervin::i18n
