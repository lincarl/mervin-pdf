#include "security/AtomicPdfWriter.h"

#include <algorithm>
#include <limits>
#include <stdexcept>

namespace mervin {

AtomicPdfWriter::AtomicPdfWriter(QPDF &pdf, const QString &path)
    : file_(path), sink_(file_), writer_(pdf)
{
    file_.setDirectWriteFallback(false);
    if (!file_.open(QIODevice::WriteOnly))
        throw std::runtime_error(file_.errorString().toStdString());
    writer_.setOutputPipeline(&sink_);
    writer_.setStaticID(false);
}

void AtomicPdfWriter::Sink::write(const unsigned char *data, size_t length)
{
    while (length) {
        const auto chunk = static_cast<qint64>(std::min<size_t>(length,
            std::numeric_limits<qint64>::max()));
        const qint64 written = file_.write(reinterpret_cast<const char *>(data), chunk);
        if (written <= 0)
            throw std::runtime_error(file_.errorString().toStdString());
        data += written;
        length -= static_cast<size_t>(written);
    }
}

void AtomicPdfWriter::write()
{
    writer_.write();
    if (!file_.commit())
        throw std::runtime_error(file_.errorString().toStdString());
}

} // namespace mervin
