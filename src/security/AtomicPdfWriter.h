#pragma once

#include "io/JobContext.h"

#include <QSaveFile>
#include <qpdf/Pipeline.hh>
#include <qpdf/QPDFWriter.hh>

namespace mervin {

// Streams to a sibling temporary file; only a complete PDF replaces the destination.
// Destruction or any write/commit exception leaves the previous file intact. Inside a
// job::Scope, a stopped job gives up at its next write and never replaces the file.
class AtomicPdfWriter
{
public:
    // Takes the destination's job::DestinationLock before it opens the temporary
    // file, so another writer of the same file waits until this one is destroyed.
    // Throws when the job stopped first.
    AtomicPdfWriter(QPDF &pdf, const QString &path);
    QPDFWriter &options() { return writer_; }
    // Replaces the destination only after job::beginCommit() agreed.
    void write();

private:
    class Sink final : public Pipeline
    {
    public:
        explicit Sink(QSaveFile &file) : Pipeline("atomic PDF output", nullptr), file_(file) {}
        // Ticks job::progressed() once per call.
        void write(const unsigned char *data, size_t length) override;
        void finish() override {}
    private:
        QSaveFile &file_;
    };

    job::DestinationLock destination_; // first, so it outlives the temporary file
    QSaveFile file_;
    Sink sink_;
    QPDFWriter writer_;
};

} // namespace mervin
