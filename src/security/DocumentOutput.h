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
    // Source remains available for recovery if replacement fails. Holds the
    // destination's job::DestinationLock throughout and ticks job::progressed() per
    // 1 MiB copied. Inside a job::Scope, a stopped job gives up at its next chunk and
    // never replaces the destination.
    static bool replace(const QString &source, const QString &destination, QString *error);
};
} // namespace mervin
