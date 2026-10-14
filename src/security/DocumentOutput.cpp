#include "security/DocumentOutput.h"

#include "io/JobContext.h"
#include "render/Document.h"
#include "security/MeasureExport.h"

#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>
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
    const auto fail = [&](const QString &message) {
        if (error)
            *error = message;
        return false;
    };
    // Taken before the temporary file opens and kept until the replace returned.
    job::DestinationLock lock(destination);
    if (!lock.owns() || !job::writing(QFileInfo(destination).absolutePath()))
        return fail(job::notRunError());
    QFile input(source);
    QSaveFile output(destination);
    output.setDirectWriteFallback(false);
    if (!input.open(QIODevice::ReadOnly))
        return fail(input.errorString());
    if (!output.open(QIODevice::WriteOnly))
        return fail(output.errorString());
    QByteArray buffer(1024 * 1024, Qt::Uninitialized);
    for (;;) {
        if (job::stopped())
            return fail(job::notRunError());
        const qint64 count = input.read(buffer.data(), buffer.size());
        if (count < 0)
            return fail(input.errorString());
        if (count == 0)
            break;
        if (output.write(buffer.constData(), count) != count)
            return fail(output.errorString());
        job::progressed();
    }
    // Writes out Qt's buffer while a stop can still keep the destination as it was.
    // The commit then syncs the temporary file to disk and renames it.
    if (!output.flush())
        return fail(output.errorString());
    if (!job::beginCommit(destination)) {
        output.cancelWriting(); // the destructor discards the temporary file
        return fail(job::notRunError());
    }
    const bool committed = output.commit();
    job::endCommit();
    return committed || fail(output.errorString());
}
} // namespace mervin
