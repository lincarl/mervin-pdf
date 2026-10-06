#pragma once

#include <QString>
#include <QStringList>

// The UI text language: which translation catalogs ship, which one suits the
// user's OS, and installing one. A language is identified by its catalog ID,
// the suffix of its .qm file ("en", "sv", "zh_CN"); Qt's own catalogs use the
// same IDs, which is what lets the build merge them into ours. The catalogs
// are compiled into the binary under :/i18n (see qt_add_translations in
// CMakeLists.txt). English is the source language: its catalog only holds
// plural forms.
namespace mervin::i18n {

inline constexpr char kEnglish[] = "en";

// Catalog IDs of every language this build can show, English first.
QStringList availableLanguages();

// `code` as one of `available`, or empty when it isn't one. Case and the
// separator don't matter ("zh-cn" gives "zh_CN").
QString normalized(const QString &code, const QStringList &available);

// The first language in `osLanguages` (most preferred first, as
// QLocale::uiLanguages() lists them) that one of `available` can show:
// same language and script, preferring the same territory. A Chinese region
// matches the catalog with its script (zh-HK gives zh_TW), and a regional
// variant matches the base catalog (de-AT gives de). English when nothing
// matches.
QString suggestedLanguage(const QStringList &osLanguages, const QStringList &available);

// suggestedLanguage() for this user's OS languages and this build.
QString suggestedLanguage();

// Show the UI in `code` from now on: replaces the installed catalog, which
// makes Qt send LanguageChange to every widget, and sets the layout direction.
// Unknown codes fall back to English. Needs a QCoreApplication.
void apply(const QString &code);

// The language the last apply() installed, English before any.
QString current();

// "Svenska (Swedish)": the name in its own language, then in English.
QString displayName(const QString &code);

// Text the language picker's search matches: both names above, the code, and
// the name in the current UI language ("kinesiska" in Swedish).
QString searchText(const QString &code);

} // namespace mervin::i18n
