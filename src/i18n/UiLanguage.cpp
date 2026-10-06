#include "i18n/UiLanguage.h"

#include <QCoreApplication>
#include <QDir>
#include <QFontDatabase>
#include <QGuiApplication>
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
    {"sv", "Svenska", QT_TRANSLATE_NOOP("UiLanguage", "Swedish")},
    //: Language name. The language picker's search finds a language by this name too.
    {"zh_CN", "简体中文", QT_TRANSLATE_NOOP("UiLanguage", "Chinese, Simplified")},
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

// Chinese text falls back to whichever CJK font the OS prefers for its own
// locale, which on a non-Chinese system can be a Japanese one with different
// glyph shapes. While the UI is in Simplified Chinese, prefer a Simplified
// Chinese font that exists on this system.
void preferChineseFont(bool simplified)
{
    static const char *const kFamilies[] = {"Microsoft YaHei UI", "Microsoft YaHei",
                                            "Noto Sans CJK SC", "Source Han Sans SC",
                                            "Source Han Sans CN", "WenQuanYi Micro Hei"};
    static QString added;
    if (!added.isEmpty()) {
        QFontDatabase::removeApplicationFallbackFontFamily(QChar::Script_Han, added);
        added.clear();
    }
    if (!simplified)
        return;
    const QStringList installed = QFontDatabase::families();
    for (const char *family : kFamilies) {
        const QString name = QString::fromLatin1(family);
        if (installed.contains(name)) {
            QFontDatabase::addApplicationFallbackFontFamily(QChar::Script_Han, name);
            added = name;
            return;
        }
    }
}

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
    for (const QString &tag : osLanguages) {
        const QLocale os(tag);
        if (os.language() == QLocale::C || os.language() == QLocale::AnyLanguage)
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

    // One catalog per language: the build merges Qt's own strings (standard
    // buttons, file and print dialogs) into it.
    static QPointer<QTranslator> installed;
    if (installed) {
        QCoreApplication::removeTranslator(installed);
        delete installed;
    }
    auto *translator = new QTranslator(QCoreApplication::instance());
    if (translator->load(QStringLiteral(":/i18n/mervin_%1.qm").arg(language))) {
        QCoreApplication::installTranslator(translator);
        installed = translator;
    } else {
        delete translator;
    }
    currentLanguage() = language;

    if (qobject_cast<QGuiApplication *>(QCoreApplication::instance())) {
        QGuiApplication::setLayoutDirection(QLocale(language).textDirection());
        preferChineseFont(language == QLatin1String("zh_CN"));
    }
}

QString current()
{
    return currentLanguage();
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
