#include "security/DocumentOutput.h"

#include "render/Document.h"
#include "security/MeasureExport.h"

#include <QCoreApplication>
#include <QFile>
#include <QSaveFile>
#include <QTemporaryDir>

namespace mervin {
bool DocumentOutput::snapshot(const Document &document, const MeasureDoc &measurements,
                              const QString &destination, const QString &password, QString *error)
{
    QTemporaryDir stage;
    if (!stage.isValid()) {
        if (error)
            *error = QCoreApplication::translate("mervin::DocumentOutput",
                                                 "Could not create a temporary document.");
        return false;
    }
    const QString live = stage.filePath(QStringLiteral("live.pdf"));
    return document.savePdfTo(live, error)
        && MeasureExport::embedMervin(live, destination, measurements, password, error)
               == MeasureExport::Status::Ok;
}

bool DocumentOutput::replace(const QString &source, const QString &destination, QString *error)
{
    QFile input(source);
    QSaveFile output(destination);
    output.setDirectWriteFallback(false);
    const auto fail = [&](const QString &message) {
        if (error)
            *error = message;
        return false;
    };
    if (!input.open(QIODevice::ReadOnly))
        return fail(input.errorString());
    if (!output.open(QIODevice::WriteOnly))
        return fail(output.errorString());
    QByteArray buffer(1024 * 1024, Qt::Uninitialized);
    for (;;) {
        const qint64 count = input.read(buffer.data(), buffer.size());
        if (count < 0)
            return fail(input.errorString());
        if (count == 0)
            break;
        if (output.write(buffer.constData(), count) != count)
            return fail(output.errorString());
    }
    return output.commit() || fail(output.errorString());
}
} // namespace mervin
