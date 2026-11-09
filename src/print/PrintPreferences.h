#pragma once

#include "print/PrintLayout.h"

#include <QJsonObject>
#include <QPageLayout>
#include <QSizeF>
#include <QString>

namespace mervin::printing {

enum class PageSelection { All, Current, Range, Custom };

// Per-file dialog choices saved after Print, independent of automatic preview scale.
struct Preferences
{
    QString printerName;
    bool printToFile = false;
    QString paperKey;
    QSizeF paperPoints;
    QPageLayout::Orientation orientation = QPageLayout::Portrait;
    PageSelection pageSelection = PageSelection::All;
    int fromPage = 1;
    int toPage = 1;
    QString customPages;
    int copies = 1;
    Settings settings;
    bool twoSided = false;

    bool operator==(const Preferences &) const = default;

    // Stores stable identifiers and physical paper dimensions, without an output filename.
    QJsonObject toJson() const;
    // Keeps defaults for malformed fields. Document and printer constraints apply on restore.
    static Preferences fromJson(const QJsonObject &object);
};

} // namespace mervin::printing
