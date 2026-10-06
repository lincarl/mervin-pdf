#pragma once

#include <QSaveFile>
#include <qpdf/Pipeline.hh>
#include <qpdf/QPDFWriter.hh>

namespace mervin {

// Streams to a sibling temporary file; only a complete PDF replaces the destination.
// Destruction or any write/commit exception leaves the previous file intact.
class AtomicPdfWriter
{
public:
    AtomicPdfWriter(QPDF &pdf, const QString &path);
    QPDFWriter &options() { return writer_; }
    void write();

private:
    class Sink final : public Pipeline
    {
    public:
        explicit Sink(QSaveFile &file) : Pipeline("atomic PDF output", nullptr), file_(file) {}
        void write(const unsigned char *data, size_t length) override;
        void finish() override {}
    private:
        QSaveFile &file_;
    };

    QSaveFile file_;
    Sink sink_;
    QPDFWriter writer_;
};

} // namespace mervin
