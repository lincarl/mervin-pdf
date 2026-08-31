#include "net/UrlOpen.h"

#include <QCryptographicHash>
#include <QFileInfo>

namespace mervin::urlopen {

std::optional<QUrl> fromUserInput(const QString &input)
{
    QString repaired = input.trimmed();
    const int colon = repaired.indexOf(u':');
    if (colon <= 0)
        return std::nullopt;

    const QString scheme = repaired.left(colon).toLower();
    if (scheme != QStringLiteral("http") && scheme != QStringLiteral("https"))
        return std::nullopt;

    QString remainder = repaired.mid(colon + 1);
    remainder.replace(u'\\', u'/');
    while (remainder.startsWith(u'/'))
        remainder.remove(0, 1);

    const QUrl url(scheme + QStringLiteral("://") + remainder, QUrl::TolerantMode);
    if (!url.isValid() || url.host().isEmpty())
        return std::nullopt;
    return url;
}

QNetworkRequest makeRequest(const QUrl &url)
{
    QUrl requestUrl = url;
    // Analog Devices' retired HTTP endpoint accepts connections without ever
    // replying. Browsers recover through their HTTPS-first/CDN machinery; use
    // the vendor's current document route directly.
    if (requestUrl.host().compare(QStringLiteral("www.analog.com"), Qt::CaseInsensitive) == 0
        && requestUrl.path().startsWith(
            QStringLiteral("/static/imported-files/data_sheets/"))) {
        requestUrl.setScheme(QStringLiteral("https"));
        requestUrl.setPath(QStringLiteral("/media/en/technical-documentation/data-sheets/")
                           + requestUrl.fileName().toLower());
    }

    QNetworkRequest request(requestUrl);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::NoLessSafeRedirectPolicy);
    request.setTransferTimeout(30000);
    // Some document CDNs (notably Analog Devices' Akamai edge) reject Qt's
    // HTTP/2 request with INTERNAL_ERROR unless it looks like a complete
    // browser navigation.  A browser User-Agent by itself is not sufficient;
    // keep the matching client hints and navigation header together.
    request.setRawHeader(
        "User-Agent",
        QByteArrayLiteral("Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 "
                          "(KHTML, like Gecko) HeadlessChrome/151.0.0.0 Safari/537.36"));
    request.setRawHeader("Accept-Language", QByteArrayLiteral("en-US,en;q=0.9"));
    request.setRawHeader("Upgrade-Insecure-Requests", QByteArrayLiteral("1"));
    request.setRawHeader("sec-ch-ua",
                         QByteArrayLiteral("\"Chromium\";v=\"151\", "
                                           "\"Not=A?Brand\";v=\"99\""));
    request.setRawHeader("sec-ch-ua-mobile", QByteArrayLiteral("?0"));
    request.setRawHeader("sec-ch-ua-platform", QByteArrayLiteral("\"Linux\""));
    request.setRawHeader("Accept",
                         QByteArrayLiteral("application/pdf,application/octet-stream;q=0.9,*/*;q=0.8"));
    return request;
}

QString suggestedFileName(const QUrl &url)
{
    const QString fileName = QFileInfo(url.path()).fileName();
    return fileName.isEmpty() ? QStringLiteral("download.pdf") : fileName;
}

QString cachedFileName(const QUrl &url)
{
    const QFileInfo nameInfo(suggestedFileName(url));
    QString safeBase = nameInfo.completeBaseName();
    for (QChar &ch : safeBase) {
        if (!ch.isLetterOrNumber() && ch != u'-' && ch != u'_' && ch != u'.')
            ch = u'_';
    }
    if (safeBase.isEmpty())
        safeBase = QStringLiteral("download");

    QString safeSuffix = nameInfo.suffix();
    for (QChar &ch : safeSuffix) {
        if (!ch.isLetterOrNumber())
            ch = u'_';
    }
    if (safeSuffix.isEmpty())
        safeSuffix = QStringLiteral("pdf");

    const QString hash = QString::fromLatin1(
        QCryptographicHash::hash(url.toEncoded(), QCryptographicHash::Sha256).toHex().left(12));
    return QStringLiteral("%1-%2.%3").arg(safeBase, hash, safeSuffix);
}

} // namespace mervin::urlopen
