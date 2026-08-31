#pragma once

#include <QNetworkRequest>
#include <QString>
#include <QUrl>

#include <optional>

namespace mervin::urlopen {

// Parse an HTTP(S) URL entered either directly or through a native Windows
// file picker. The latter may turn its slashes into path separators before
// returning the text to the application.
std::optional<QUrl> fromUserInput(const QString &input);

// Build a request suitable for public document servers. Some CDNs reject
// Qt's empty default User-Agent, so identify both the download client family
// and Mervin itself.
QNetworkRequest makeRequest(const QUrl &url);

QString suggestedFileName(const QUrl &url);
QString cachedFileName(const QUrl &url);

} // namespace mervin::urlopen
