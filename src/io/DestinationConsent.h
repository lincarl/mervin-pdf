#pragma once

#include <QByteArray>
#include <QString>

#include <optional>

namespace mervin {

/// Records the destination contents and resolved target approved for replacement.
struct DestinationConsent
{
    QString key;
    bool exists = false;
    QByteArray sha256;

    /// Reads absence or hashes an existing regular file on a worker, failing closed on errors.
    static std::optional<DestinationConsent> read(const QString &path, QString *error = nullptr);
    bool operator==(const DestinationConsent &) const = default;
};

} // namespace mervin
