#pragma once

#include <QString>

namespace mervin {
class Document;
struct MeasureDoc;

class DocumentOutput
{
public:
    // Write live forms, annotations and the complete measurement state, including deletions.
    static bool snapshot(const Document &document, const MeasureDoc &measurements,
                         const QString &destination, const QString &password, QString *error);
    // Source remains available for recovery if replacement fails.
    static bool replace(const QString &source, const QString &destination, QString *error);
};
} // namespace mervin
