#include "update/UpdatePlan.h"

#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>

namespace mervin::update {

bool automaticCheckDue(const QDateTime &lastCheck, const QDateTime &now)
{
    return !lastCheck.isValid() || lastCheck > now
        || lastCheck.toUTC().addDays(kDaysPerCheck) <= now;
}

QVersionNumber parseVersion(QString text)
{
    text = text.trimmed();
    if (text.startsWith(QLatin1Char('v')) || text.startsWith(QLatin1Char('V')))
        text.remove(0, 1);
    qsizetype suffix = 0;
    const QVersionNumber version = QVersionNumber::fromString(text, &suffix);
    return suffix == text.size() && version.segmentCount() == 3 ? version : QVersionNumber{};
}

bool isNewerStableVersion(const QVersionNumber &candidate, QString installedVersion)
{
    static const QRegularExpression pattern(
        QStringLiteral("^([vV]?[0-9]+\\.[0-9]+\\.[0-9]+)(-rc[1-9][0-9]*)?$"));
    const QRegularExpressionMatch match = pattern.match(installedVersion.trimmed());
    if (candidate.isNull() || !match.hasMatch())
        return false;
    const QVersionNumber installed = parseVersion(match.captured(1));
    if (installed.isNull())
        return false;
    const int comparison = QVersionNumber::compare(candidate, installed);
    return comparison > 0 || (comparison == 0 && !match.captured(2).isEmpty());
}

std::optional<Release> parseRelease(const QByteArray &json)
{
    const QJsonObject root = QJsonDocument::fromJson(json).object();
    if (root.value(QStringLiteral("draft")).toBool()
        || root.value(QStringLiteral("prerelease")).toBool())
        return std::nullopt;
    Release r;
    r.tag = root.value(QStringLiteral("tag_name")).toString();
    r.version = parseVersion(r.tag);
    if (r.version.isNull())
        return std::nullopt;
    r.pageUrl = root.value(QStringLiteral("html_url")).toString();

    const QString digestPrefix = QStringLiteral("sha256:");
    for (const QJsonValue &v : root.value(QStringLiteral("assets")).toArray()) {
        const QJsonObject a = v.toObject();
        ReleaseAsset asset;
        asset.name = a.value(QStringLiteral("name")).toString();
        asset.url = a.value(QStringLiteral("browser_download_url")).toString();
        const QString digest = a.value(QStringLiteral("digest")).toString();
        if (digest.startsWith(digestPrefix))
            asset.sha256 = digest.mid(digestPrefix.size()).toLower();
        if (!asset.name.isEmpty() && !asset.url.isEmpty())
            r.assets.append(asset);
    }
    return r;
}

std::optional<ReleaseAsset> assetFor(const QList<ReleaseAsset> &assets, PackageKind kind,
                                     const QString &osVersion)
{
    // Asset names as release.yml publishes them.
    auto named = [](const ReleaseAsset &a, QLatin1StringView prefix, QLatin1StringView suffix) {
        return a.name.startsWith(prefix, Qt::CaseInsensitive)
            && a.name.endsWith(suffix, Qt::CaseInsensitive);
    };
    QLatin1StringView prefix;
    QLatin1StringView suffix;
    switch (kind) {
    case PackageKind::None:
        return std::nullopt;
    case PackageKind::Msi: // MervinPDF-1.64.0.msi
        prefix = QLatin1StringView("MervinPDF-");
        suffix = QLatin1StringView(".msi");
        break;
    case PackageKind::AppImage: // MervinPDF-1.64.0-x86_64.AppImage
        prefix = QLatin1StringView("MervinPDF-");
        suffix = QLatin1StringView("-x86_64.AppImage");
        break;
    case PackageKind::Deb: // mervin-pdf_1.64.0_ubuntu26.04_amd64.deb
        prefix = QLatin1StringView("mervin-pdf_");
        suffix = QLatin1StringView("_amd64.deb");
        break;
    case PackageKind::Rpm: // mervin-pdf-1.64.0.x86_64.rpm
        prefix = QLatin1StringView("mervin-pdf-");
        suffix = QLatin1StringView(".x86_64.rpm");
        break;
    }

    std::optional<ReleaseAsset> match;
    for (const ReleaseAsset &a : assets) {
        if (!named(a, prefix, suffix) || a.sha256.isEmpty() || a.name.contains(QLatin1Char('/'))
            || a.name.contains(QLatin1Char('\\')))
            continue;
        if (kind == PackageKind::Deb && !osVersion.isEmpty()
            && a.name.contains(QStringLiteral("_ubuntu%1_").arg(osVersion), Qt::CaseInsensitive))
            return a; // built for exactly this release
        if (!match)
            match = a;
    }
    return match;
}

PackageKind windowsPackageKind(const QString &exeDir, const QString &registeredInstallDir)
{
    if (exeDir.isEmpty() || registeredInstallDir.isEmpty())
        return PackageKind::None;
    // Explicit '\\' -> '/' rather than fromNativeSeparators, which is a no-op off
    // Windows: the rule must hold wherever it runs, including the Linux test run.
    const auto clean = [](QString p) {
        return QDir::cleanPath(p.replace(QLatin1Char('\\'), QLatin1Char('/')));
    };
    if (clean(exeDir).compare(clean(registeredInstallDir), Qt::CaseInsensitive) != 0)
        return PackageKind::None;
    return PackageKind::Msi;
}

PackageKind linuxPackageKind(const QString &exePath, const QString &appImage,
                             const QString &appDir, QStringView buildFormat)
{
    if (!appImage.isEmpty() && !appDir.isEmpty()
        && exePath.startsWith(QDir::cleanPath(appDir) + QLatin1Char('/')))
        return PackageKind::AppImage;
    if (!exePath.startsWith(QLatin1String("/usr/")))
        return PackageKind::None; // a build tree, even when configured as a package
    if (buildFormat == u"deb")
        return PackageKind::Deb;
    if (buildFormat == u"rpm")
        return PackageKind::Rpm;
    return PackageKind::None;
}

} // namespace mervin::update
