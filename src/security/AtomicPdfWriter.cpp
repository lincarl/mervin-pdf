#include "security/AtomicPdfWriter.h"

#include <QFileInfo>

#include <algorithm>
#include <limits>
#include <stdexcept>

namespace mervin {

/// Acquires the destination before opening a sibling temporary PDF.
AtomicPdfWriter::AtomicPdfWriter(QPDF &pdf, const QString &path)
    : destination_(path), file_(path), sink_(file_, QFileInfo(path).absolutePath()), writer_(pdf)
{
    // A test can hold the write step here, while this writer owns its destination.
    if (!destination_.owns() || !job::writing(QFileInfo(path).absolutePath()))
        throw std::runtime_error(job::notRunError().toStdString());
    file_.setDirectWriteFallback(false);
    if (!file_.open(QIODevice::WriteOnly))
        throw std::runtime_error(file_.errorString().toStdString());
    writer_.setOutputPipeline(&sink_);
    writer_.setStaticID(false);
}

/// Streams qpdf output with Stop checks and progress reports.
void AtomicPdfWriter::Sink::write(const unsigned char *data, size_t length)
{
    // qpdf reads its source between the pieces it writes.
    job::switchTo(job::Step::Writing, folder_);
    if (job::stopped())
        throw std::runtime_error(job::notRunError().toStdString());
    while (length) {
        const auto chunk = static_cast<qint64>(std::min<size_t>(length,
            std::numeric_limits<qint64>::max()));
        const qint64 written = file_.write(reinterpret_cast<const char *>(data), chunk);
        if (written <= 0)
            throw std::runtime_error(file_.errorString().toStdString());
        data += written;
        length -= static_cast<size_t>(written);
    }
    job::progressed();
}

/// Validates replacement while this writer still owns the destination, before commit admission.
void AtomicPdfWriter::write(const std::function<bool()> &mayCommit)
{
    writer_.write();
    // Writes out Qt's buffer while a stop can still keep the destination as it was.
    // The commit then syncs the temporary file to disk and renames it.
    if (!file_.flush())
        throw std::runtime_error(file_.errorString().toStdString());
    if ((mayCommit && !mayCommit()) || !job::beginCommit(file_.fileName())) {
        file_.cancelWriting(); // the destructor discards the temporary file
        throw std::runtime_error(job::notRunError().toStdString());
    }
    const bool committed = file_.commit();
    job::endCommit();
    if (!committed)
        throw std::runtime_error(file_.errorString().toStdString());
}

} // namespace mervin
