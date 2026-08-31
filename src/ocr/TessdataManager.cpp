#include "ocr/TessdataManager.h"

#include "config/ConfigPaths.h"

#include <QCoreApplication>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QLocale>
#include <QObject>
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
    static const QHash<QString, QString> names{
        {QStringLiteral("chi_sim"), QObject::tr("Chinese (Simplified)")},
        {QStringLiteral("chi_tra"), QObject::tr("Chinese (Traditional)")},
        {QStringLiteral("deu_frak"), QObject::tr("German Fraktur")},
        {QStringLiteral("equ"), QObject::tr("Math / equation detection")},
        {QStringLiteral("osd"), QObject::tr("Orientation and script detection")},
    };
    if (const auto it = names.constFind(code); it != names.cend())
        return *it;
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
